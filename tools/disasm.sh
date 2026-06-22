#!/usr/bin/env bash
# Dizassembler dlya libgppvppgfrcplussession.so cherez aarch64 binutils.
# Vydayot chistyj annotirovannyj listing klyuchevyh funkcij + vtable + typeinfo,
# chtoby reversnut ConfigParams / GPPBufferSlot / IGPPCallback.
set -euo pipefail

LIB="${1:-engine/libgppvppgfrcplussession.so}"
OUT="${2:-disasm_out}"
OD=aarch64-linux-gnu-objdump
RE=aarch64-linux-gnu-readelf
mkdir -p "$OUT"

echo "[*] lib: $LIB"
$OD --version | head -1

# polnyj dizasm (.text), demangled
echo "[*] full .text disasm -> $OUT/full_text.asm"
$OD -d -C --no-show-raw-insn "$LIB" > "$OUT/full_text.asm"

# tablica simvolov (demangled), s adresami i razmerami
echo "[*] symbols -> $OUT/symbols.txt"
$RE -sW "$LIB" | c++filt > "$OUT/symbols.txt"

# vydernut konkretnye funkcii po adresu+razmeru
extract() {
  local sym="$1"; local label="$2"
  local line addr size end
  line=$($RE -sW "$LIB" | awk -v s="$sym" '$8==s{print $2, $3; exit}')
  if [ -z "$line" ]; then echo "  [!] ne najden $label ($sym)"; return; fi
  addr=0x$(echo "$line" | awk '{print $1}')
  size=$(echo "$line" | awk '{print $2}')
  end=$(printf '0x%x' $(( addr + size )))
  echo "  [+] $label  $addr .. $end  ($size bajt)"
  {
    echo "=================================================================="
    echo "### $label"
    echo "### $sym"
    echo "### vaddr=$addr size=$size end=$end"
    echo "=================================================================="
    $OD -d -C --no-show-raw-insn --start-address="$addr" --stop-address="$end" "$LIB"
  } >> "$OUT/key_functions.asm"
}

: > "$OUT/key_functions.asm"
extract _ZN7android12GPPComponent6configEjjjRKNS_12ConfigParamsEb              "GPPComponent::config(uint,uint,uint,ConfigParams const&,bool)"
extract _ZN7android12GPPComponent9putBufferEbPNS_13GPPBufferSlotE              "GPPComponent::putBuffer(bool,GPPBufferSlot*)"
extract _ZN7android12GPPComponent4initERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEPNS_12IGPPCallbackE "GPPComponent::init(string const&,IGPPCallback*)"
extract _ZN7android12GPPComponent12processFrameEv                             "GPPComponent::processFrame()"
extract _ZN7android12GPPComponent11processInitEv                              "GPPComponent::processInit()"
extract _ZN7android12GPPComponentC2Ev                                        "GPPComponent::GPPComponent() [C2]"
extract _ZN7android12GPPComponentC1Ev                                        "GPPComponent::GPPComponent() [C1]"
extract _ZN7android12GPPComponent12emptyBuffersEb                            "GPPComponent::emptyBuffers(bool)"

# vtable GPPComponent + IGPPCallback: dump .data.rel.ro + relokacii (razresheniya ukazatelej v vtable)
echo "[*] vtables/typeinfo -> $OUT/vtables.txt"
{
  echo "===== RELA (vse, otfiltruem vtable/typeinfo) ====="
  $RE -rW "$LIB" | c++filt | grep -iE 'vtable|typeinfo|GPPComponent|IGPPCallback|GPPProducer|GPPSession' || true
  echo
  echo "===== .data.rel.ro hex dump ====="
  $OD -s -j .data.rel.ro "$LIB" || true
} > "$OUT/vtables.txt"

# stroki (imena propsov, soobsheniya) ryadom
$RE -p .rodata "$LIB" 2>/dev/null | grep -iE 'gpp|vpp|frc|interp|callback|config|buffer|format|width|height' > "$OUT/strings_rodata.txt" || true

echo "[*] gotovo. fajly v $OUT/:"
ls -l "$OUT"
