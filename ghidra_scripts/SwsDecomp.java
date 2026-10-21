// Decompile functions at the given addresses; write C + callers + callees to work/decomp/<addr>.c
// Args: hex addresses (no 0x). @category SWS
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class SwsDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        String outDir = "E:/sws_community/work/decomp/";
        for (String a : getScriptArgs()) {
            Address addr = toAddr(Long.parseUnsignedLong(a, 16));
            Function f = getFunctionContaining(addr);
            if (f == null) { createFunction(addr, null); f = getFunctionAt(addr); }
            if (f == null) { println("no function at " + a); continue; }
            StringBuilder sb = new StringBuilder();
            sb.append("// ").append(f.getName()).append(" @ ").append(f.getEntryPoint()).append(" size ").append(f.getBody().getNumAddresses()).append("\n");
            sb.append("// callers:");
            Set<String> seen = new TreeSet<>();
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                seen.add(r.getFromAddress() + (c != null ? "(" + c.getEntryPoint() + ")" : "(data)") + ":" + r.getReferenceType());
            }
            for (String s : seen) sb.append(" ").append(s);
            sb.append("\n// callees:");
            for (Function c : f.getCalledFunctions(monitor)) sb.append(" ").append(c.getEntryPoint());
            sb.append("\n");
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            sb.append(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            try (PrintWriter w = new PrintWriter(new FileWriter(outDir + a.toLowerCase() + ".c"))) { w.print(sb); }
            println("wrote " + a);
        }
    }
}
