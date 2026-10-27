// Host-side test: builds .mcstructure bytes by hand and checks the loader.
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
