// FixVoidReturns.java - Ghidra headless postScript.
//
// WHAT IS WRONG. Ghidra decided 4,149 of the 5,849 functions in this image
// return nothing. For 108 of them that is demonstrably false: their callers
// assign the result, at 437 call sites -
//
//     uVar6 = FUN_020cd574(...);      // and FUN_020cd574 is declared void
//     piVar3 = (int *)uVar6;
//
// The callee's body then has a bare `return;`, so in the port the caller reads
// whatever the host left in EAX. FUN_020CD574 is the thread scheduler's "find
// the next ready thread", and its instructions plainly end
//
//     CMP r0, #0 / LDRNE r1,[r0,#0x64] / CMPNE r1,#1 / BNE loop / BX lr
//
// returning r0. Read as void, the scheduler got a garbage "next thread", took
// the `next == current || next == 0` early exit every time, and never switched
// - which is what left the port spinning in a file-system wait.
//
// WHY THIS IS A GHIDRA JOB AND NOT A SOURCE PATCH. Which expression to return
// is exactly what the decompiler works out. Told the function returns a value,
// it emits the right `return`; guessed at by hand across 108 functions it would
// be 108 chances to be wrong. So the return type is set and the function is
// decompiled again - the output is still the decompiler's, not an edit of it.
//
// The list is EVIDENCE-DRIVEN: a function only appears in it because a call
// site uses its result. Nothing is retyped speculatively.
//
// Usage:  -postScript FixVoidReturns.java <addrs.txt> <outdir>
//
// @category LSW2

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.Undefined4DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

public class FixVoidReturns extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: FixVoidReturns <addrs.txt> <outdir>");
            return;
        }
        List<String> addrs = new ArrayList<>();
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            String t = line.trim();
            if (!t.isEmpty() && !t.startsWith("#")) addrs.add(t);
        }
        Path outdir = Paths.get(args[1]);
        Files.createDirectories(outdir);

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);

        int retyped = 0, written = 0, missing = 0, failed = 0;
        for (String spec : addrs) {
            String raw = spec.toLowerCase().startsWith("0x")
                ? spec.substring(2) : spec;
            Address a = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(Long.parseLong(raw, 16));
            Function f = getFunctionContaining(a);
            if (f == null) { missing++; continue; }

            f.setReturnType(Undefined4DataType.dataType, SourceType.USER_DEFINED);
            retyped++;

            DecompileResults r = di.decompileFunction(f, 120, monitor);
            if (r == null || !r.decompileCompleted()) { failed++; continue; }

            // Same header the corpus already carries, so every tool that reads
            // `size=` keeps working on these files.
            StringBuilder calls = new StringBuilder();
            for (Function c : f.getCalledFunctions(monitor)) {
                calls.append(" ").append(c.getEntryPoint());
            }
            String name = f.getName();
            PrintWriter out = new PrintWriter(
                outdir.resolve(name + ".c").toFile(), "UTF-8");
            out.println("// 0x" + f.getEntryPoint() + "  " + name
                + "  size=" + f.getBody().getNumAddresses());
            out.println("// calls:" + calls);
            out.println();
            out.println();
            out.print(r.getDecompiledFunction().getC());
            out.close();
            written++;
        }
        println("FIXVOIDRETURNS retyped=" + retyped + " written=" + written
            + " missing=" + missing + " failed=" + failed);
    }
}
