// Host-side test: which item places a block when the item is not the block's own.
#include "itemmap.hpp"

#include <cstdio>
#include <string>

using bematica::itemmap::itemFor;

namespace {
int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)
}  // namespace

int main() {
  for (const char *door : {"wooden_door", "iron_door", "spruce_door", "dark_oak_door", "cherry_door", "bamboo_door",
                           "crimson_door", "copper_door", "exposed_copper_door", "waxed_oxidized_copper_door"})
    CHECK(itemFor(std::string("minecraft:") + door) == std::string("minecraft:") + door);
  CHECK(itemFor("minecraft:spruce_trapdoor").empty());
  CHECK(itemFor("minecraft:iron_trapdoor").empty());
  CHECK(itemFor("minecraft:_door").empty());
  CHECK(itemFor("minecraft:water") == "minecraft:water_bucket");
  CHECK(itemFor("minecraft:flowing_water") == "minecraft:water_bucket");
  CHECK(itemFor("minecraft:lava") == "minecraft:lava_bucket");
  CHECK(itemFor("minecraft:flowing_lava") == "minecraft:lava_bucket");
  CHECK(itemFor("minecraft:reeds") == "minecraft:sugar_cane");
  CHECK(itemFor("minecraft:unpowered_comparator") == "minecraft:comparator");
  CHECK(itemFor("minecraft:redstone_wire") == "minecraft:redstone");
  CHECK(itemFor("minecraft:carrots") == "minecraft:carrot");
  CHECK(itemFor("minecraft:stone").empty());
  CHECK(itemFor("minecraft:oak_planks").empty());
  CHECK(itemFor("minecraft:spruce_sign").empty());
  CHECK(itemFor("spruce_door").empty());
  CHECK(itemFor("other:spruce_door").empty());
  CHECK(itemFor("").empty());
  if (failures) return 1;
  std::puts("itemmap_test OK");
  return 0;
}
