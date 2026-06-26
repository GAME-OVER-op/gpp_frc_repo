// Headless Ghidra script: exports functions, symbols and decompiled C.
// Output directory resolution order:
//   1) first script argument (recommended, absolute path)
//   2) -Danalysis.out=... JVM property
//   3) ANALYSIS_OUT environment variable
//   4) "ghidra_out" in the current working directory
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import java.io.*;

public class ExportAnalysis extends GhidraScript {
  @Override
  public void run() throws Exception {
    String outDir = null;
    String[] args = getScriptArgs();
    if (args != null && args.length > 0 && args[0] != null && !args[0].isEmpty()) {
      outDir = args[0];
    }
    if (outDir == null) outDir = System.getProperty("analysis.out");
    if (outDir == null) outDir = System.getenv("ANALYSIS_OUT");
    if (outDir == null) outDir = "ghidra_out";

    File dir = new File(outDir);
    if (!dir.isAbsolute()) dir = dir.getAbsoluteFile();
    dir.mkdirs();
    println("[ExportAnalysis] writing to: " + dir.getAbsolutePath());

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
    println("[ExportAnalysis] done");
  }
}
