// Pre-analysis: drop the Denuvo section (.idata, ~440 MB) and the expensive analyzers.
// @category SWS
import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;
import ghidra.program.model.listing.Program;
import ghidra.program.model.mem.MemoryBlock;

public class SwsPrepare extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println("block " + b.getName() + " " + b.getStart() + " size=" + b.getSize());
        }
        MemoryBlock idata = currentProgram.getMemory().getBlock(".idata");
        if (idata != null && idata.getSize() > 0x10000000L) {
            println("removing Denuvo block " + idata.getName() + " size=" + idata.getSize());
            currentProgram.getMemory().removeBlock(idata, monitor);
        }
        Options o = currentProgram.getOptions(Program.ANALYSIS_PROPERTIES);
        String[] off = { "Decompiler Switch Analysis", "Create Address Tables", "Embedded Media",
                         "Decompiler Parameter ID", "Aggressive Instruction Finder", "Condense Filler Bytes" };
        for (String k : off) {
            if (o.contains(k)) { o.setBoolean(k, false); println("disabled analyzer: " + k); }
            else println("analyzer not found: " + k);
        }
    }
}
