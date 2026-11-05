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
  // Redstone saved while powered loads as the form the item places (a cannon saved with its circuit running).
  Structure r;
  nbt::Tag comp = nbt::makeCompound();
  nbt::put(comp, "minecraft:cardinal_direction", nbt::makeString("west"));
  nbt::put(comp, "output_lit_bit", nbt::makeInt(nbt::Type::Byte, 1));
  nbt::put(comp, "output_subtract_bit", nbt::makeInt(nbt::Type::Byte, 1));
  r.palette = {{"minecraft:powered_comparator", comp, 1},
               {"minecraft:powered_repeater", nbt::makeCompound(), 1},
               {"minecraft:unlit_redstone_torch", nbt::makeCompound(), 1},
               {"minecraft:unpowered_repeater", nbt::makeCompound(), 1}};
  unpowerBlocks(r);
  CHECK(r.palette[0].name == "minecraft:unpowered_comparator");
  CHECK(r.palette[0].states.get("output_lit_bit")->i == 0);
  CHECK(r.palette[0].states.get("output_subtract_bit")->i == 1);
  CHECK(r.palette[0].states.get("minecraft:cardinal_direction")->s == "west");
  CHECK(r.palette[1].name == "minecraft:unpowered_repeater");
  CHECK(r.palette[2].name == "minecraft:redstone_torch");
  CHECK(r.palette[3].name == "minecraft:unpowered_repeater");

  if (failures) return 1;
  std::printf("substitute_test: ok (%zu Bedrock blocks)\n", blocks.size());
}
