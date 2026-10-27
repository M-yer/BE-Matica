#!/usr/bin/env bash
# Runs the host-side unit tests (no NDK or phone needed).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -D_GLIBCXX_ASSERTIONS -Isrc \
  src/nbt.cpp src/mcstructure.cpp src/library.cpp src/mesh.cpp tests/mcstructure_test.cpp -o build/host-tests/mcstructure_test
build/host-tests/mcstructure_test
