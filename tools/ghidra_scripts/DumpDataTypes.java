// DumpDataTypes.java - Ghidra headless postScript.
//
// WHY. The port shapes every DAT_ symbol itself, from the text of the
// decompiled C, because that is all it had. Text is a poor source for one
// specific thing: the ELEMENT TYPE of a pointer, which decides what indexing
// and pointer arithmetic mean.
//
//     *(uint *)(DAT_020d1334 + 2) = ...;      // DAT_020d1334 is 0x04000184
//
// Ghidra typed that symbol `undefined2 *`, so +2 is +4 bytes and the write
// lands on IPCFIFOSEND at 0x04000188. Shaped as `undefined4 *` it is +8 bytes
// and lands on 0x0400018C - so the ARM9's message to the ARM7 was written to
// the wrong register and the reply it then waited for could never come.
//
// There was no local declaration in that file to infer the type from. But
// Ghidra HAS the type; nothing was asking it. This dumps every defined data
// symbol with its type and size so gen_dat_perfile.py can use the decompiler's
// own answer instead of guessing from how the C reads.
//
// Output: one line per symbol, tab separated -
//     address <TAB> name <TAB> typename <TAB> length <TAB> pointee-length
//
// pointee-length is 0 when the type is not a pointer. It is the number that
// matters: the width of one element of what the symbol points at.
//
// @category LSW2

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;

import java.io.PrintWriter;

public class DumpDataTypes extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: DumpDataTypes <out.txt>");
            return;
        }
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        int n = 0;
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            DataType t = d.getDataType();
            if (t == null) continue;
            int pointee = 0;
            if (t instanceof Pointer) {
                DataType to = ((Pointer) t).getDataType();
                if (to != null) pointee = to.getLength();
            }
            String label = d.getLabel();
            if (label == null) label = "";
            out.println(d.getAddress() + "\t" + label + "\t" + t.getName()
                + "\t" + t.getLength() + "\t" + pointee);
            n++;
        }
        out.close();
        println("DUMPDATATYPES wrote=" + n);
    }
}
