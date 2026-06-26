// Headless Ghidra script: exports decompiler-like C, symbols, functions and refs.
// Place is auto-used by scripts/ghidra_full_analysis.sh.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.address.Address;
import java.io.*;

public class ExportAnalysis extends GhidraScript {
  @Override
  public void run() throws Exception {
    String outDir = System.getProperty("analysis.out", "ghidra_out");
    File dir = new File(outDir);
    dir.mkdirs();
    String name = currentProgram.getName().replaceAll("[^A-Za-z0-9._-]", "_");

    try (PrintWriter pw = new PrintWriter(new File(dir, name + ".functions.txt"))) {
      for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
        pw.printf("%s %s size=%d\n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses());
      }
    }

    try (PrintWriter pw = new PrintWriter(new File(dir, name + ".symbols.txt"))) {
      SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
      while (it.hasNext()) {
        Symbol s = it.next();
        pw.printf("%s %s %s %s\n", s.getAddress(), s.getSymbolType(), s.getParentNamespace(), s.getName());
      }
    }

    DecompInterface ifc = new DecompInterface();
    ifc.openProgram(currentProgram);
    try (PrintWriter pw = new PrintWriter(new File(dir, name + ".decompiled.c"))) {
      for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
        if (monitor.isCancelled()) break;
        DecompileResults res = ifc.decompileFunction(f, 60, monitor);
        pw.printf("\n/* ===== %s @ %s ===== */\n", f.getName(), f.getEntryPoint());
        if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
          pw.println(res.getDecompiledFunction().getC());
        } else {
          pw.println("/* decompile failed */");
        }
      }
    } finally {
      ifc.dispose();
    }
  }
}
