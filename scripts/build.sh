#!/usr/bin/env bash
# Builds release/BE-Matica.levipack. Requires NDK r28c; set NDK=/path/to/android-ndk-r28c.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${NDK:?Set NDK to your Android NDK r28c directory}"
git submodule update --init --recursive
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  "$@"
cmake --build build --target BEMatica
python3 scripts/package.py build/libBEMatica.so manifest.json release/BE-Matica.levipack
