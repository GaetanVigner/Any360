// Pre-analysis script: disassembles and creates every guest function the recompiler found (functions.txt: "<hex address>
// <name>" per line, from generated/default/crackdown_init.cpp), named as in the recompiled code, so auto-analysis and
// cross-references work on the raw Xbox 360 image.
// Lines whose name starts with "helper_" are the XDK's register save/restore helpers (__savegprlr_N...), entered at
// several points and ending in blr: they are created as returning functions, and the "Non-Returning Functions -
// Discovered" analyzer is switched off, since it otherwise marks them non-returning and truncates every caller.
// Args: <functions.txt>
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class SeedFunctions extends GhidraScript {
  @Override
  protected void run() throws Exception {
    List<String> lines = Files.readAllLines(Paths.get(getScriptArgs()[0]));
    setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
    int created = 0, failed = 0;
    for (String line : lines) {
      String[] parts = line.trim().split("\\s+");
      if (parts.length < 2) continue;
      Address at = toAddr(Long.parseLong(parts[0], 16));
      new DisassembleCommand(at, null, true).applyTo(currentProgram, monitor);
      Function f = getFunctionAt(at);
      if (f == null) f = createFunction(at, parts[1]);
      else f.setName(parts[1], SourceType.IMPORTED);
      if (f == null) ++failed; else { ++created; f.setNoReturn(false); }
      if ((created + failed) % 5000 == 0) println("seeded " + (created + failed) + " / " + lines.size());
    }
    println("SeedFunctions: " + created + " functions, " + failed + " failed");
  }
}
