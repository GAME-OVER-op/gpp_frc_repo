#!/usr/bin/env bash
# Local build helper. CI builds the release zip automatically, but this script is
# useful when testing with a local Android NDK installation.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [ -z "${ANDROID_NDK:-}" ]; then
  echo "ERROR: set ANDROID_NDK to your NDK path" >&2
  exit 1
fi

if ! grep -q 'class ModuleBase' jni/zygisk.hpp 2>/dev/null; then
  echo "Fetching official zygisk.hpp ..."
  curl -fsSL "https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp" -o jni/zygisk.hpp
  grep -q REGISTER_ZYGISK_MODULE jni/zygisk.hpp || { echo "bad zygisk.hpp"; exit 1; }
fi

if ! command -v glslangValidator >/dev/null 2>&1; then
  echo "ERROR: glslangValidator is required to build jni/blend.comp" >&2
  exit 1
fi
glslangValidator -V jni/blend.comp --vn blend_comp_spv -o jni/blend_comp.h

patch_dobby() {
  local src="$1"
  [ -d "$src" ] || return 0
  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    echo "Patching Mach-O relocations in: $f"
    sed -i -E 's/([^[:space:],]+)@PAGEOFF/:lo12:\1/g' "$f"
    sed -i 's/@PAGE//g' "$f"
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
  cp "$BUILD_DIR/libgpp_frc_repo.so" "module/zygisk/$ABI.so"
  "$ANDROID_NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip "module/zygisk/$ABI.so" || true
done

rm -f gpp_frc_repo-magisk.zip
( cd module && zip -qr ../gpp_frc_repo-magisk.zip . -x "README_INSTALL.txt" )
echo "Done -> gpp_frc_repo-magisk.zip"
