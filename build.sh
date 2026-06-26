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

mkdir -p module/zygisk
for ABI in arm64-v8a armeabi-v7a; do
  echo "== building $ABI =="
  cmake -S jni -B "build/$ABI" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "build/$ABI" -j"$(nproc)"
  cp "build/$ABI/liblybfghook.so" "module/zygisk/$ABI.so"
done

rm -f cleanfg-magisk.zip
( cd module && zip -qr ../cleanfg-magisk.zip . -x "README_INSTALL.txt" )
echo "Done -> cleanfg-magisk.zip"
