#pragma once

#include "mcstructure.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bematica {

struct LitematicInfo {
  std::string name, author;
  int regions = 0;
  // Java blocks with no Bedrock equivalent; they are left out of the structure.
  std::vector<std::string> unknownBlocks;
};

// Converts a Litematica schematic (gzipped Java NBT) into a Bedrock structure. All regions are
// merged into one box. Block entity contents (chest items, sign text) and entities are not carried over.
bool loadLitematic(std::span<const uint8_t> data, Structure &out, LitematicInfo &info, std::string &error);

}  // namespace bematica
