// Decompile EVERY function in the program to disk, once, so that later lookups
// are a grep instead of a Ghidra run.
//
// WHY THIS EXISTS. Every question so far has cost a full headless invocation -
// import, analyse, decompile a handful of addresses - which is minutes per
// answer and has to happen inside whatever session is asking. Decompiling the
// whole image once turns that into a file read.
//
// OUTPUT, all under out/ghidra/decomp/:
//
//   c/FUN_%08x.c        one file per function, its full decompiled body
//   index.jsonl         one JSON object per function: address, name, size,
//                       parameter count, and the addresses it CALLS
//   callers.jsonl       the inverted edge list: address, and who calls it
//   strings.txt         every defined string with its address
//   index.md            a human-scannable table, sorted by address
//
// The C files carry a header comment with the address so a grep hit identifies
// itself without needing the filename.
//
// @category LSW2

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

public class DecompileAll extends GhidraScript {

    private static String jsonEscape(String s) {
        if (s == null) {
            return "";
        }
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20 || c > 0x7e) {
                        b.append(String.format("\\u%04x", (int) c));
                    } else {
                        b.append(c);
                    }
            }
        }
        return b.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = args.length > 0 ? args[0] : "out/ghidra/decomp";

        File base = new File(outDir);
        File cDir = new File(base, "c");
        cDir.mkdirs();

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        // 60 s is generous; a handful of very large functions would otherwise
        // stall the whole run, and they are recorded as timeouts rather than
        // silently skipped.
        int timeout = 60;

        PrintWriter index = new PrintWriter(new BufferedWriter(
                new FileWriter(new File(base, "index.jsonl"))));
        PrintWriter md = new PrintWriter(new BufferedWriter(
                new FileWriter(new File(base, "index.md"))));

        // address -> the addresses that call it
        Map<String, Set<String>> callers = new TreeMap<String, Set<String>>();

        md.println("| address | name | size | calls | file |");
        md.println("| --- | --- | --- | --- | --- |");

        int total = 0;
        int failed = 0;

        FunctionIterator functions = currentProgram.getFunctionManager()
                .getFunctions(true);
        while (functions.hasNext() && !monitor.isCancelled()) {
            Function f = functions.next();
            Address entry = f.getEntryPoint();
            String addr = String.format("0x%08x", entry.getOffset());
            String name = f.getName();
            long size = f.getBody().getNumAddresses();

            // Who does it call? Taken from the reference manager rather than
            // parsed out of the C, so an indirect call through a vtable slot
            // still shows up when Ghidra resolved it.
            List<String> calls = new ArrayList<String>();
            Set<String> seen = new HashSet<String>();
            InstructionIterator instructions =
                    currentProgram.getListing().getInstructions(f.getBody(), true);
            while (instructions.hasNext()) {
                Instruction ins = instructions.next();
                for (Reference r : ins.getReferencesFrom()) {
                    if (r.getReferenceType().isCall()) {
                        String t = String.format("0x%08x",
                                r.getToAddress().getOffset());
                        if (seen.add(t)) {
                            calls.add(t);
                        }
                        Set<String> who = callers.get(t);
                        if (who == null) {
                            who = new java.util.TreeSet<String>();
                            callers.put(t, who);
                        }
                        who.add(addr);
                    }
                }
            }

            String fileName = String.format("FUN_%08x.c", entry.getOffset());
            File cFile = new File(cDir, fileName);

            DecompileResults res = decomp.decompileFunction(f, timeout, monitor);
            String body;
            if (res != null && res.decompileCompleted()
                    && res.getDecompiledFunction() != null) {
                body = res.getDecompiledFunction().getC();
            } else {
                body = "// DECOMPILATION FAILED"
                        + (res != null ? ": " + res.getErrorMessage() : "");
                failed++;
            }

            PrintWriter out = new PrintWriter(new BufferedWriter(
                    new FileWriter(cFile)));
            // The header makes a grep hit self-identifying.
            out.println("// " + addr + "  " + name + "  size=" + size);
            out.println("// calls: " + String.join(" ", calls));
            out.println();
            out.print(body);
            out.close();

            StringBuilder callsJson = new StringBuilder();
            for (int i = 0; i < calls.size(); i++) {
                if (i > 0) {
                    callsJson.append(", ");
                }
                callsJson.append('"').append(calls.get(i)).append('"');
            }
            index.println("{\"address\": \"" + addr + "\", \"name\": \""
                    + jsonEscape(name) + "\", \"size\": " + size
                    + ", \"params\": " + f.getParameterCount()
                    + ", \"file\": \"c/" + fileName + "\", \"calls\": ["
                    + callsJson + "]}");

            md.println("| `" + addr + "` | " + name + " | " + size + " | "
                    + calls.size() + " | `c/" + fileName + "` |");

            total++;
            if (total % 250 == 0) {
                println("decompiled " + total + " functions");
            }
        }

        index.close();
        md.close();
        decomp.dispose();

        PrintWriter cw = new PrintWriter(new BufferedWriter(
                new FileWriter(new File(base, "callers.jsonl"))));
        for (Map.Entry<String, Set<String>> e : callers.entrySet()) {
            StringBuilder who = new StringBuilder();
            int i = 0;
            for (String a : e.getValue()) {
                if (i++ > 0) {
                    who.append(", ");
                }
                who.append('"').append(a).append('"');
            }
            cw.println("{\"address\": \"" + e.getKey() + "\", \"callers\": ["
                    + who + "]}");
        }
        cw.close();

        // Defined strings, which are cheap here and save a separate pass.
        PrintWriter sw = new PrintWriter(new BufferedWriter(
                new FileWriter(new File(base, "strings.txt"))));
        DataIterator data = currentProgram.getListing().getDefinedData(true);
        int strings = 0;
        while (data.hasNext() && !monitor.isCancelled()) {
            Data d = data.next();
            if (d.getDataType() == null) {
                continue;
            }
            Object value = d.getValue();
            if (value instanceof String) {
                sw.println(String.format("0x%08x\t%s",
                        d.getAddress().getOffset(),
                        ((String) value).replace("\n", "\\n")));
                strings++;
            } else if (value instanceof StringDataInstance) {
                sw.println(String.format("0x%08x\t%s",
                        d.getAddress().getOffset(),
                        ((StringDataInstance) value).getStringValue()));
                strings++;
            }
        }
        sw.close();

        println("DecompileAll: " + total + " functions, " + failed
                + " failed, " + callers.size() + " call targets, "
                + strings + " strings -> " + base.getAbsolutePath());
    }
}
