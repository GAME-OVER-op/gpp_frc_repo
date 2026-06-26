#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/analysis_out"
mkdir -p "$OUT"
for f in "$ROOT"/binaries/*.so; do
  base=$(basename "$f" .so)
  echo "[*] $base"
  file "$f" | tee "$OUT/$base.file.txt"
  readelf -h "$f" > "$OUT/$base.elf_header.txt"
  readelf -S "$f" > "$OUT/$base.sections.txt"
  readelf -d "$f" > "$OUT/$base.dynamic.txt"
  readelf -Ws "$f" > "$OUT/$base.symbols.txt"
  strings -a "$f" > "$OUT/$base.strings.txt"
  grep -Eio 'https?://[^ ]+|[a-zA-Z0-9._-]+\.(com|net|org|io|cn|ru)[^ ]*' "$OUT/$base.strings.txt" | sort -u > "$OUT/$base.urls_domains.txt" || true
  grep -Ei 'zygisk|dlopen|dlsym|pthread|socket|connect|send|recv|getaddrinfo|http|https|ssl|openssl|egl|gles|unity|vulkan|analytics|firebase|bugly|umeng|telemetry|/proc|maps' "$OUT/$base.strings.txt" > "$OUT/$base.interesting_strings.txt" || true
done
