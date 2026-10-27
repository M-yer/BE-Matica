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

struct View {
  int rotation = 0;     // quarter turns clockwise seen from above, 0..3
  bool mirror = false;  // flip along X before rotating
  enum class Layers { All, Single, UpTo } layers = Layers::All;
  int layer = 0;        // structure Y level used by Single / UpTo
};

// Size of the structure after rotation (x and z swap on quarter turns).
std::array<int, 3> viewSize(const Structure &s, const View &v);

// Faces between two shown blocks are hidden, so only the outer shell of each solid area is drawn.
// Blocks are tinted by name so different materials are easy to tell apart. `origin` is the world
// position of the transformed structure's minimum corner.
HologramMesh buildMesh(const Structure &s, std::array<int, 3> origin, const View &view, size_t maxQuads);

// Palette index the structure wants at a world block, or -1 when the block is outside the structure,
// hidden by the layer view, or air / structure void.
int32_t expectedAt(const Structure &s, std::array<int, 3> origin, const View &view, std::array<int, 3> world);

}  // namespace bematica
