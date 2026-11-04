#include "blockmap.hpp"
#include "library.hpp"

#include <cstdio>
#include <cstdlib>

using namespace bematica;

static int failures = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++failures; \
  } while (0)

static const BlockState &block(const char *name) {
  for (const auto &b : BlockMap::bedrockBlocks())
    if (b.name == name) return b;
  std::printf("missing %s\n", name);
  std::exit(1);
}

int main() {
  const auto &blocks = BlockMap::bedrockBlocks();
  CHECK(blocks.size() > 1000);
  for (size_t k = 1; k < blocks.size(); ++k) CHECK(blocks[k - 1].name < blocks[k].name);

  Structure s;
  s.size = {3, 1, 1};
  nbt::Tag stairs = nbt::makeCompound();
  nbt::put(stairs, "weirdo_direction", nbt::makeInt(nbt::Type::Int, 2));
  nbt::put(stairs, "upside_down_bit", nbt::makeInt(nbt::Type::Byte, 1));
  s.palette = {{"minecraft:smooth_stone", nbt::makeCompound(), 1}, {"minecraft:oak_stairs", stairs, 1}};
  s.layers[0] = {0, 0, 1};
  s.layers[1] = {-1, -1, -1};

  substituteBlock(s, "smooth_stone", block("minecraft:cobblestone"));
  CHECK(s.palette[0].name == "minecraft:cobblestone");
  substituteBlock(s, "oak_stairs", block("minecraft:stone_brick_stairs"));
  CHECK(s.palette[1].name == "minecraft:stone_brick_stairs");
  const nbt::Tag *dir = s.palette[1].states.get("weirdo_direction");
  const nbt::Tag *up = s.palette[1].states.get("upside_down_bit");
  CHECK(dir && dir->i == 2);  // the stairs keep facing the same way
  CHECK(up && up->i == 1);

  // Into a block without those states: the target's own states only.
  substituteBlock(s, "stone_brick_stairs", block("minecraft:stone"));
  CHECK(!s.palette[1].states.get("weirdo_direction"));

  const auto m = countMaterials(s);
  CHECK(m.size() == 2 && m[0].name == "cobblestone" && m[0].count == 2);
  if (failures) return 1;
  std::printf("substitute_test: ok (%zu Bedrock blocks)\n", blocks.size());
}
