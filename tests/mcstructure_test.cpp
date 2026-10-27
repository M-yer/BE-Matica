// Host-side test: builds .mcstructure bytes by hand and checks the loader.
#include "library.hpp"
#include "mesh.hpp"
#include "mcstructure.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

struct Writer {
  std::vector<uint8_t> b;
  template <typename T> void raw(T v) {
    uint8_t tmp[sizeof(T)];
    std::memcpy(tmp, &v, sizeof(T));
    b.insert(b.end(), tmp, tmp + sizeof(T));
  }
  void str(const std::string &s) {
    raw<uint16_t>(static_cast<uint16_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }
  void key(nbt::Type t, const std::string &name) {
    raw<uint8_t>(static_cast<uint8_t>(t));
    str(name);
  }
  void end() { raw<uint8_t>(0); }
  void intList(const std::string &name, const std::vector<int32_t> &v) {
    key(nbt::Type::List, name);
    intListPayload(v);
  }
  void intListPayload(const std::vector<int32_t> &v) {
    raw<uint8_t>(static_cast<uint8_t>(nbt::Type::Int));
    raw<int32_t>(static_cast<int32_t>(v.size()));
    for (int32_t x : v) raw<int32_t>(x);
  }
};

// 2x1x2 structure: stone, air, waterlogged stairs, untouched.
std::vector<uint8_t> sample(int badIndex = 0) {
  Writer w;
  w.key(nbt::Type::Compound, "");
  w.key(nbt::Type::Int, "format_version");
  w.raw<int32_t>(1);
  w.intList("size", {2, 1, 2});
  w.key(nbt::Type::Compound, "structure");
  {
    w.key(nbt::Type::List, "block_indices");
    w.raw<uint8_t>(static_cast<uint8_t>(nbt::Type::List));
    w.raw<int32_t>(2);
    w.intListPayload({0, 1, 2, badIndex ? badIndex : -1});
    w.intListPayload({-1, -1, 3, -1});
    w.key(nbt::Type::List, "entities");
    w.raw<uint8_t>(static_cast<uint8_t>(nbt::Type::Compound));
    w.raw<int32_t>(0);
    w.key(nbt::Type::Compound, "palette");
    w.key(nbt::Type::Compound, "default");
    w.key(nbt::Type::List, "block_palette");
    w.raw<uint8_t>(static_cast<uint8_t>(nbt::Type::Compound));
    w.raw<int32_t>(4);
    for (const char *name : {"minecraft:stone", "minecraft:air", "minecraft:oak_stairs", "minecraft:water"}) {
      w.key(nbt::Type::String, "name");
      w.str(name);
      w.key(nbt::Type::Compound, "states");
      if (std::string(name) == "minecraft:oak_stairs") {
        w.key(nbt::Type::Int, "weirdo_direction");
        w.raw<int32_t>(2);
        w.key(nbt::Type::Byte, "upside_down_bit");
        w.raw<int8_t>(1);
      }
      w.end();
      w.key(nbt::Type::Int, "version");
      w.raw<int32_t>(18168865);
      w.end();
    }
    w.key(nbt::Type::Compound, "block_position_data");
    w.end();
    w.end();  // default
    w.end();  // palette
  }
  w.end();  // structure
  w.intList("structure_world_origin", {10, 64, -5});
  w.end();  // root
  return w.b;
}

}  // namespace

// With file arguments: loads each real .mcstructure and prints a summary instead of running the tests.
int loadFiles(int argc, char **argv) {
  int bad = 0;
  for (int k = 1; k < argc; ++k) {
    std::FILE *f = std::fopen(argv[k], "rb");
    std::vector<uint8_t> data;
    if (f) {
      uint8_t buf[65536];
      for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
      std::fclose(f);
    }
    Structure s;
    std::string err;
    if (!f || !loadMcstructure(data, s, err)) {
      std::printf("FAIL %s: %s\n", argv[k], f ? err.c_str() : "cannot open");
      ++bad;
      continue;
    }
    size_t placed = 0, waterlogged = 0;
    for (size_t i = 0; i < s.volume(); ++i) {
      placed += s.layers[0][i] >= 0;
      waterlogged += s.layers[1][i] >= 0;
    }
    std::printf("ok   %dx%dx%d palette=%zu blocks=%zu layer1=%zu  %s\n", s.size[0], s.size[1], s.size[2],
                s.palette.size(), placed, waterlogged, argv[k]);
  }
  return bad ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  if (argc > 1) return loadFiles(argc, argv);
  std::string err;
  Structure s;

  auto good = sample();
  CHECK(loadMcstructure(good, s, err));
  CHECK(err.empty());
  CHECK(s.size[0] == 2 && s.size[1] == 1 && s.size[2] == 2);
  CHECK(s.origin[0] == 10 && s.origin[1] == 64 && s.origin[2] == -5);
  CHECK(s.palette.size() == 4);
  CHECK(s.palette[0].name == "minecraft:stone");
  CHECK(s.palette[0].version == 18168865);
  CHECK(s.layers[0][s.index(0, 0, 0)] == 0);
  CHECK(s.layers[0][s.index(0, 0, 1)] == 1);
  CHECK(s.layers[0][s.index(1, 0, 0)] == 2);
  CHECK(s.layers[0][s.index(1, 0, 1)] == -1);
  CHECK(s.layers[1][s.index(1, 0, 0)] == 3);
  const auto *dir = s.palette[2].states.get("weirdo_direction", nbt::Type::Int);
  CHECK(dir && dir->i == 2);
  const auto *upside = s.palette[2].states.get("upside_down_bit", nbt::Type::Byte);
  CHECK(upside && upside->i == 1);

  auto mats = countMaterials(s);
  CHECK(mats.size() == 3);  // stone, oak_stairs, water; air left out
  CHECK(mats.size() == 3 && mats[0].name == "oak_stairs" && mats[0].count == 1);

  // Mesh: stone at (0,0,0) and stairs at (1,0,0) touch, so each loses one face: 5 + 5 visible faces.
  auto mesh = buildMesh(s, {100, 64, -3}, {}, 1000);
  CHECK(mesh.quads.size() == 10);
  CHECK(mesh.lines.size() == 12);
  CHECK(mesh.quads[0].v[0].x >= 100 && mesh.quads[0].v[0].y >= 64);

  // Rotated or mirrored, every vertex must stay inside the (possibly swapped) bounding box.
  for (int rot = 0; rot < 4; ++rot)
    for (bool mirror : {false, true}) {
      View v{.rotation = rot, .mirror = mirror};
      auto m = buildMesh(s, {0, 0, 0}, v, 1000);
      auto size = viewSize(s, v);
      CHECK(m.quads.size() == 10);
      for (const auto &q : m.quads)
        for (const auto &p : q.v)
          CHECK(p.x >= 0 && p.x <= size[0] && p.y >= 0 && p.y <= size[1] && p.z >= 0 && p.z <= size[2]);
    }
  // A quarter turn moves the block at x=1 (stairs) onto z=1 in the rotated frame.
  {
    View v{.rotation = 1};
    auto m = buildMesh(s, {0, 0, 0}, v, 1000);
    bool stairsAtZ1 = false;
    for (const auto &q : m.quads)
      if (q.r == mesh.quads[5].r && q.g == mesh.quads[5].g && q.v[0].z >= 1) stairsAtZ1 = true;
    CHECK(stairsAtZ1);
  }
  // Single layer above the structure hides everything.
  CHECK(buildMesh(s, {0, 0, 0}, View{.layers = View::Layers::Single, .layer = 5}, 1000).quads.empty());

  // Every truncation of a valid file must fail cleanly, never crash.
  for (size_t n = 0; n < good.size(); ++n) {
    Structure t;
    CHECK(!loadMcstructure(std::span(good.data(), n), t, err));
  }

  CHECK(!loadMcstructure(sample(7), s, err));
  CHECK(err == "block index outside palette");

  // Entity-only structure: empty index lists and an empty palette compound, as Bedrock saves it.
  Writer e;
  e.key(nbt::Type::Compound, "");
  e.key(nbt::Type::Int, "format_version");
  e.raw<int32_t>(1);
  e.intList("size", {1, 1, 1});
  e.key(nbt::Type::Compound, "structure");
  e.key(nbt::Type::List, "block_indices");
  e.raw<uint8_t>(static_cast<uint8_t>(nbt::Type::List));
  e.raw<int32_t>(2);
  for (int k = 0; k < 2; ++k) {
    e.raw<uint8_t>(0);
    e.raw<int32_t>(0);
  }
  e.key(nbt::Type::Compound, "palette");
  e.end();
  e.end();  // structure
  e.end();  // root
  CHECK(loadMcstructure(e.b, s, err));
  CHECK(s.palette.empty() && s.layers[0].size() == 1 && s.layers[0][0] == -1 && s.layers[1][0] == -1);

  std::printf(failures ? "%d failure(s)\n" : "all tests passed\n", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
