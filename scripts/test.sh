#!/usr/bin/env bash
# Runs the host-side unit tests (no NDK or phone needed).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
sed "s|@BLOCK_MAP@|$PWD/data/java_to_bedrock.tsv.gz|" src/block_map_data.S.in > build/host-tests/block_map_data.S
CXXFLAGS=(-std=c++20 -O1 -g -Wall -Wextra -D_GLIBCXX_ASSERTIONS -Isrc)
CORE=(src/nbt.cpp src/mcstructure.cpp src/library.cpp src/mesh.cpp src/gzip.cpp src/blockmap.cpp src/litematic.cpp
      build/host-tests/block_map_data.S)
"${CXX:-c++}" "${CXXFLAGS[@]}" "${CORE[@]}" tests/mcstructure_test.cpp -lz -o build/host-tests/mcstructure_test
"${CXX:-c++}" "${CXXFLAGS[@]}" "${CORE[@]}" tests/litematic_test.cpp -lz -o build/host-tests/litematic_test
"${CXX:-c++}" "${CXXFLAGS[@]}" "${CORE[@]}" tools/litematic2mcstructure.cpp -lz -o build/host-tests/litematic2mcstructure
build/host-tests/mcstructure_test
build/host-tests/litematic_test tests/data/litematic
