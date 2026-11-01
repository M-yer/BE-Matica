#pragma once

#include "mcstructure.hpp"

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace bematica {

class TextureResolver;

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

struct BlockCell {
  int x, y, z;      // world block position
  int32_t palette;  // index into the structure palette
};

// Texture coordinates of one Quad, same corner order. u < 0: the face has no texture, draw its tint.
struct QuadUv {
  float uv[4][2];
  float shade;  // brightness of the face by its direction, like the game's own
};

struct HologramMesh {
  uint64_t generation = 0;        // unique per built mesh, so cached GPU copies can tell meshes apart
  bool textured = false;          // built with a TextureResolver: `uvs` is parallel to `quads`
  std::vector<QuadUv> uvs;
  std::vector<Quad> quads;        // block faces, world coordinates
  std::vector<Line> lines;        // outline of the whole structure
  std::vector<BlockCell> blocks;  // blocks with at least one visible face, for the textured view
  std::vector<Line> blockLines;   // edges of those blocks (shared edges once), for the outline style
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
// `placed` (optional) holds world positions (packWorld) already built correctly; those blocks are left out entirely.
// `textures` (optional) also fills `uvs`: the block's own texture on every face, turned to match the rotated / mirrored view.
HologramMesh buildMesh(const Structure &s, std::array<int, 3> origin, const View &view, size_t maxQuads,
                       const std::unordered_set<uint64_t> *placed = nullptr, const TextureResolver *textures = nullptr);

inline uint64_t packWorld(int x, int y, int z) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFF) << 42) |
         (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFF) << 21) | (static_cast<uint32_t>(z) & 0x1FFFFF);
}

// Palette index the structure wants at a world block, or -1 when the block is outside the structure,
// hidden by the layer view, or air / structure void.
int32_t expectedAt(const Structure &s, std::array<int, 3> origin, const View &view, std::array<int, 3> world);

}  // namespace bematica
