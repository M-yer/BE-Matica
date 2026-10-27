#pragma once

#include "nbt.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bematica {

struct BlockState {
  std::string name;  // e.g. "minecraft:oak_stairs"
  nbt::Tag states;   // Compound of block state values
  int32_t version = 0;
};

// A Bedrock structure as saved by a structure block (.mcstructure).
struct Structure {
  std::array<int32_t, 3> size{};    // x, y, z
  std::array<int32_t, 3> origin{};  // world position it was saved from
  std::vector<BlockState> palette;
  // One palette index per block (-1 = leave the world block alone). Layer 1 holds the second
  // block in the same space, which is how Bedrock stores waterlogging.
  std::vector<int32_t> layers[2];

  size_t index(int x, int y, int z) const {
    return (static_cast<size_t>(x) * size[1] + y) * size[2] + z;
  }
  size_t volume() const { return static_cast<size_t>(size[0]) * size[1] * size[2]; }
};

bool loadMcstructure(std::span<const uint8_t> data, Structure &out, std::string &error);

}  // namespace bematica
