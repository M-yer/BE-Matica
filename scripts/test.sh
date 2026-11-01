#!/usr/bin/env bash
# Runs the host-side unit tests (no NDK or phone needed).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
sed "s|@BLOCK_MAP@|$PWD/data/java_to_bedrock.tsv.gz|" src/block_map_data.S.in > build/host-tests/block_map_data.S
CXXFLAGS=(-std=c++20 -O1 -g -Wall -Wextra -D_GLIBCXX_ASSERTIONS -Isrc)
CORE=(src/nbt.cpp src/mcstructure.cpp src/library.cpp src/mesh.cpp src/gzip.cpp src/blockmap.cpp src/litematic.cpp
      src/texresolve.cpp build/host-tests/block_map_data.S)
"${CXX:-c++}" "${CXXFLAGS[@]}" "${CORE[@]}" tests/mcstructure_test.cpp -lz -o build/host-tests/mcstructure_test
"${CXX:-c++}" "${CXXFLAGS[@]}" "${CORE[@]}" tests/litematic_test.cpp -lz -o build/host-tests/litematic_test
"${CXX:-c++}" "${CXXFLAGS[@]}" "${CORE[@]}" tests/orient_test.cpp -lz -o build/host-tests/orient_test
"${CXX:-c++}" "${CXXFLAGS[@]}" tests/itemmap_test.cpp -o build/host-tests/itemmap_test
"${CXX:-c++}" "${CXXFLAGS[@]}" "${CORE[@]}" tools/litematic2mcstructure.cpp -lz -o build/host-tests/litematic2mcstructure
build/host-tests/mcstructure_test
build/host-tests/orient_test
build/host-tests/itemmap_test
build/host-tests/litematic_test tests/data/litematic

# Texture atlas / resolver. Needs the header-only libraries the Android build fetches (nlohmann/json, stb in lunasvg's
# plutovg: set BEMATICA_DEPS to that _deps folder if it is not build/_deps) and a vanilla resource pack zip standing in
# for the game's apk (set TEXTURE_TEST_PACK to e.g. the Mojang bedrock-samples zip).
DEPS="${BEMATICA_DEPS:-build/_deps}"
PACK="${TEXTURE_TEST_PACK:-/run/media/mayer/Primary/DevTools/downloads/bedrock-samples-1.26.50.4.zip}"
if [[ -d "$DEPS/nlohmann_json-src/include" && -d "$DEPS/lunasvg-src/plutovg/source" && -f "$PACK" ]]; then
  TEX=(src/zipfile.cpp src/image.cpp src/atlas.cpp src/texresolve.cpp src/nbt.cpp)
  TEXFLAGS=("${CXXFLAGS[@]}" -I"$DEPS/nlohmann_json-src/include" -I"$DEPS/lunasvg-src/plutovg/source")
  "${CXX:-c++}" "${TEXFLAGS[@]}" "${TEX[@]}" tests/texture_test.cpp -lz -o build/host-tests/texture_test
  "${CXX:-c++}" "${TEXFLAGS[@]}" "${TEX[@]}" tools/atlas_dump.cpp -lz -o build/host-tests/atlas_dump
  build/host-tests/texture_test "$PACK"
  mkdir -p build/debug-textures
  build/host-tests/atlas_dump "$PACK" build/debug-textures > build/debug-textures/log.txt
  echo "texture debug pictures: build/debug-textures/atlas.png, debug_faces.png"
  # The textured face renderer on a real GLES 3 context (Mesa, surfaceless EGL). Needs the GLES / EGL headers of the
  # Android NDK (set NDK) and the system libEGL / libGLESv2; skipped otherwise.
  GLSYS="${NDK:-/nonexistent}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include"
  if [[ -d "$GLSYS/GLES3" && -d "$GLSYS/EGL" ]] && ldconfig -p | grep -q "libGLESv2.so.2" && ldconfig -p | grep -q "libEGL.so.1"; then
    rm -rf build/host-tests/glinc && mkdir -p build/host-tests/glinc
    for d in EGL GLES2 GLES3 KHR; do ln -s "$GLSYS/$d" "build/host-tests/glinc/$d"; done
    "${CXX:-c++}" "${TEXFLAGS[@]}" -Ibuild/host-tests/glinc src/glquads.cpp src/mesh.cpp "${TEX[@]}" tests/glquads_test.cpp -lz \
      -l:libEGL.so.1 -l:libGLESv2.so.2 -o build/host-tests/glquads_test
    build/host-tests/glquads_test "$PACK" 2> build/host-tests/glquads_test.log
  else
    echo "SKIPPED glquads test: set NDK (for the GLES headers) and install Mesa's libEGL / libGLESv2"
  fi
else
  echo "SKIPPED texture tests: need $DEPS (nlohmann_json-src, lunasvg-src) and the pack zip $PACK"
fi
