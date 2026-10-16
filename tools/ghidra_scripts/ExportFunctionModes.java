/* Export every function's address, name and ARM/Thumb mode as JSON.
 *
 * WHY THIS EXISTS. The corpus exports in out/ghidra/ record addresses and
 * nothing else. That is enough to decompile, because Ghidra already decided
 * the mode before it produced the C - but it is not enough to hand the corpus
 * to anything that has to decode the bytes itself: a lifter, a disassembler,
 * an entry-point list for another framework.
 *
 * tools/gen_ndsrecomp_config.py had to guess instead, by matching the first
 * instruction against known prologues. That works for 5,404 of 5,860 ARM9
 * functions and is honest about the rest, but it is a guess, and a function
 * lifted in the wrong instruction set is silently and totally wrong - the
 * exact defect class this project keeps having to dig out.
 *
 * GHIDRA ALREADY KNOWS THE ANSWER EXACTLY. On ARM it carries a context
 * register named TMode: 1 where the listing is Thumb, 0 where it is ARM. It is
 * set during disassembly and is per-address, so reading it at a function's
 * entry point is the same fact the decompiler used. No heuristic involved.
 *
 * The output shape - a JSON array of {"addr","name","mode"} - is deliberately
 * the same one ndsrecomp's discovery seeds use, so the file is usable both by
 * this project's tools and, if the retarget onto ndsrecomp goes ahead, directly as
 * a seed list. Seeds are hints, not correctness: they say where to start
 * looking, and a lifter is still expected to verify.
 *
 * Usage:  -postScript ExportFunctionModes.java <output.json>
 */

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.PrintWriter;

public class ExportFunctionModes extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: ExportFunctionModes <output.json>");
            return;
        }

        Register tmode = currentProgram.getProgramContext().getRegister("TMode");
        if (tmode == null) {
            /* Not fatal, but say so loudly rather than emitting a file that
             * silently claims everything is ARM - which is precisely the
             * wrong answer this script was written to stop producing. */
            println("ExportFunctionModes: NO TMode REGISTER in this program's "
                    + "language. Refusing to write - every mode would be a "
                    + "guess, and the caller would not be able to tell.");
            return;
        }

        int arm = 0, thumb = 0;
        StringBuilder sb = new StringBuilder("[\n");
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        boolean first = true;
        while (it.hasNext()) {
            if (monitor.isCancelled()) {
                println("ExportFunctionModes: CANCELLED - no file written.");
                return;
            }
            Function fn = it.next();
            Address entry = fn.getEntryPoint();

            boolean isThumb = false;
            RegisterValue rv =
                currentProgram.getProgramContext().getRegisterValue(tmode, entry);
            if (rv != null && rv.hasValue()) {
                isThumb = rv.getUnsignedValueIgnoreMask().intValue() != 0;
            }
            if (isThumb) { thumb++; } else { arm++; }

            if (!first) { sb.append(",\n"); }
            first = false;
            sb.append(String.format(
                " {\"addr\": \"0x%08X\", \"name\": \"%s\", \"mode\": \"%s\"}",
                entry.getOffset(), fn.getName(), isThumb ? "thumb" : "arm"));
        }
        sb.append("\n]\n");

        try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
            w.print(sb);
        }
        println(String.format(
            "ExportFunctionModes: wrote %d functions (arm=%d thumb=%d) to %s",
            arm + thumb, arm, thumb, args[0]));
    }
}
