// Host-side test: converts the sample .litematic files and checks the Bedrock result.
#include "blockmap.hpp"
#include "library.hpp"
#include "litematic.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::vector<uint8_t> readFile(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

size_t countBlocks(const Structure &s) {
  size_t n = 0;
  for (int32_t v : s.layers[0]) n += v >= 0;
  return n;
}

const nbt::Tag *state(const BlockState &bs, const char *key) { return bs.states.get(key); }

void testBlockMap() {
  const auto map = BlockMap::forBlocks({"minecraft:oak_stairs", "minecraft:grass", "minecraft:not_a_block"});
  CHECK(map.version() > 0);
  BlockState bs;
  CHECK(map.toBedrock("minecraft:oak_stairs",
                      {{"facing", "east"}, {"half", "top"}, {"shape", "straight"}, {"waterlogged", "false"}}, bs));
  CHECK(bs.name == "minecraft:oak_stairs");
  CHECK(state(bs, "upside_down_bit") && state(bs, "upside_down_bit")->type == nbt::Type::Byte &&
        state(bs, "upside_down_bit")->i == 1);
  CHECK(state(bs, "weirdo_direction") && state(bs, "weirdo_direction")->type == nbt::Type::Int);
  // Missing properties fall back to the closest state of the same block.
  CHECK(map.toBedrock("minecraft:oak_stairs", {{"facing", "east"}}, bs) && bs.name == "minecraft:oak_stairs");
  CHECK(map.toBedrock("minecraft:grass", {}, bs) && bs.name == "minecraft:short_grass");
  CHECK(!map.toBedrock("minecraft:not_a_block", {}, bs));
  CHECK(!map.toBedrock("minecraft:stone", {}, bs));  // in the table, but not asked for
}

}  // namespace

int main(int argc, char **argv) {
  testBlockMap();
  const std::filesystem::path dir = argc > 1 ? argv[1] : "tests/data/litematic";

  for (const char *file : {"demo", "simple", "tower", "mansion", "island"}) {
    Structure s;
    LitematicInfo info;
    std::string error;
    const bool ok = loadLitematic(readFile(dir / (std::string(file) + ".litematic")), s, info, error);
    if (!ok) std::printf("%s: %s\n", file, error.c_str());
    CHECK(ok);
    CHECK(info.unknownBlocks.empty());
    CHECK(countBlocks(s) > 0);

    // Saving and loading again gives back the same structure.
    Structure back;
    CHECK(loadMcstructure(saveMcstructure(s), back, error));
    CHECK(back.size == s.size);
    CHECK(back.palette.size() == s.palette.size());
    CHECK(back.layers[0] == s.layers[0] && back.layers[1] == s.layers[1]);
  }

  // demo.litematic: 4x3x4, 8 blocks (Metadata TotalBlocks), wool and a chest.
  Structure demo;
  LitematicInfo info;
  std::string error;
  CHECK(loadLitematic(readFile(dir / "demo.litematic"), demo, info, error));
  CHECK(info.name == "Demo Schematic" && info.regions == 1);
  CHECK((demo.size == std::array<int32_t, 3>{4, 3, 4}));
  CHECK(countBlocks(demo) == 8);
  bool wool = false, chest = false;
  for (const auto &m : countMaterials(demo)) {
    wool |= m.name == "white_wool";
    chest |= m.name == "chest";
  }
  CHECK(wool && chest);
  auto at = [&](int x, int y, int z) -> std::string {
    const int32_t v = demo.layers[0][demo.index(x, y, z)];
    return v < 0 ? "" : demo.palette[v].name;
  };
  CHECK(at(0, 0, 0) == "minecraft:white_wool");
  CHECK(at(1, 0, 0) == "minecraft:red_wool" && at(2, 0, 0) == "minecraft:red_wool" && at(3, 0, 0).empty());
  CHECK(at(0, 1, 0) == "minecraft:lime_wool" && at(0, 2, 0) == "minecraft:lime_wool");
  CHECK(at(0, 0, 1) == "minecraft:light_blue_wool" && at(0, 0, 2) == "minecraft:light_blue_wool" && at(0, 0, 3).empty());
  CHECK(at(1, 0, 1) == "minecraft:chest");
  const auto &chestState = demo.palette[demo.layers[0][demo.index(1, 0, 1)]];
  CHECK(state(chestState, "minecraft:cardinal_direction") &&
        state(chestState, "minecraft:cardinal_direction")->s == "south");

  // Damaged input fails cleanly.
  CHECK(!loadLitematic(std::vector<uint8_t>{0x1f, 0x8b, 0, 0}, demo, info, error));

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("litematic tests passed\n");
  return 0;
}
