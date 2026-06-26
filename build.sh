#!/usr/bin/env bash
# Local build helper (optional). CI does this automatically via
# .github/workflows/build.yml — you normally do NOT need to run this.
#
# Requirements for local builds:
#   - Android NDK (set ANDROID_NDK to its path)
#   - cmake + ninja
#   - internet access (FetchContent pulls Dobby; we also fetch zygisk.hpp)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [ -z "${ANDROID_NDK:-}" ]; then
  echo "ERROR: set ANDROID_NDK to your NDK path" >&2
  exit 1
fi

# Fetch the official Zygisk header (same as CI).
if ! grep -q 'class ModuleBase' jni/zygisk.hpp 2>/dev/null; then
  echo "Fetching official zygisk.hpp ..."
  curl -fsSL "https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp" -o jni/zygisk.hpp
  grep -q REGISTER_ZYGISK_MODULE jni/zygisk.hpp || { echo "bad zygisk.hpp"; exit 1; }
fi

patch_dobby() {
  # Dobby's arm64 closure-bridge asm uses Apple Mach-O relocation syntax
  # (@PAGE / @PAGEOFF) that the NDK ELF assembler rejects. Rewrite to ELF form.
  local src="$1"
  [ -d "$src" ] || return 0
  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    echo "Patching Mach-O relocations in: $f"
    sed -i -E 's/([A-Za-z_.$][A-Za-z0-9_.$]*)@PAGEOFF/:lo12:\1/g' "$f"
    sed -i -E 's/([A-Za-z_.$][A-Za-z0-9_.$]*)@PAGE/\1/g' "$f"
  done < <(grep -rlE '@PAGEOFF|@PAGE' "$src" || true)
}

mkdir -p module/zygisk
for ABI in arm64-v8a armeabi-v7a; do
  echo "== building $ABI =="
  BUILD_DIR="build/$ABI"
  cmake -S jni -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  patch_dobby "$BUILD_DIR/_deps/dobby-src"
  cmake --build "$BUILD_DIR" -j"$(nproc)"
  cp "$BUILD_DIR/liblybfghook.so" "module/zygisk/$ABI.so"
done

rm -f cleanfg-magisk.zip
( cd module && zip -qr ../cleanfg-magisk.zip . -x "README_INSTALL.txt" )
echo "Done -> cleanfg-magisk.zip"
