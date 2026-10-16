/* Move a function's entry to where its callers actually enter it.
 *
 * WHY. Ghidra can start a function one or more instructions late - it found
 * the body by a branch into it, or a gap-filling pass decoded from the second
 * word - and then every real call to the true entry reaches no lifted code and
 * runs in the interpreter. ITCM 0x01FFAD38 is the case that prompted this:
 * the main-RAM code calls it with `bl 0x01FFAD38` (0x020103E0), its
 * first instruction is `push {r0-r3}`, and the corpus had FUN_01ffad3c starting
 * at the SECOND push - so the interpreter's hottest entry by far was a function
 * the banks held, entered four bytes early.
 *
 * Removes any function at <new> and at <old>, then creates one at <new>, whose
 * body the flow from <new> decides (it runs straight into the old body).
 *
 * Usage:  -postScript RebaseFunctionEntry.java <new-hex> <old-hex>
 *
 * @category LSW2
 */

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class RebaseFunctionEntry extends GhidraScript {
    private Address parse(String a) {
        String hex = a.startsWith("0x") || a.startsWith("0X") ? a.substring(2) : a;
        return toAddr(Long.parseLong(hex, 16));
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            println("usage: RebaseFunctionEntry <new-hex> <old-hex>");
            return;
        }
        Address entry = parse(args[0]);
        Address old = parse(args[1]);
        for (Address at : new Address[] {entry, old}) {
            Function f = getFunctionAt(at);
            if (f != null) {
                println("removing " + f.getName() + " (" + f.getBody().getNumAddresses()
                        + " bytes) at " + at);
                removeFunction(f);
            }
        }
        if (getInstructionAt(entry) == null) disassemble(entry);
        CreateFunctionCmd cmd = new CreateFunctionCmd(entry);
        if (!cmd.applyTo(currentProgram, monitor)) {
            println("could NOT create a function at " + entry + ": " + cmd.getStatusMsg());
            return;
        }
        Function f = getFunctionAt(entry);
        println("function at " + entry + ": body " + f.getBody().getNumAddresses()
                + " bytes, " + f.getBody().getMinAddress() + " - "
                + f.getBody().getMaxAddress());
    }
}
