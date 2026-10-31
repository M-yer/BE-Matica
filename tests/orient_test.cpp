// Host-side test: directional state transforms for rotated / mirrored holograms.
#include "mcstructure.hpp"
#include "mesh.hpp"
#include "orient.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace bematica;
using orient::StateValue;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

int num(const char *block, const char *key, int v, int rot, bool mirror) {
  StateValue s{v, {}, false};
  orient::transformState(block, key, s, rot, mirror);
  return s.i;
}
std::string text(const char *block, const char *key, const char *v, int rot, bool mirror) {
  StateValue s{0, v, true};
  orient::transformState(block, key, s, rot, mirror);
  return s.s;
}

// World direction of structure `from` -> `to` (adjacent cells) under `view`, as a compass name.
std::string worldDir(const Structure &s, const View &view, int dx, int dz) {
  std::array<int, 3> a{-1, 0, -1}, b{-1, 0, -1};
  const auto size = viewSize(s, view);
  for (int x = 0; x < size[0]; ++x)
    for (int z = 0; z < size[2]; ++z) {
      const int32_t e = expectedAt(s, {0, 0, 0}, view, {x, 0, z});
      if (e == 0) a = {x, 0, z};
      if (e == 1) b = {x, 0, z};
    }
  (void)dx, (void)dz;
  const int ddx = b[0] - a[0], ddz = b[2] - a[2];
  return ddx == 1 ? "east" : ddx == -1 ? "west" : ddz == 1 ? "south" : "north";
}

}  // namespace

int main() {
  // Face-driven block entities: the clicked face is the (transformed) facing_direction.
  for (int rot = 0; rot < 4; ++rot)
    for (int mirror = 0; mirror < 2; ++mirror)
      for (int v = 0; v < 6; ++v) {
        StateValue s{v, {}, false};
        orient::transformState("minecraft:oak_wall_sign", "facing_direction", s, rot, mirror != 0);
        const int want = s.i >= 2 && s.i <= 5 ? s.i : -1;
        CHECK(orient::clickFace("minecraft:oak_wall_sign", "facing_direction", s) == want);
      }
  {
    StateValue north{2, {}, false}, east{5, {}, false}, up{1, {}, false}, text{0, "north", true};
    CHECK(orient::clickFace("wall_sign", "facing_direction", north) == 2);
    CHECK(orient::clickFace("minecraft:crimson_wall_sign", "minecraft:facing_direction", east) == 5);
    CHECK(orient::clickFace("wall_banner", "facing_direction", east) == 5);
    CHECK(orient::clickFace("skull", "facing_direction", north) == 2);
    CHECK(orient::clickFace("skull", "facing_direction", up) == -1);
    CHECK(orient::clickFace("barrel", "facing_direction", north) == -1);       // follows the player's look, not the face
    CHECK(orient::clickFace("standing_sign", "facing_direction", north) == -1);
    CHECK(orient::clickFace("wall_sign", "direction", north) == -1);
    CHECK(orient::clickFace("wall_sign", "facing_direction", text) == -1);
    StateValue r{2, {}, false};  // north rotated a quarter turn clockwise faces east
    orient::transformState("wall_sign", "facing_direction", r, 1, false);
    CHECK(orient::clickFace("wall_sign", "facing_direction", r) == 5);
  }
  // The transform must agree with where mesh.cpp puts the hologram: step along structure +x (east) / +z (south).
  for (int mirror = 0; mirror < 2; ++mirror)
    for (int rot = 0; rot < 4; ++rot) {
      const View view{.rotation = rot, .mirror = mirror != 0};
      Structure ex;
      ex.size = {2, 1, 1};
      ex.palette = {{"minecraft:a", {}}, {"minecraft:b", {}}};
      ex.layers[0] = {0, 1};
      Structure sz;
      sz.size = {1, 1, 2};
      sz.palette = ex.palette;
      sz.layers[0] = {0, 1};
      CHECK(worldDir(ex, view, 1, 0) == text("minecraft:fence_gate", "minecraft:cardinal_direction", "east", rot, mirror != 0));
      CHECK(worldDir(sz, view, 0, 1) == text("minecraft:fence_gate", "minecraft:cardinal_direction", "south", rot, mirror != 0));
    }

  // cardinal_direction: clockwise from above, mirror (east <-> west) first.
  CHECK(text("anvil", "minecraft:cardinal_direction", "north", 1, false) == "east");
  CHECK(text("anvil", "minecraft:cardinal_direction", "east", 1, false) == "south");
  CHECK(text("anvil", "minecraft:cardinal_direction", "west", 3, false) == "south");
  CHECK(text("anvil", "minecraft:cardinal_direction", "east", 0, true) == "west");
  CHECK(text("anvil", "minecraft:cardinal_direction", "north", 0, true) == "north");
  CHECK(text("anvil", "minecraft:cardinal_direction", "east", 1, true) == "north");  // mirror -> west, then -> north
  // Doors label their facing one quarter turn clockwise (Java east = Bedrock south), so a mirror swaps north/south labels.
  CHECK(text("oak_door", "minecraft:cardinal_direction", "south", 0, true) == "north");
  CHECK(text("oak_door", "minecraft:cardinal_direction", "east", 0, true) == "east");
  CHECK(text("oak_door", "minecraft:cardinal_direction", "south", 1, false) == "west");
  CHECK(num("oak_door", "door_hinge_bit", 0, 1, false) == 0);
  CHECK(num("oak_door", "door_hinge_bit", 0, 0, true) == 1);
  CHECK(num("oak_door", "door_hinge_bit", 1, 2, true) == 0);
  CHECK(text("small_dripleaf", "minecraft:cardinal_direction", "north", 0, true) == "south");  // label = Java - 1 turn

  // facing_direction: 0 down 1 up 2 north 3 south 4 west 5 east.
  CHECK(num("barrel", "facing_direction", 2, 1, false) == 5);
  CHECK(num("barrel", "facing_direction", 5, 1, false) == 3);
  CHECK(num("barrel", "facing_direction", 5, 0, true) == 4);
  CHECK(num("barrel", "facing_direction", 0, 3, true) == 0);
  CHECK(num("barrel", "facing_direction", 1, 2, false) == 1);
  CHECK(text("observer", "minecraft:facing_direction", "up", 1, true) == "up");
  CHECK(text("observer", "minecraft:facing_direction", "west", 2, false) == "east");

  // stairs: 0 east 1 west 2 south 3 north.
  CHECK(num("oak_stairs", "weirdo_direction", 0, 1, false) == 2);
  CHECK(num("oak_stairs", "weirdo_direction", 2, 1, false) == 1);
  CHECK(num("oak_stairs", "weirdo_direction", 0, 0, true) == 1);
  CHECK(num("oak_stairs", "weirdo_direction", 3, 0, true) == 3);
  CHECK(num("oak_stairs", "upside_down_bit", 1, 1, true) == 1);

  // direction depends on the block family.
  CHECK(num("oak_trapdoor", "direction", 0, 1, false) == 2);  // east -> south
  CHECK(num("oak_trapdoor", "direction", 0, 0, true) == 1);   // east -> west
  CHECK(num("white_bed", "direction", 0, 1, false) == 1);     // south -> west
  CHECK(num("loom", "direction", 3, 0, true) == 1);           // east -> west
  CHECK(num("bell", "direction", 0, 1, false) == 1);          // north -> east
  CHECK(num("repeater", "direction", 2, 1, true) == 2);       // unknown family untouched
  CHECK(num("oak_fence_gate", "direction", 2, 1, false) == 2);

  // axes
  CHECK(text("oak_log", "pillar_axis", "x", 1, false) == "z");
  CHECK(text("oak_log", "pillar_axis", "z", 3, true) == "x");
  CHECK(text("oak_log", "pillar_axis", "x", 2, true) == "x");
  CHECK(text("oak_log", "pillar_axis", "y", 1, false) == "y");
  CHECK(text("portal", "portal_axis", "x", 1, false) == "z");

  // ground_sign_direction: 0 south, +4 per clockwise quarter turn, mirror reflects.
  CHECK(num("standing_sign", "ground_sign_direction", 0, 1, false) == 4);
  CHECK(num("standing_sign", "ground_sign_direction", 14, 1, false) == 2);
  CHECK(num("standing_sign", "ground_sign_direction", 4, 0, true) == 12);
  CHECK(num("standing_sign", "ground_sign_direction", 0, 0, true) == 0);
  CHECK(num("standing_sign", "ground_sign_direction", 3, 1, true) == 17 % 16);  // 16-3=13, +4

  // rail_direction: 0 NS, 1 EW, 2..5 ascending E/W/N/S, 6 SE, 7 SW, 8 NW, 9 NE.
  CHECK(num("rail", "rail_direction", 0, 1, false) == 1);
  CHECK(num("rail", "rail_direction", 1, 2, false) == 1);
  CHECK(num("rail", "rail_direction", 2, 1, false) == 5);   // ascending east -> south
  CHECK(num("rail", "rail_direction", 2, 0, true) == 3);    // ascending east -> west
  CHECK(num("rail", "rail_direction", 6, 1, false) == 7);   // SE -> SW
  CHECK(num("rail", "rail_direction", 6, 0, true) == 7);    // SE mirrored -> SW
  CHECK(num("rail", "rail_direction", 9, 1, false) == 6);   // NE -> SE
  CHECK(num("powered_rail", "rail_direction", 4, 2, false) == 5);

  // wall torches, levers, vines, lichen, coral fans
  CHECK(text("wall_torch", "torch_facing_direction", "west", 1, false) == "north");
  CHECK(text("wall_torch", "torch_facing_direction", "top", 1, true) == "top");
  CHECK(text("lever", "lever_direction", "east", 1, false) == "south");
  CHECK(text("lever", "lever_direction", "up_east_west", 1, false) == "up_north_south");
  CHECK(text("lever", "lever_direction", "down_north_south", 0, true) == "down_north_south");
  CHECK(num("vine", "vine_direction_bits", 1, 1, false) == 2);    // south -> west
  CHECK(num("vine", "vine_direction_bits", 8, 0, true) == 2);     // east -> west
  CHECK(num("vine", "vine_direction_bits", 5, 2, false) == 5);    // south+north stay
  CHECK(num("glow_lichen", "multi_face_direction_bits", 1 | 2 | 16, 1, false) == (1 | 2 | 32));  // down, up, north -> east
  CHECK(num("tube_coral_wall_fan", "coral_direction", 2, 1, false) == 1);  // north -> east

  // identity and unknown states never change
  CHECK(num("barrel", "facing_direction", 3, 0, false) == 3);
  CHECK(num("barrel", "open_bit", 1, 1, true) == 1);
  CHECK(text("barrel", "some_mystery_state", "north", 1, true) == "north");
  CHECK(num("barrel", "facing_direction", 3, 4, false) == 3);

  std::printf(failures ? "%d failure(s)\n" : "all tests passed\n", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
