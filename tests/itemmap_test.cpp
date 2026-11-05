// Host-side test: which item places a block when the item is not the block's own.
#include "itemmap.hpp"

#include <cstdio>
#include <string>

using bematica::itemmap::breakdown;
using bematica::itemmap::itemFor;
using bematica::itemmap::stackSize;

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
  CHECK(stackSize("stone") == 64);
  CHECK(stackSize("minecraft:stripped_spruce_wood") == 64);
  CHECK(stackSize("oak_hanging_sign") == 16);
  CHECK(stackSize("standing_banner") == 16);
  CHECK(stackSize("bed") == 1);
  CHECK(stackSize("red_shulker_box") == 1);
  CHECK(stackSize("water") == 1);
  {
    const auto b = breakdown(5134, 64);  // the Discord example: 2 shulkers + 26 stacks + 14
    CHECK(b.shulkers == 2 && b.stacks == 26 && b.rest == 14);
    const auto s = breakdown(100, 16);
    CHECK(s.shulkers == 0 && s.stacks == 6 && s.rest == 4);
    const auto u = breakdown(30, 1);
    CHECK(u.shulkers == 1 && u.stacks == 0 && u.rest == 3);
    const auto z = breakdown(14, 64);
    CHECK(z.shulkers == 0 && z.stacks == 0 && z.rest == 14);
  }
  CHECK(itemFor("minecraft:oak_planks").empty());
  CHECK(itemFor("minecraft:spruce_sign").empty());
  CHECK(itemFor("minecraft:spruce_wall_sign") == "minecraft:spruce_sign");
  CHECK(itemFor("minecraft:spruce_standing_sign") == "minecraft:spruce_sign");
  CHECK(itemFor("minecraft:wall_sign") == "minecraft:oak_sign");
  CHECK(itemFor("minecraft:standing_sign") == "minecraft:oak_sign");
  CHECK(itemFor("minecraft:darkoak_wall_sign") == "minecraft:dark_oak_sign");
  CHECK(itemFor("minecraft:pale_oak_standing_sign") == "minecraft:pale_oak_sign");
  CHECK(itemFor("minecraft:crimson_wall_sign") == "minecraft:crimson_sign");
  CHECK(itemFor("minecraft:spruce_hanging_sign").empty());
  CHECK(itemFor("minecraft:foowall_sign").empty());
  CHECK(itemFor("spruce_door").empty());
  CHECK(itemFor("other:spruce_door").empty());
  CHECK(itemFor("").empty());
  if (failures) return 1;
  std::puts("itemmap_test OK");
  return 0;
}
