#pragma once

#include "mcstructure.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace bematica {

struct Vec3f {
  float x, y, z;
};

struct Quad {
  Vec3f v[4];
  float r, g, b;
};

struct Line {
  Vec3f a, b;
};

struct HologramMesh {
  std::vector<Quad> quads;  // block faces, world coordinates
  std::vector<Line> lines;  // outline of the whole structure
};

// Faces between two structure blocks are hidden, so only the outer shell of each solid area is drawn.
// Blocks are tinted by name so different materials are easy to tell apart.
HologramMesh buildMesh(const Structure &s, std::array<int, 3> origin, size_t maxQuads);

}  // namespace bematica
