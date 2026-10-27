#include "mesh.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace bematica {
namespace {

bool isEmpty(const std::string &name) {
  return name == "minecraft:air" || name == "minecraft:structure_void";
}

// Stable pastel color per block name (FNV-1a hash into hue).
void tint(const std::string &name, float &r, float &g, float &b) {
  uint32_t h = 2166136261u;
  for (unsigned char c : name) h = (h ^ c) * 16777619u;
  const float hue = (h % 360) / 60.0f;
  const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
  float rgb[3]{};
  switch (static_cast<int>(hue)) {
  case 0: rgb[0] = 1, rgb[1] = x; break;
  case 1: rgb[0] = x, rgb[1] = 1; break;
  case 2: rgb[1] = 1, rgb[2] = x; break;
  case 3: rgb[1] = x, rgb[2] = 1; break;
  case 4: rgb[0] = x, rgb[2] = 1; break;
  default: rgb[0] = 1, rgb[2] = x; break;
  }
  // Mix toward white so colors stay soft and readable over terrain.
  r = 0.45f + 0.55f * rgb[0];
  g = 0.45f + 0.55f * rgb[1];
  b = 0.45f + 0.55f * rgb[2];
}

// Corners of each face of the unit cube at (x, y, z), counter-clockwise seen from outside.
constexpr int kDir[6][3] = {{0, 1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
constexpr float kCorner[6][4][3] = {
    {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},  // up
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},  // down
    {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},  // north (-z)
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},  // south (+z)
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},  // west (-x)
    {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},  // east (+x)
};

}  // namespace

std::array<int, 3> viewSize(const Structure &s, const View &v) {
  return v.rotation % 2 ? std::array<int, 3>{s.size[2], s.size[1], s.size[0]} : s.size;
}

HologramMesh buildMesh(const Structure &s, std::array<int, 3> origin, const View &view, size_t maxQuads) {
  HologramMesh mesh;
  const int sx = s.size[0], sy = s.size[1], sz = s.size[2];
  const int rot = ((view.rotation % 4) + 4) % 4;

  auto shownLayer = [&](int y) {
    switch (view.layers) {
    case View::Layers::Single: return y == view.layer;
    case View::Layers::UpTo: return y <= view.layer;
    default: return true;
    }
  };

  std::vector<bool> solid(s.volume());
  std::vector<std::array<float, 3>> colors(s.palette.size());
  std::vector<bool> paletteEmpty(s.palette.size());
  for (size_t k = 0; k < s.palette.size(); ++k) {
    paletteEmpty[k] = isEmpty(s.palette[k].name);
    tint(s.palette[k].name, colors[k][0], colors[k][1], colors[k][2]);
  }
  for (int x = 0; x < sx; ++x)
    for (int y = 0; y < sy; ++y)
      for (int z = 0; z < sz; ++z) {
        const size_t i = s.index(x, y, z);
        const int32_t idx = s.layers[0][i];
        solid[i] = idx >= 0 && !paletteEmpty[idx] && shownLayer(y);
      }

  // Structure-local point (0..sx, 0..sy, 0..sz) to world, applying mirror then rotation.
  auto toWorld = [&](float x, float y, float z) {
    if (view.mirror) x = sx - x;
    float w = sx, d = sz;
    for (int r = 0; r < rot; ++r) {  // clockwise from above: +x -> +z
      const float nx = d - z, nz = x;
      x = nx, z = nz;
      std::swap(w, d);
    }
    return Vec3f{origin[0] + x, origin[1] + y, origin[2] + z};
  };

  for (int x = 0; x < sx; ++x)
    for (int y = 0; y < sy; ++y)
      for (int z = 0; z < sz; ++z) {
        const size_t i = s.index(x, y, z);
        if (!solid[i]) continue;
        const auto &c = colors[s.layers[0][i]];
        for (int f = 0; f < 6; ++f) {
          const int nx = x + kDir[f][0], ny = y + kDir[f][1], nz = z + kDir[f][2];
          const bool inside = nx >= 0 && ny >= 0 && nz >= 0 && nx < sx && ny < sy && nz < sz;
          if (inside && solid[s.index(nx, ny, nz)]) continue;
          if (mesh.quads.size() >= maxQuads) goto outline;
          Quad q{};
          for (int k = 0; k < 4; ++k) {
            // Mirroring flips the winding; reverse the corners so faces still point outward.
            const int c4 = view.mirror ? 3 - k : k;
            q.v[k] = toWorld(x + kCorner[f][c4][0], y + kCorner[f][c4][1], z + kCorner[f][c4][2]);
          }
          q.r = c[0], q.g = c[1], q.b = c[2];
          mesh.quads.push_back(q);
        }
      }

outline:
  const auto size = viewSize(s, view);
  const float x0 = origin[0], y0 = origin[1], z0 = origin[2];
  const float x1 = x0 + size[0], y1 = y0 + size[1], z1 = z0 + size[2];
  const Vec3f p[8] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1},
                      {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
  constexpr int kEdge[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto &e : kEdge) mesh.lines.push_back({p[e[0]], p[e[1]]});
  // Outline the shown layer too, so it's easy to find while building.
  if (view.layers != View::Layers::All) {
    const float ly0 = y0 + view.layer, ly1 = ly0 + 1;
    const Vec3f q[8] = {{x0, ly0, z0}, {x1, ly0, z0}, {x1, ly0, z1}, {x0, ly0, z1},
                        {x0, ly1, z0}, {x1, ly1, z0}, {x1, ly1, z1}, {x0, ly1, z1}};
    for (const auto &e : kEdge) mesh.lines.push_back({q[e[0]], q[e[1]]});
  }
  return mesh;
}

}  // namespace bematica
