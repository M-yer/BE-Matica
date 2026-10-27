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
};

// Blocks needed to build the structure, most-used first. Air and structure voids are left out.
std::vector<Material> countMaterials(const Structure &s);

}  // namespace bematica
