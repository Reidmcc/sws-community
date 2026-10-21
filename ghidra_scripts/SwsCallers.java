// Print the caller tree (upward) of a function, with strings each function references.
// Args: <hex addr> <depth>. Output to work/decomp/callers_<addr>.txt. @category SWS
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.io.*;
import java.util.*;

public class SwsCallers extends GhidraScript {
    PrintWriter w; Set<Address> seen = new HashSet<>();
    String strings(Function f) {
        List<String> out = new ArrayList<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (it.hasNext() && out.size() < 6) {
            for (Reference r : it.next().getReferencesFrom()) {
                Data d = getDataAt(r.getToAddress());
                if (d != null && d.hasStringValue()) out.add("\"" + d.getDefaultValueRepresentation().replace("\n"," ") + "\"");
            }
        }
        return String.join(" ", out);
    }
    void walk(Function f, int depth, String indent) {
        w.println(indent + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + " " + strings(f));
        if (depth == 0 || !seen.add(f.getEntryPoint())) return;
        Set<Function> callers = new LinkedHashSet<>();
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            if (c != null) callers.add(c); else w.println(indent + "  <- data ref " + r.getFromAddress());
        }
        for (Function c : callers) walk(c, depth - 1, indent + "  ");
    }
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        Address addr = toAddr(Long.parseUnsignedLong(a[0], 16));
        w = new PrintWriter(new FileWriter("E:/sws_community/work/decomp/callers_" + a[0].toLowerCase() + ".txt"));
        walk(getFunctionContaining(addr), Integer.parseInt(a[1]), "");
        w.close(); println("done");
    }
}
