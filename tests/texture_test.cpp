// Host-side test: game archive reader, atlas builder and cache, texture resolver (face, orientation, rotated holograms).
// Uses a real vanilla resource pack zip (bedrock-samples) as a stand-in for the game's apk:
//   texture_test <resource pack zip>
#include "atlas.hpp"
#include "image.hpp"
#include "texresolve.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace bematica;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

BlockState block(const char *name, std::initializer_list<std::pair<const char *, nbt::Tag>> states = {}) {
  BlockState b;
  b.name = name;
  b.states = nbt::makeCompound();
  for (const auto &[k, v] : states) nbt::put(b.states, k, v);
  return b;
}
nbt::Tag I(int v) { return nbt::makeInt(nbt::Type::Int, v); }
nbt::Tag S(const char *v) { return nbt::makeString(v); }

const TextureAtlas *gAtlas = nullptr;
const ResourcePack *gPack = nullptr;

bool same(const Tile &a, const Tile &b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
const Tile &tileOf(const char *key, size_t variant = 0) { return gAtlas->tiles.at(key).at(variant); }

const uint8_t *pixel(const Tile &t, int x, int y) {
  return &gAtlas->rgba[(static_cast<size_t>(t.y + y) * gAtlas->width + t.x + x) * 4];
}

bool resolve(const BlockState &b, Face f, FaceTexture &t, int rot = 0, bool mirror = false, bool carried = false) {
  return TextureResolver(*gAtlas, carried).resolve(b, f, rot, mirror, t);
}
bool is(const BlockState &b, Face f, const char *key, size_t variant = 0, int rot = 0, bool mirror = false) {
  FaceTexture t;
  return resolve(b, f, t, rot, mirror) && same(t.tile, tileOf(key, variant));
}
// Texture orientation on a face: upright (not turned or flipped) / a quarter turn (grain across the face).
bool upright(const BlockState &b, Face f, int rot = 0, bool mirror = false) {
  FaceTexture t;
  return resolve(b, f, t, rot, mirror) && t.turn[0][0] == 1 && t.turn[1][1] == 1;
}
// Upright or upside down: grain keeps running up and down the picture.
bool straight(const BlockState &b, Face f) {
  FaceTexture t;
  return resolve(b, f, t) && t.turn[0][1] == 0 && t.turn[1][0] == 0;
}
bool quarterTurn(const BlockState &b, Face f, int rot = 0, bool mirror = false) {
  FaceTexture t;
  return resolve(b, f, t, rot, mirror) && t.turn[0][0] == 0 && t.turn[1][1] == 0 && t.turn[0][1] != 0 && t.turn[1][0] != 0;
}

// The tile in the atlas equals the picture file in the pack (no tint, first frame).
bool matchesFile(const Tile &t, const char *file) {
  std::vector<uint8_t> raw;
  std::string err;
  Image img;
  if (!gPack->read(file, raw, err) || !decodeImage(raw, img) || img.w != t.w || img.h < t.h) return false;
  for (int y = 0; y < t.h; ++y)
    if (std::memcmp(pixel(t, 0, y), &img.rgba[static_cast<size_t>(y) * img.w * 4], static_cast<size_t>(t.w) * 4) != 0) return false;
  return true;
}

int countAlpha0(const Tile &t) {
  int n = 0;
  for (int y = 0; y < t.h; ++y)
    for (int x = 0; x < t.w; ++x) n += pixel(t, x, y)[3] == 0;
  return n;
}

// Average colour of the opaque pixels.
void average(const Tile &t, double rgb[3]) {
  rgb[0] = rgb[1] = rgb[2] = 0;
  int n = 0;
  for (int y = 0; y < t.h; ++y)
    for (int x = 0; x < t.w; ++x) {
      const uint8_t *p = pixel(t, x, y);
      if (p[3] == 0) continue;
      for (int c = 0; c < 3; ++c) rgb[c] += p[c];
      ++n;
    }
  for (int c = 0; c < 3; ++c) rgb[c] /= n ? n : 1;
}

void testTransformsOfFaces() {
  CHECK(worldFace(Face::East, 0, false) == Face::East);
  CHECK(worldFace(Face::East, 1, false) == Face::South);
  CHECK(worldFace(Face::North, 1, false) == Face::East);
  CHECK(worldFace(Face::West, 2, false) == Face::East);
  CHECK(worldFace(Face::East, 0, true) == Face::West);
  CHECK(worldFace(Face::North, 1, true) == Face::East);
  CHECK(worldFace(Face::East, 1, true) == Face::North);  // mirror first (east -> west), then a quarter turn (west -> north)
  CHECK(worldFace(Face::Up, 3, true) == Face::Up);

  float a, b;
  faceCoords(Face::Up, 1, 1, 0, a, b);  // top face: north is at the top, east at the right
  CHECK(a == 1 && b == 1);
  faceCoords(Face::South, 0, 1, 1, a, b);
  CHECK(a == 0 && b == 1);
  faceCoords(Face::North, 1, 0, 0, a, b);  // seen from the north, east is on the left
  CHECK(a == 0 && b == 0);
  faceCoords(Face::East, 1, 0, 0, a, b);  // seen from the east, north is on the right
  CHECK(a == 1 && b == 0);
  faceCoords(Face::Down, 0, 0, 1, a, b);
  CHECK(a == 0 && b == 1);

  FaceTexture t;
  CHECK(resolve(block("minecraft:stone"), Face::South, t));
  float u, v;
  t.uv(0, 1, u, v);  // top-left of the picture
  CHECK(u == t.u0 && v == t.v0);
  t.uv(1, 0, u, v);
  CHECK(u == t.u1 && v == t.v1);
}

void testSimpleBlocks() {
  const BlockState stone = block("minecraft:stone");
  for (int f = 0; f < 6; ++f) {
    CHECK(is(stone, static_cast<Face>(f), "flattened_stone"));
    CHECK(upright(stone, static_cast<Face>(f)));
  }
  CHECK(matchesFile(tileOf("flattened_stone"), "textures/blocks/stone.png"));

  const BlockState grass = block("minecraft:grass_block");
  FaceTexture top, side, bottom;
  CHECK(resolve(grass, Face::Up, top) && resolve(grass, Face::North, side) && resolve(grass, Face::Down, bottom));
  CHECK(is(grass, Face::Up, "grass_top") && is(grass, Face::East, "grass_side") && is(grass, Face::Down, "grass_bottom"));
  double rgb[3];
  average(top.tile, rgb);
  CHECK(rgb[1] > rgb[0] + 15 && rgb[1] > rgb[2] + 15);  // grey grass_top is tinted green
  CHECK(!matchesFile(top.tile, "textures/blocks/grass_top.png"));
  const uint8_t *fringe = pixel(side.tile, 8, 0), *dirt = pixel(side.tile, 8, 15);
  CHECK(fringe[1] > fringe[0] && fringe[1] > fringe[2] && fringe[3] == 255);  // tinted grass fringe on the side
  CHECK(dirt[0] > dirt[2] && dirt[3] == 255);                                  // brown dirt below it, opaque
  CHECK(matchesFile(bottom.tile, "textures/blocks/dirt.png"));
  FaceTexture carried;
  CHECK(resolve(grass, Face::Up, carried, 0, false, true) && matchesFile(carried.tile, "textures/blocks/grass_carried.png"));

  const BlockState glass = block("minecraft:glass");
  FaceTexture g;
  CHECK(resolve(glass, Face::Up, g) && countAlpha0(g.tile) > 0 && countAlpha0(g.tile) < 16 * 16);

  const BlockState leaves = block("minecraft:oak_leaves");
  FaceTexture l;
  CHECK(resolve(leaves, Face::East, l));
  average(l.tile, rgb);
  CHECK(countAlpha0(l.tile) > 0 && rgb[1] > rgb[0] + 15 && rgb[1] > rgb[2] + 15);

  const BlockState stairs0 = block("minecraft:oak_stairs", {{"weirdo_direction", I(0)}, {"upside_down_bit", I(0)}});
  for (int dir = 0; dir < 4; ++dir)
    for (int f = 0; f < 6; ++f) {
      const BlockState stairs = block("minecraft:oak_stairs", {{"weirdo_direction", I(dir)}});
      CHECK(is(stairs, static_cast<Face>(f), "wood_oak") && upright(stairs, static_cast<Face>(f)));
    }
  CHECK(is(stairs0, Face::Up, "wood_oak"));
  CHECK(!resolve(block("minecraft:not_a_block"), Face::Up, g));
}

void testLogs() {
  const Face sides[4] = {Face::North, Face::South, Face::West, Face::East};
  const BlockState y = block("minecraft:oak_log", {{"pillar_axis", S("y")}});
  CHECK(is(y, Face::Up, "oak_log_top") && is(y, Face::Down, "oak_log_top"));
  for (Face f : sides) CHECK(is(y, f, "oak_log_side") && upright(y, f));
  CHECK(matchesFile(tileOf("oak_log_side"), "textures/blocks/log_oak.png"));

  const BlockState x = block("minecraft:oak_log", {{"pillar_axis", S("x")}});
  CHECK(is(x, Face::East, "oak_log_top") && is(x, Face::West, "oak_log_top"));
  CHECK(is(x, Face::Up, "oak_log_side") && is(x, Face::North, "oak_log_side"));
  CHECK(quarterTurn(x, Face::Up) && quarterTurn(x, Face::Down) && quarterTurn(x, Face::North) && quarterTurn(x, Face::South));
  CHECK(upright(x, Face::East) || quarterTurn(x, Face::East));  // end caps carry no grain

  const BlockState z = block("minecraft:oak_log", {{"pillar_axis", S("z")}});
  CHECK(is(z, Face::North, "oak_log_top") && is(z, Face::South, "oak_log_top"));
  CHECK(is(z, Face::East, "oak_log_side") && is(z, Face::Up, "oak_log_side"));
  CHECK(straight(z, Face::Up) && straight(z, Face::Down));  // grain runs north-south on the top, as the picture's up
  CHECK(quarterTurn(z, Face::East) && quarterTurn(z, Face::West));

  // Rotating the hologram a quarter turn swaps x and z; mirroring leaves the axis alone.
  CHECK(is(x, Face::North, "oak_log_top", 0, 1, false) && quarterTurn(x, Face::East, 1, false));
  CHECK(is(z, Face::East, "oak_log_top", 0, 1, false));
  CHECK(is(x, Face::East, "oak_log_top", 0, 0, true));
  CHECK(is(z, Face::East, "oak_log_side", 0, 2, false) && quarterTurn(z, Face::East, 2, false));

  const BlockState basalt = block("minecraft:basalt", {{"pillar_axis", S("x")}});
  CHECK(is(basalt, Face::East, "basalt_top") && is(basalt, Face::Up, "basalt_side") && quarterTurn(basalt, Face::Up));
}

void testFurnaces() {
  const int world[4] = {2, 3, 4, 5};  // facing_direction north, south, west, east
  const Face face[4] = {Face::North, Face::South, Face::West, Face::East};
  const char *text[4] = {"north", "south", "west", "east"};
  for (int i = 0; i < 4; ++i) {
    const BlockState f = block("minecraft:furnace", {{"facing_direction", I(world[i])}});
    const BlockState c = block("minecraft:furnace", {{"minecraft:cardinal_direction", S(text[i])}});
    for (const BlockState *b : {&f, &c}) {
      for (int k = 0; k < 4; ++k) {
        CHECK(is(*b, face[k], k == i ? "furnace_front_off" : "furnace_side"));
        CHECK(upright(*b, face[k]));
      }
      CHECK(is(*b, Face::Up, "furnace_top") && is(*b, Face::Down, "furnace_top"));
    }
  }
  const BlockState lit = block("minecraft:lit_furnace", {{"facing_direction", I(4)}});
  CHECK(is(lit, Face::West, "furnace_front_on") && is(lit, Face::East, "furnace_side") && upright(lit, Face::West));

  // A hologram turned clockwise: a furnace facing south now faces west; two turns north.
  const BlockState south = block("minecraft:furnace", {{"facing_direction", I(3)}});
  CHECK(is(south, Face::West, "furnace_front_off", 0, 1) && is(south, Face::South, "furnace_side", 0, 1));
  CHECK(is(south, Face::North, "furnace_front_off", 0, 2));
  CHECK(is(south, Face::East, "furnace_front_off", 0, 3));
  // Mirrored along X: a furnace facing east faces west; mirrored then turned once it faces north.
  const BlockState east = block("minecraft:furnace", {{"facing_direction", I(5)}});
  CHECK(is(east, Face::West, "furnace_front_off", 0, 0, true) && is(east, Face::East, "furnace_side", 0, 0, true));
  CHECK(is(east, Face::North, "furnace_front_off", 0, 1, true) && upright(east, Face::North, 1, true));

  const BlockState disp = block("minecraft:dispenser", {{"facing_direction", I(2)}});
  CHECK(is(disp, Face::North, "dispenser_front_horizontal") && is(disp, Face::South, "dispenser_side"));
  const BlockState dispUp = block("minecraft:dispenser", {{"facing_direction", I(1)}});
  CHECK(is(dispUp, Face::Up, "dispenser_front_vertical") && is(dispUp, Face::Down, "dispenser_top") && is(dispUp, Face::East, "dispenser_side"));
  const BlockState dispDown = block("minecraft:dropper", {{"facing_direction", I(0)}});
  CHECK(is(dispDown, Face::Down, "dropper_front_vertical") && is(dispDown, Face::Up, "dropper_top"));

  for (int i = 0; i < 4; ++i) {  // observer: face (eyes) on the facing side, back opposite
    const BlockState o = block("minecraft:observer", {{"facing_direction", I(world[i])}});
    CHECK(is(o, face[i], "observer_north") && is(o, face[i ^ 1], "observer_south"));
  }
  const BlockState oUp = block("minecraft:observer", {{"facing_direction", I(1)}, {"powered_bit", I(1)}});
  CHECK(is(oUp, Face::Up, "observer_north") && is(oUp, Face::Down, "observer_south", 1));

  const BlockState piston = block("minecraft:piston", {{"facing_direction", I(5)}});
  CHECK(is(piston, Face::East, "piston_top") && is(piston, Face::West, "piston_bottom") && is(piston, Face::North, "piston_side"));
}

void testBarrels() {
  const BlockState up = block("minecraft:barrel", {{"facing_direction", I(1)}, {"open_bit", I(0)}});
  CHECK(is(up, Face::Up, "barrel_top") && is(up, Face::Down, "barrel_bottom") && is(up, Face::East, "barrel_side"));
  CHECK(upright(up, Face::East) && upright(up, Face::North));  // staves stand upright on the sides

  const BlockState north = block("minecraft:barrel", {{"facing_direction", I(2)}});
  CHECK(is(north, Face::North, "barrel_top") && is(north, Face::South, "barrel_bottom"));
  CHECK(is(north, Face::Up, "barrel_side") && is(north, Face::East, "barrel_side") && is(north, Face::Down, "barrel_side"));
  CHECK(straight(north, Face::Up) && straight(north, Face::Down));            // staves run north-south: along the picture's up
  CHECK(quarterTurn(north, Face::East) && quarterTurn(north, Face::West));  // seen from the side they run across the face

  const BlockState east = block("minecraft:barrel", {{"facing_direction", I(5)}});
  CHECK(is(east, Face::East, "barrel_top") && quarterTurn(east, Face::Up) && quarterTurn(east, Face::North));
  CHECK(is(north, Face::East, "barrel_top", 0, 1) && is(north, Face::West, "barrel_bottom", 0, 1));  // turned a quarter: top faces east

  const BlockState open = block("minecraft:barrel", {{"facing_direction", I(1)}, {"open_bit", I(1)}});
  CHECK(is(open, Face::Up, "barrel_top", 1) && !is(open, Face::Up, "barrel_top", 0));
}

void testCache() {
  const std::string path = "build/host-tests/texture_cache_test.bin";
  CHECK(saveAtlas(*gAtlas, path));
  TextureAtlas back;
  CHECK(loadAtlas(path, gAtlas->stamp, back));
  CHECK(back.width == gAtlas->width && back.height == gAtlas->height && back.rgba == gAtlas->rgba);
  CHECK(back.tiles.size() == gAtlas->tiles.size() && back.blocks.size() == gAtlas->blocks.size());
  CHECK(same(back.tiles.at("barrel_top").at(1), tileOf("barrel_top", 1)));
  CHECK(back.blocks.at("furnace").faces == gAtlas->blocks.at("furnace").faces);
  TextureAtlas other;
  CHECK(!loadAtlas(path, gAtlas->stamp + "x", other));  // another game build: rebuild
  CHECK(!loadAtlas(path + ".missing", gAtlas->stamp, other));
  std::remove(path.c_str());
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::printf("usage: texture_test <vanilla resource pack zip>\n");
    return 2;
  }
  ResourcePack pack;
  std::string error;
  if (!pack.open(argv[1], error)) {
    std::printf("FAIL open: %s\n", error.c_str());
    return 1;
  }
  gPack = &pack;
  TextureAtlas atlas;
  const auto t0 = std::chrono::steady_clock::now();
  if (!buildAtlas(pack, archiveStamp(argv[1]), atlas, error)) {
    std::printf("FAIL build: %s\n", error.c_str());
    return 1;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  std::printf("atlas %dx%d, %zu texture keys, %zu blocks, built in %lld ms%s%s\n", atlas.width, atlas.height, atlas.tiles.size(),
              atlas.blocks.size(), static_cast<long long>(ms), error.empty() ? "" : " (", error.empty() ? "" : (error + ")").c_str());
  gAtlas = &atlas;
  CHECK(atlas.width == 1024 && atlas.height <= 4096);

  testTransformsOfFaces();
  testSimpleBlocks();
  testLogs();
  testFurnaces();
  testBarrels();
  testCache();

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("texture_test: all passed\n");
  return 0;
}
