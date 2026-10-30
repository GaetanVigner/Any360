// Writes the decompiled C of the functions containing the given addresses (hex, or sub_XXXXXXXX names).
// Args: <output file> <address or name>...
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {
  static long Parse(String text) {
    String t = text.replaceFirst("^(sub_|0x)", "");
    return Long.parseLong(t, 16);
  }

  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    try (PrintWriter out = new PrintWriter(args[0])) {
      DecompInterface decomp = new DecompInterface();
      decomp.openProgram(currentProgram);
      for (int i = 1; i < args.length; ++i) {
        Address at = toAddr(Parse(args[i]));
        Function f = getFunctionContaining(at);
        if (f == null) {
          out.println("/* no function contains " + at + " */");
          continue;
        }
        DecompileResults r = decomp.decompileFunction(f, 180, monitor);
        out.println("/* ---- " + f.getName() + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses()
            + " bytes) ---- */");
        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "/* failed: " + r.getErrorMessage() + " */");
      }
      out.println("/* done */");
    }
  }
}
