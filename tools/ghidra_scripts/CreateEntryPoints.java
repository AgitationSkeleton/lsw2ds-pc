/* Make functions at the addresses the hardware enters, so analysis follows them.
 *
 * WHY ANYTHING IS MISSING AT ALL. Ghidra finds functions by following calls. The
 * ARM9 reset vector at 0x02000800 is not called by anything - the hardware jumps
 * to it - so nothing points at it, no function was ever created there, and every
 * branch inside it went unexplored.
 *
 * That is why FUN_02000938, FUN_02000950, FUN_02000A0C and FUN_02000A98 do not
 * exist in the program: they are reached only from the reset vector. This port
 * had to hand-translate the stub for exactly that reason, and 741 functions were
 * missing from a re-export without anyone noticing until the counts were
 * compared.
 *
 * The entry point is not a guess. It is a field in the cartridge header, at
 * offset 0x24, and it is passed in rather than assumed here.
 *
 * Creating a function there lets Ghidra's own analysis walk the branches and
 * create everything downstream - which is the difference between decompiling
 * the game and decompiling the parts of it that happened to be easy to find.
 *
 * Usage:  -postScript CreateEntryPoints.java <hex-address> [<hex-address> ...]
 */

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class CreateEntryPoints extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: CreateEntryPoints <hex-address>... | <file>");
            return;
        }
        /* A list this long does not fit on a command line, and the addresses
         * that matter most are exactly the ones nothing branches to directly. */
        if (args.length == 1 && new java.io.File(args[0]).exists()) {
            java.util.List<String> read =
                    java.nio.file.Files.readAllLines(
                            java.nio.file.Paths.get(args[0]));
            java.util.List<String> acc = new java.util.ArrayList<>();
            for (String line : read) {
                for (String tok : line.trim().split("\s+")) {
                    if (!tok.isEmpty() && !tok.startsWith("#")) acc.add(tok);
                }
            }
            args = acc.toArray(new String[0]);
            println("read " + args.length + " addresses from " + read.size()
                    + " line(s)");
        }
        int made = 0, already = 0, failed = 0;
        for (String a : args) {
            String hex = a.startsWith("0x") || a.startsWith("0X")
                    ? a.substring(2) : a;
            Address at = toAddr(Long.parseLong(hex, 16));
            Function f = getFunctionAt(at);
            if (f != null) {
                already++;
                continue;
            }
            /* Disassemble first: an address nothing referenced is usually
             * still raw bytes, and a function cannot be made over undefined
             * data.
             *
             * Some of these are worse than undefined - they are DEFINED data,
             * because analysis saw no code reaching them and took the bytes for
             * a literal pool. Ghidra refuses outright then ("Function
             * entryPoint may not be created on defined data"), so the mistaken
             * definition is cleared first. That is safe precisely because the
             * caller has evidence this address is executed: it is on the list
             * because a branch elsewhere targets it. */
            if (getInstructionAt(at) == null) {
                if (getDataAt(at) != null) {
                    clearListing(at, at.add(3));
                }
                disassemble(at);
            }
            CreateFunctionCmd cmd = new CreateFunctionCmd(at);
            if (cmd.applyTo(currentProgram, monitor)) {
                made++;
                println("created a function at " + at);
            } else {
                failed++;
                println("could NOT create a function at " + at + ": "
                        + cmd.getStatusMsg());
            }
        }
        println("entry points: " + made + " created, " + already
                + " already present, " + failed + " failed");
        if (made > 0) {
            println("re-analysing so the branches inside them are followed");
            analyzeAll(currentProgram);
            println("functions now: "
                    + currentProgram.getFunctionManager().getFunctionCount());
        }
    }
}
