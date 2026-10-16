// DecompileAddrs.java - Ghidra headless postScript.
//
// Decompiles a list of addresses to C and writes them to one text file, so a
// whole batch of ROM questions can be answered in a single Ghidra run instead
// of one analyzeHeadless start-up per function (each costs ~30s).
//
// Usage:  -postScript DecompileAddrs.java <addrs.txt> <out.txt>
//
// Addresses come from a FILE, one hex address per line, rather than as argv.
// analyzeHeadless does not reliably forward a long trailing argument list to a
// post-script - only the first extra argument arrived - and a file is both
// robust and easier to generate.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DecompileAddrs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DecompileAddrs <addrs.txt> <out.txt>");
            return;
        }
        java.util.List<String> addrs = new java.util.ArrayList<>();
        for (String line : java.nio.file.Files.readAllLines(
                java.nio.file.Paths.get(args[0]))) {
            String t = line.trim();
            if (!t.isEmpty() && !t.startsWith("#")) addrs.add(t);
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter out = new PrintWriter(args[1], "UTF-8");
        for (String spec : addrs) {
            String raw = spec.toLowerCase().startsWith("0x")
                ? spec.substring(2) : spec;
            Address a = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(Long.parseLong(raw, 16));
            Function f = getFunctionContaining(a);
            out.println("//================================================");
            if (f == null) {
                // No function here yet - create one so a bare address still
                // decompiles rather than silently returning nothing.
                f = createFunction(a, null);
            }
            if (f == null) {
                out.println("// 0x" + raw + ": no function and none could be created");
                continue;
            }
            out.println("// 0x" + raw + "  in  " + f.getName()
                + "  @ " + f.getEntryPoint());
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted()) {
                out.println(r.getDecompiledFunction().getC());
            } else {
                out.println("// decompilation failed: "
                    + (r == null ? "null" : r.getErrorMessage()));
            }
        }
        out.close();
        di.dispose();
        println("wrote " + args[1] + " (" + addrs.size() + " addresses)");
    }
}
