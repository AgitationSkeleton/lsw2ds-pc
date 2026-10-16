/* Give each tail-call veneer the signature of what it jumps to.
 *
 * WHY THIS IS NEEDED AFTER TURNING ON DECOMPILER PARAMETER ID. A veneer is two
 * instructions:
 *
 *     ldr r12, [pc, #N]
 *     bx  r12
 *
 * They touch no argument registers, so Parameter ID concludes - correctly, as
 * far as those two instructions go - that the function takes nothing. It then
 * propagates that to every caller and REMOVES the arguments they were passing.
 * FUN_020cccdc(0x7e) became FUN_020cccdc(), which is worse than before: the
 * value the hardware puts in r0 vanished from the C entirely.
 *
 * The truth is that a veneer's signature IS its target's signature, because a
 * branch preserves every register and the target is what consumes them. Setting
 * that explicitly gives Parameter ID something correct to propagate, and the
 * callers get their arguments back.
 *
 * The pairs come from a file rather than being re-derived here, so the
 * instruction-pattern match lives in exactly one place - tools/gen_veneers.py
 * already needs it for the host forwarders.
 *
 * Usage:  -postScript FixVeneerSignatures.java <pairs.txt>
 *         each line "<veneer-hex> <target-hex>"
 */

import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.SourceType;

public class FixVeneerSignatures extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: FixVeneerSignatures <pairs.txt>");
            return;
        }
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        FunctionManager fm = currentProgram.getFunctionManager();

        int applied = 0, missingVeneer = 0, missingTarget = 0, sameAlready = 0;
        for (String line : lines) {
            String s = line.trim();
            if (s.isEmpty() || s.startsWith("#")) continue;
            String[] parts = s.split("\\s+");
            if (parts.length < 2) continue;

            Address va = toAddr(Long.parseLong(parts[0], 16));
            Address ta = toAddr(Long.parseLong(parts[1], 16));
            Function veneer = fm.getFunctionAt(va);
            Function target = fm.getFunctionAt(ta);
            if (veneer == null) { missingVeneer++; continue; }
            if (target == null) { missingTarget++; continue; }

            if (veneer.getParameterCount() == target.getParameterCount()
                    && veneer.getParameterCount() > 0) {
                sameAlready++;
                continue;
            }
            /* Copy the target's parameters and return type onto the veneer, and
             * mark them USER_DEFINED so later analysis does not helpfully
             * discard them again. */
            veneer.updateFunction(target.getCallingConventionName(),
                    target.getReturn(),
                    Function.FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS,
                    true, SourceType.USER_DEFINED,
                    target.getParameters());
            applied++;
        }
        println("veneers given their target's signature: " + applied);
        println("  already matching: " + sameAlready
                + "   no veneer at address: " + missingVeneer
                + "   no target function: " + missingTarget);
    }
}
