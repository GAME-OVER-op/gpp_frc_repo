#!/usr/bin/env bash
set -euo pipefail
: "${GHIDRA_INSTALL_DIR:?Set GHIDRA_INSTALL_DIR=/path/to/ghidra}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJ="$ROOT/ghidra_project"
OUT="$ROOT/ghidra_out"
mkdir -p "$PROJ" "$OUT"
for f in "$ROOT"/binaries/*.so; do
  base="$(basename "$f")"
  echo "[*] Ghidra analyze $base -> $OUT"
  "$GHIDRA_INSTALL_DIR/support/analyzeHeadless" "$PROJ" lybfghook \
    -import "$f" \
    -overwrite \
    -analysisTimeoutPerFile 1800 \
    -scriptPath "$ROOT/ghidra_scripts" \
    -postScript ExportAnalysis.java "$OUT"
done
echo "[*] ghidra_out contents:"
ls -la "$OUT"
