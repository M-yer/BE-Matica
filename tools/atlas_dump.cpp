// Host tool: builds the texture atlas from a game apk / vanilla resource pack zip and writes pictures to check it by eye.
//   atlas_dump <apk or resource pack zip[,more splits...]> <out folder> [block names file]
// Writes atlas.png (the whole atlas), debug_faces.png (resolved faces, unfolded cubes) and atlas_cache.bin.
// With a block names file (one name per line) it lists the blocks that get no texture.
#include "atlas.hpp"
#include "texresolve.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "plutovg-stb-image-write.h"
#pragma GCC diagnostic pop

using namespace bematica;

namespace {

BlockState block(const char *name, std::initializer_list<std::pair<const char *, nbt::Tag>> states = {}) {
  BlockState b;
  b.name = name;
  b.states = nbt::makeCompound();
  for (const auto &[k, v] : states) nbt::put(b.states, k, v);
  return b;
}
nbt::Tag I(int v) { return nbt::makeInt(nbt::Type::Int, v); }
nbt::Tag S(const char *v) { return nbt::makeString(v); }

struct Case {
  const char *label;
  BlockState block;
  int rotation = 0;
  bool mirror = false;
};

constexpr int kScale = 3, kTile = 16 * kScale, kGap = 12;
constexpr int kCellW = 4 * kTile + kGap, kCellH = 3 * kTile + kGap, kColumns = 4;

struct Canvas {
  int w, h;
  std::vector<uint8_t> px;
  Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4, 40) {
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
  }
};

// One face as seen from outside, over a checkerboard so transparent pixels show.
void drawFace(Canvas &c, const TextureAtlas &atlas, const FaceTexture &t, int x0, int y0) {
  for (int y = 0; y < kTile; ++y)
    for (int x = 0; x < kTile; ++x) {
      float u, v;
      t.uv((x + 0.5f) / kTile, 1.0f - (y + 0.5f) / kTile, u, v);
      const int sx = std::min(static_cast<int>(u * atlas.width), atlas.width - 1);
      const int sy = std::min(static_cast<int>(v * atlas.height), atlas.height - 1);
      const uint8_t *s = &atlas.rgba[(static_cast<size_t>(sy) * atlas.width + sx) * 4];
      const int bg = (((x / 6) + (y / 6)) & 1) ? 90 : 130;
      uint8_t *d = &c.px[(static_cast<size_t>(y0 + y) * c.w + x0 + x) * 4];
      for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>((s[k] * s[3] + bg * (255 - s[3])) / 255);
    }
}

// Unfolded cube:      Up
//             West North East South
//                    Down
void drawCube(Canvas &c, const TextureAtlas &atlas, const Case &cs, int cellX, int cellY) {
  const TextureResolver resolver(atlas);
  struct Slot {
    Face face;
    int col, row;
  };
  const Slot slots[6] = {{Face::Up, 1, 0},   {Face::West, 0, 1}, {Face::North, 1, 1},
                         {Face::East, 2, 1}, {Face::South, 3, 1}, {Face::Down, 1, 2}};
  for (const Slot &s : slots) {
    FaceTexture t;
    if (resolver.resolve(cs.block, s.face, cs.rotation, cs.mirror, t)) drawFace(c, atlas, t, cellX + s.col * kTile, cellY + s.row * kTile);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::printf("usage: atlas_dump <apk or resource pack zip> <out folder> [block names file]\n");
    return 2;
  }
  const std::string out = argv[2];
  std::vector<std::string> archives;  // "a.apk,b.apk": an apk and its splits
  for (size_t from = 0, to; from <= std::string(argv[1]).size(); from = to + 1) {
    const std::string all = argv[1];
    to = all.find(',', from);
    if (to == std::string::npos) to = all.size();
    archives.push_back(all.substr(from, to - from));
  }
  ResourcePack pack;
  std::string error;
  if (!pack.open(archives, error)) {
    std::printf("cannot open pack: %s\n", error.c_str());
    return 1;
  }
  TextureAtlas atlas;
  const std::string cache = out + "/atlas_cache.bin", stamp = archiveStamp(archives);
  if (loadAtlas(cache, stamp, atlas)) {
    std::printf("atlas loaded from cache\n");
  } else {
    if (!buildAtlas(pack, stamp, atlas, error)) {
      std::printf("cannot build atlas: %s\n", error.c_str());
      return 1;
    }
    if (!error.empty()) std::printf("note: %s\n", error.c_str());
    std::printf("atlas built, cache %s\n", saveAtlas(atlas, cache) ? "written" : "NOT written");
  }
  std::printf("atlas %dx%d, %zu texture keys, %zu blocks\n", atlas.width, atlas.height, atlas.tiles.size(), atlas.blocks.size());
  stbi_write_png((out + "/atlas.png").c_str(), atlas.width, atlas.height, 4, atlas.rgba.data(), atlas.width * 4);

  const Case cases[] = {
      {"stone", block("minecraft:stone")},
      {"grass_block", block("minecraft:grass_block")},
      {"oak_leaves", block("minecraft:oak_leaves")},
      {"glass", block("minecraft:glass")},
      {"oak_log y", block("minecraft:oak_log", {{"pillar_axis", S("y")}})},
      {"oak_log x", block("minecraft:oak_log", {{"pillar_axis", S("x")}})},
      {"oak_log z", block("minecraft:oak_log", {{"pillar_axis", S("z")}})},
      {"oak_log x, hologram rotated 1", block("minecraft:oak_log", {{"pillar_axis", S("x")}}), 1},
      {"furnace facing_direction 2 (north)", block("minecraft:furnace", {{"facing_direction", I(2)}})},
      {"furnace facing_direction 3 (south)", block("minecraft:furnace", {{"facing_direction", I(3)}})},
      {"furnace facing_direction 4 (west)", block("minecraft:furnace", {{"facing_direction", I(4)}})},
      {"furnace facing_direction 5 (east)", block("minecraft:furnace", {{"facing_direction", I(5)}})},
      {"furnace south, hologram rotated 1", block("minecraft:furnace", {{"facing_direction", I(3)}}), 1},
      {"furnace east, hologram mirrored", block("minecraft:furnace", {{"facing_direction", I(5)}}), 0, true},
      {"furnace east, mirrored + rotated 1", block("minecraft:furnace", {{"facing_direction", I(5)}}), 1, true},
      {"lit_furnace cardinal west", block("minecraft:lit_furnace", {{"minecraft:cardinal_direction", S("west")}})},
      {"barrel facing_direction 1 (up)", block("minecraft:barrel", {{"facing_direction", I(1)}})},
      {"barrel facing_direction 2 (north)", block("minecraft:barrel", {{"facing_direction", I(2)}})},
      {"barrel facing_direction 5 (east)", block("minecraft:barrel", {{"facing_direction", I(5)}})},
      {"barrel up open", block("minecraft:barrel", {{"facing_direction", I(1)}, {"open_bit", I(1)}})},
      {"observer facing_direction 2", block("minecraft:observer", {{"facing_direction", I(2)}})},
      {"observer facing_direction 1", block("minecraft:observer", {{"facing_direction", I(1)}})},
      {"dispenser facing_direction 4", block("minecraft:dispenser", {{"facing_direction", I(4)}})},
      {"dispenser facing_direction 1", block("minecraft:dispenser", {{"facing_direction", I(1)}})},
      {"piston facing_direction 5", block("minecraft:piston", {{"facing_direction", I(5)}})},
      {"oak_stairs weirdo 2", block("minecraft:oak_stairs", {{"weirdo_direction", I(2)}})},
      {"carved_pumpkin cardinal north", block("minecraft:carved_pumpkin", {{"minecraft:cardinal_direction", S("north")}})},
      {"hay_block axis x", block("minecraft:hay_block", {{"pillar_axis", S("x")}})},
  };
  const int count = static_cast<int>(sizeof(cases) / sizeof(cases[0])), rows = (count + kColumns - 1) / kColumns;
  Canvas canvas(kColumns * kCellW, rows * kCellH);
  for (int i = 0; i < count; ++i) {
    drawCube(canvas, atlas, cases[i], (i % kColumns) * kCellW, (i / kColumns) * kCellH);
    std::printf("debug_faces.png cell %d (row %d, col %d): %s\n", i, i / kColumns, i % kColumns, cases[i].label);
  }
  stbi_write_png((out + "/debug_faces.png").c_str(), canvas.w, canvas.h, 4, canvas.px.data(), canvas.w * 4);
  std::printf("each cell is an unfolded cube seen from outside:  . Up .  /  West North East South  /  . Down .\n");

  if (argc > 3) {
    std::ifstream names(argv[3]);
    const TextureResolver resolver(atlas);
    int total = 0, missing = 0;
    for (std::string n; std::getline(names, n);) {
      if (n.empty()) continue;
      ++total;
      FaceTexture t;
      if (!resolver.resolve(block(n.c_str()), Face::Up, 0, false, t)) {
        ++missing;
        std::printf("no texture: %s\n", n.c_str());
      }
    }
    std::printf("%d of %d block names have a texture\n", total - missing, total);
  }
  return 0;
}
