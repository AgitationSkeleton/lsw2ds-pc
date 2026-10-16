/* Dump each function's INSTRUCTION SET, which nothing else in the export records.
 *
 * WHY THIS IS NEEDED. The differential harness calls a ROM function through the
 * ARM interpreter, and the interpreter selects ARM or Thumb from bit 0 of the
 * address - the same convention BX and BLX use, and the same one the cartridge
 * itself uses. Get it wrong and the interpreter decodes Thumb halfwords as ARM
 * words and dies on garbage, which is exactly what happened at 0x0200071C.
 *
 * The instruction set CANNOT be inferred from the address. This project already
 * concluded once that the game was "effectively pure ARM" because 4,390 of 4,391
 * functions sit at 4-byte-aligned addresses, and that was wrong: Ghidra records
 * a Thumb function at its aligned address too. Alignment says nothing.
 *
 * Ghidra knows, because it tracked the TMode context register while
 * disassembling. This asks it, rather than guessing from the first opcode.
 *
 * Output: one "0xADDRESS thumb|arm" line per function.
 */

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class DumpInstructionSet extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = (args.length > 0) ? args[0] : "instruction_sets.txt";

        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        if (tmode == null) {
            println("no TMode register in this language - nothing to dump");
            return;
        }

        PrintWriter w = new PrintWriter(out, "UTF-8");
        int thumb = 0, arm = 0, unknown = 0;
        FunctionIterator it = currentProgram.getFunctionManager()
                .getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            Address at = f.getEntryPoint();
            RegisterValue rv = currentProgram.getProgramContext()
                    .getRegisterValue(tmode, at);
            String kind;
            if (rv == null || !rv.hasValue()) {
                /* No recorded context. Say so rather than defaulting to ARM:
                 * a silent default is how the alignment mistake happened. */
                kind = "unknown";
                unknown++;
            } else if (rv.getUnsignedValue().intValue() != 0) {
                kind = "thumb";
                thumb++;
            } else {
                kind = "arm";
                arm++;
            }
            w.printf("0x%s %s%n", at.toString(), kind);
        }
        w.close();
        println("functions: " + (thumb + arm + unknown)
                + "  thumb=" + thumb + "  arm=" + arm
                + "  unknown=" + unknown);
        println("wrote " + out);
    }
}
