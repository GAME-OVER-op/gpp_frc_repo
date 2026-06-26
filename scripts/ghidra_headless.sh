#!/usr/bin/env bash
set -euo pipefail
: "${GHIDRA_INSTALL_DIR:?Set GHIDRA_INSTALL_DIR=/path/to/ghidra}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJ="$ROOT/ghidra_project"
mkdir -p "$PROJ"
"$GHIDRA_INSTALL_DIR/support/analyzeHeadless" "$PROJ" lybfghook \
  -import "$ROOT/binaries/arm64-v8a.so" \
  -analysisTimeoutPerFile 1200 \
  -overwrite
