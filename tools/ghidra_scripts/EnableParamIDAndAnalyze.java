/* Re-analyse with Decompiler Parameter ID, which is what recovers call arguments.
 *
 * WHY. The port's biggest remaining defect is that Ghidra emits calls with
 * fewer arguments than the callee takes - 2,735 call sites across 972 callees.
 * ARM passes the first four arguments in r0-r3, so a function handing its own
 * arguments onward emits no setup, and the default analysis has nothing to
 * attribute to the call:
 *
 *     LDR r1, =0x027FFFF0
 *     MOV r2, #0
 *     MOV r0, #0x7E
 *     BL  0x020CCCDC        ->  Ghidra writes  FUN_020cccdc(0x7e)
 *
 * The dropped r1 is a pointer the game later dereferences, and the port died on
 * it. Recovering these from the instructions by hand works but only reaches
 * values that are literals or untouched parameters; a register holding the
 * result of a previous call needs Ghidra's own variable naming to express.
 *
 * DECOMPILER PARAMETER ID IS THE ANALYSER THAT DOES THIS PROPERLY. It runs the
 * decompiler over the whole program to infer each function's real signature and
 * then propagates that to every call site. It is OFF BY DEFAULT because it is
 * slow - which is why the original import never ran it, and why the exported C
 * has been missing arguments all along.
 *
 * Slow is acceptable. Wrong is not.
 */

import ghidra.app.script.GhidraScript;

public class EnableParamIDAndAnalyze extends GhidraScript {
    @Override
    public void run() throws Exception {
        setAnalysisOption(currentProgram, "Decompiler Parameter ID", "true");
        /* Give the decompiler room; the default per-function timeout is short
         * and this program has functions that already time out. */
        setAnalysisOption(currentProgram,
                          "Decompiler Parameter ID.Analysis Decompiler Timeout (sec)",
                          "300");
        println("Decompiler Parameter ID enabled: "
                + getCurrentAnalysisOptionsAndValues(currentProgram)
                        .get("Decompiler Parameter ID"));
        analyzeAll(currentProgram);
        println("re-analysis complete for " + currentProgram.getName());
    }
}
