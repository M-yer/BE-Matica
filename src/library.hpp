#pragma once

#include "mcstructure.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace bematica {

// All .mcstructure files directly inside the given folders, sorted by file name.
std::vector<std::filesystem::path> findStructures(const std::vector<std::filesystem::path> &dirs);

bool loadStructureFile(const std::filesystem::path &file, Structure &out, std::string &error);

struct Material {
  std::string name;  // without the "minecraft:" prefix
  size_t count = 0;
  std::string was;  // the structure's own blocks this one replaces (substitution), comma separated
};

// Blocks needed to build the structure, most-used first. Air and structure voids are left out.
std::vector<Material> countMaterials(const Structure &s);

// Every block named `from` (without "minecraft:") becomes `to`. States that `to` also has keep the old block's value,
// so stairs stay turned the same way; the others come from `to`.
void substituteBlock(Structure &s, const std::string &from, const BlockState &to);

// Powered repeaters, comparators, unlit torches and lit lamps become the form their item places (see itemmap::placedForm),
// so they count, match the held item and get oriented like the rest. The comparator's lit bit is cleared with it.
void unpowerBlocks(Structure &s);

}  // namespace bematica
