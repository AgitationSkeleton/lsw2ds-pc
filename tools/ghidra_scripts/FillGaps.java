// Turn undefined bytes inside executable memory into disassembled functions,
// so the decompile pass actually covers the image.
//
// WHY. A plain headless import + auto-analysis leaves large stretches of the
// ARM9 unclaimed - code nothing referenced, or that a data island or an
// unfollowable PC-relative switch broke the flow out of. Those gaps are not
// empty: several hold routines this project has already disassembled by hand.
//
// ONE PASS, PROLOGUE ANCHORED. Every 4-aligned undefined word is tested against
// the ARM function-entry idiom:
//
//     E92D4xxx    stmfd sp!, {..., lr}      (push with lr in the register list)
//     E52DE004    str   lr, [sp, #-4]!      (single push)
//
// and a function is created there. This is the pass that works, because it
// starts at a real entry rather than wherever the gap happens to begin.
//
// WHY NOT DISASSEMBLE THE GAPS DIRECTLY. The first version of this script did
// exactly that, and it left 15,552 bytes at 0x0203C178 unclaimed - a region that
// opens with ONE literal-pool word and then a genuine
// stmfd sp!,{r3-r7,sb,sl,fp,lr} at 0x0203C17C. Disassembling from the run start
// fed that literal to the decoder, produced garbage, and the run was abandoned.
// A gap beginning with a literal pool word poisoned everything after it.
// Anchoring on the push avoids that, and it is the rule this project already
// applies by hand: A FUNCTION'S ENTRY IS ITS PUSH.
//
// A direct scan of the image found 195 such prologues sitting in bytes no
// function covered, every one of them below the end of the code region -
// including 0x0205ACA0, the asset loader, and 0x0203C17C above.
//
// WHY THERE IS NO LONGER A RUN SCAN. An earlier version added a second pass
// that disassembled any run of undefined bytes from its first 4-aligned
// address. It created 455 functions and AT LEAST 277 OF THEM WERE DATA - whole
// regions above the end of the code claimed as code, including bytes belonging
// to the HUD table at 0x020EDE22 and the weapon table at 0x020EB7BC. Exactly
// one of those 277 was ever the target of a call. That pass was guessing at
// data, which is the thing this script must not do, so it is gone.
//
// The prologue rule is safe by construction, and measurably so: scanning the
// whole image for E92D4xxx words whose register list is matched by a later
// ldmfd sp!,{same,pc} finds 3,226 of them below 0x020E5C74 and NOT ONE above
// it. Real prologues simply do not occur in the data region, so anchoring on
// them cannot manufacture a function there.
//
// THE PROLOGUE RULE HAS A STRUCTURAL BLIND SPOT, and a second anchor now
// covers it. A LEAF FUNCTION THAT PUSHES NOTHING HAS NO PROLOGUE: it computes,
// returns with `bx lr`, and this scan cannot see it. Enumerating every BL in
// the image and subtracting the corpus found exactly eleven such functions,
// two of which - 0x01FFA478, called ten times, and 0x01FFD538, called four -
// are the only unresolved call targets in the entire frame closure.
//
// A BL TARGET IS A FUNCTION ENTRY BY DEFINITION, which makes it a stronger
// anchor than a prologue rather than a weaker one: the prologue rule pattern-
// matches bytes and could in principle match data, whereas a BL target is
// DECODED from an instruction that is itself reachable. The second pass below
// therefore walks every 4-aligned word, decodes the BL displacement, and
// creates a function at the target if nothing covers it.
//
// It cannot manufacture a function in the data region for the same reason the
// prologue pass cannot: it only follows branches that already exist, and
// tools/find_call_targets.py reports zero of the 3,193 BL targets landing
// inside an existing function at a non-entry offset - so Ghidra's boundaries
// and this decode agree everywhere they overlap.
//
// The pass repeats until it finds nothing new, because creating one function
// lets Ghidra's flow-following reach others.
//
// Run it BEFORE DecompileAll.java.
//
// @category LSW2

import ghidra.app.cmd.disassemble.ArmDisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

public class FillGaps extends GhidraScript {

    // How many prologue rounds before giving up on finding more.
    private static final int MAX_ROUNDS = 4;

    private boolean isUndefined(Address at) {
        CodeUnit cu = currentProgram.getListing().getCodeUnitAt(at);
        return cu != null && (cu instanceof Data) && !((Data) cu).isDefined();
    }

    /** stmfd sp!,{...,lr} or str lr,[sp,#-4]! - an ARM function entry. */
    private boolean isPrologue(int word) {
        if ((word & 0xFFFF0000) == 0xE92D0000 && (word & 0x4000) != 0) {
            return true;
        }
        return word == 0xE52DE004;
    }

    private int prologuePass(MemoryBlock block) throws Exception {
        int created = 0;
        Address at = block.getStart();
        while ((at.getOffset() & 3) != 0 && at.compareTo(block.getEnd()) < 0) {
            at = at.next();
        }
        Address last = block.getEnd();
        while (at != null && at.compareTo(last) < 0 && !monitor.isCancelled()) {
            Address next;
            try {
                next = at.add(4);
            } catch (Exception e) {
                break;
            }
            if (!isUndefined(at)) {
                at = next;
                continue;
            }
            int word;
            try {
                word = currentProgram.getMemory().getInt(at);
            } catch (Exception e) {
                at = next;
                continue;
            }
            if (!isPrologue(word)) {
                at = next;
                continue;
            }
            if (currentProgram.getFunctionManager()
                    .getFunctionContaining(at) != null) {
                at = next;
                continue;
            }
            new ArmDisassembleCommand(at, null, false)
                    .applyTo(currentProgram, monitor);
            if (currentProgram.getListing().getInstructionAt(at) != null) {
                CreateFunctionCmd make = new CreateFunctionCmd(null, at, null,
                        ghidra.program.model.symbol.SourceType.ANALYSIS);
                if (make.applyTo(currentProgram, monitor)) {
                    created++;
                }
            }
            at = next;
        }
        return created;
    }

    /** BL: cond != 0xF and bits 27..24 == 0b1011. Returns the target, or -1. */
    private long blTarget(Address at, int word) {
        if ((word >>> 28) == 0xF || ((word >>> 24) & 0xF) != 0xB) {
            return -1;
        }
        int imm = word & 0x00FFFFFF;
        if ((imm & 0x00800000) != 0) {
            imm -= 0x01000000;
        }
        return at.getOffset() + 8 + ((long) imm) * 4;
    }

    private int callTargetPass(MemoryBlock block) throws Exception {
        int created = 0;
        Address at = block.getStart();
        while ((at.getOffset() & 3) != 0 && at.compareTo(block.getEnd()) < 0) {
            at = at.next();
        }
        while (at != null && at.compareTo(block.getEnd()) < 0
                && !monitor.isCancelled()) {
            Address next;
            try {
                next = at.add(4);
            } catch (Exception e) {
                break;
            }
            int word;
            try {
                word = currentProgram.getMemory().getInt(at);
            } catch (Exception e) {
                at = next;
                continue;
            }
            long target = blTarget(at, word);
            if (target < 0) {
                at = next;
                continue;
            }
            Address to;
            try {
                to = at.getNewAddress(target);
            } catch (Exception e) {
                at = next;
                continue;
            }
            if (!currentProgram.getMemory().contains(to)
                    || currentProgram.getFunctionManager()
                            .getFunctionAt(to) != null) {
                at = next;
                continue;
            }
            new ArmDisassembleCommand(to, null, false)
                    .applyTo(currentProgram, monitor);
            if (currentProgram.getListing().getInstructionAt(to) != null) {
                CreateFunctionCmd make = new CreateFunctionCmd(null, to, null,
                        ghidra.program.model.symbol.SourceType.ANALYSIS);
                if (make.applyTo(currentProgram, monitor)) {
                    created++;
                }
            }
            at = next;
        }
        return created;
    }

    /* Addresses supplied from outside, one hex value per line.
     *
     * WHY THIS IS NEEDED. The BL anchor only sees branches inside the program
     * being analysed, and ITCM is imported as a separate program from ARM9
     * section 0 - so a call from section 0 into ITCM is invisible to both
     * passes. The frame closure's only two unresolved targets, 0x01FFA478 and
     * 0x01FFD538, are exactly that case: ARM leaves in ITCM, called 14 times
     * between them from section 0, found by neither anchor.
     *
     * tools/find_call_targets.py decodes every BL across ALL sections at once
     * and writes what is missing here. */
    private int extraEntriesPass(MemoryBlock block) throws Exception {
        /* THE PATH IS PASSED IN, NOT DERIVED FROM THE SCRIPT'S LOCATION.
         *
         * Deriving it broke twice over. getSourceFile().getParentFile() returns
         * a ResourceFile in current Ghidra, and there is no File(ResourceFile,
         * String) constructor - which is why this script stopped compiling at
         * all, and why 741 functions quietly vanished from the re-export.
         *
         * And even fixed, it would point at the wrong place: scripts are staged
         * into the Ghidra install to run (see tools/ghidra_run.ps1), so walking
         * up three parents lands inside Ghidra, not in this project. */
        String[] scriptArgs = getScriptArgs();
        if (scriptArgs.length < 1) {
            println("FillGaps: no extra-entries file given; skipping that pass");
            return 0;
        }
        java.io.File list = new java.io.File(scriptArgs[0]);
        if (!list.exists()) {
            return 0;
        }
        int created = 0;
        java.util.Scanner in = new java.util.Scanner(list);
        while (in.hasNextLine()) {
            String line = in.nextLine().trim();
            if (line.isEmpty() || !line.startsWith("0x")) {
                continue;
            }
            long value;
            try {
                value = Long.parseLong(line.substring(2), 16);
            } catch (NumberFormatException e) {
                continue;
            }
            Address to;
            try {
                to = block.getStart().getNewAddress(value);
            } catch (Exception e) {
                continue;
            }
            if (!block.contains(to)
                    || currentProgram.getFunctionManager().getFunctionAt(to)
                            != null) {
                continue;
            }
            new ArmDisassembleCommand(to, null, false)
                    .applyTo(currentProgram, monitor);
            if (currentProgram.getListing().getInstructionAt(to) != null) {
                CreateFunctionCmd make = new CreateFunctionCmd(null, to, null,
                        ghidra.program.model.symbol.SourceType.ANALYSIS);
                if (make.applyTo(currentProgram, monitor)) {
                    created++;
                }
            }
        }
        in.close();
        return created;
    }

    @Override
    public void run() throws Exception {
        int totalProlog = 0;
        int totalCalled = 0;
        int totalExtra = 0;

        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (!block.isExecute() && !block.isInitialized()) {
                continue;
            }
            println("FillGaps: scanning " + block.getName() + " "
                    + block.getStart() + ".." + block.getEnd());

            for (int round = 0; round < MAX_ROUNDS; round++) {
                int p = prologuePass(block);
                int c = callTargetPass(block);
                int x = extraEntriesPass(block);
                totalProlog += p;
                totalCalled += c;
                totalExtra += x;
                println("FillGaps:   round " + round + " prologues: " + p
                        + "  call targets: " + c + "  supplied: " + x);
                if (p == 0 && c == 0 && x == 0) {
                    break;
                }
            }
        }

        println("FillGaps: " + totalProlog + " function(s) from prologues, "
                + totalCalled + " from BL targets, " + totalExtra
                + " supplied");
    }
}
