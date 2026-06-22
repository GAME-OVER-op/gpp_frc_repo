#!/usr/bin/env bash
# Dizassembler dlya RODNOJ libvpplibrary.so ustrojstva (SM8650) cherez aarch64 binutils.
# Cel: tochno reversnut publichnyj VPP C-API (vpp_init/vpp_open/vpp_boot/vpp_set_ctrl/
# vpp_set_parameter/vpp_get_buf_requirements/vpp_queue_buf) - poryadok init i ABI struktur,
# chtoby postroit native-VPP FRC harness (Phase 19) bez ugadyvaniya.
set -euo pipefail

LIB="${1:-vpplib/libvpplibrary.so}"
OUT="${2:-disasm_vpp_out}"
OD=aarch64-linux-gnu-objdump
RE=aarch64-linux-gnu-readelf
OC=aarch64-linux-gnu-objcopy
mkdir -p "$OUT"

echo "[*] lib: $LIB"
$OD --version | head -1

# polnyj dizasm (.text), demangled
echo "[*] full .text disasm -> $OUT/full_text.asm"
$OD -d -C --no-show-raw-insn "$LIB" > "$OUT/full_text.asm"

# dinamicheskie + lokalnye simvoly (demangled)
echo "[*] dynsym -> $OUT/symbols.txt"
$RE -sW "$LIB" | c++filt > "$OUT/symbols.txt"

# .gnu_debugdata (MiniDebugInfo, xz) -> dop. simvoly
echo "[*] .gnu_debugdata -> $OUT/debugdata_symbols.txt"
if $OC -O binary --only-section=.gnu_debugdata "$LIB" "$OUT/.dbg.xz" 2>/dev/null && [ -s "$OUT/.dbg.xz" ]; then
  xz -d -f "$OUT/.dbg.xz" 2>/dev/null || true
  if [ -f "$OUT/.dbg" ]; then
    $RE -sW "$OUT/.dbg" 2>/dev/null | c++filt > "$OUT/debugdata_symbols.txt" || true
    rm -f "$OUT/.dbg"
  fi
else
  echo "(net .gnu_debugdata)" > "$OUT/debugdata_symbols.txt"
fi

# vydernut funkciyu po simvolu; dlya thunk'ov (malenkij size) dat okno 0x240
extract() {
  local sym="$1"; local label="$2"
  local line addr size end win
  line=$($RE -sW "$LIB" | awk -v s="$sym" '$8==s{print $2, $3; exit}')
  if [ -z "$line" ]; then echo "  [!] ne najden $label ($sym)"; return; fi
  addr=0x$(echo "$line" | awk '{print $1}')
  size=$(echo "$line" | awk '{print $2}')
  win=$size; if [ "$win" -lt 96 ]; then win=576; fi
  end=$(printf '0x%x' $(( addr + win )))
  echo "  [+] $label  $addr .. $end  (sym_size=$size, dump=$win)"
  {
    echo "=================================================================="
    echo "### $label    sym=$sym  vaddr=$addr sym_size=$size dump_end=$end"
    echo "=================================================================="
    $OD -d -C --no-show-raw-insn --start-address="$addr" --stop-address="$end" "$LIB"
  } >> "$OUT/vpp_api.asm"
}

: > "$OUT/vpp_api.asm"
for s in vpp_boot vpp_init vpp_open vpp_set_ctrl vpp_set_parameter \
         vpp_get_buf_requirements vpp_queue_buf vpp_reconfigure \
         vpp_drain vpp_flush vpp_close vpp_shutdown vpp_set_vid_prop; do
  extract "$s" "$s"
done

# stroki, raskryvayushie ABI: buffer/port/format/frc/hqv/requirements/extradata
echo "[*] strings -> $OUT/strings_vpp.txt"
$RE -p .rodata "$LIB" 2>/dev/null | grep -iE 'vpp_|VPP_|hqv|HQV|frc|FRC|interp|buffer|Buf\[|FilledLen|ValidLen|AllocLen|Required|extradata|ExtraData|port|Port|format|fmt|width|height|stride|scanline|requirement|VPP_COLOR|NV12|UBWC|P010|callback|Callback|session|tuning' > "$OUT/strings_vpp.txt" || true

echo "[*] gotovo. fajly v $OUT/:"
ls -l "$OUT"
