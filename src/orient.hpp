// Rotates / mirrors the directional block states of a structure block so a placed block faces the way the transformed
// hologram shows it. Pure functions, no game types. Encodings come from data/java_to_bedrock.tsv.gz; a state or block
// family that is not listed here is left untouched on purpose (never guess).
#pragma once

#include <array>
#include <string>
#include <string_view>

namespace bematica::orient {

struct StateValue {
  int i = 0;
  std::string s;
  bool isText = false;
};

namespace detail {

// Compass order is clockwise seen from above, the same sense as View::rotation. Mirror flips X (east <-> west) first.
enum Dir { N = 0, E = 1, S = 2, W = 3 };

inline int turn(int d, int rotation, bool mirror) {
  if (mirror && (d == E || d == W)) d = d == E ? W : E;
  return (d + rotation) & 3;
}

inline bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Bedrock enum value <-> Dir, as a table indexed by the Bedrock value (-1: not a horizontal direction).
template <size_t K> bool turnIndex(const std::array<int, K> &byValue, int &value, int rotation, bool mirror) {
  if (value < 0 || value >= static_cast<int>(K) || byValue[value] < 0) return false;
  const int d = turn(byValue[value], rotation, mirror);
  for (size_t v = 0; v < K; ++v)
    if (byValue[v] == d) return value = static_cast<int>(v), true;
  return false;
}

inline int dirFromName(std::string_view n) {
  return n == "north" ? N : n == "east" ? E : n == "south" ? S : n == "west" ? W : -1;
}
inline const char *dirName(int d) {
  static constexpr const char *names[] = {"north", "east", "south", "west"};
  return names[d & 3];
}

// Text value naming a horizontal direction; `offset` is how many clockwise quarter turns the Bedrock label sits from the
// Java facing (doors are +1, small_dripleaf is -1), which matters because mirroring is defined on the Java facing.
inline bool turnName(std::string &text, int offset, int rotation, bool mirror) {
  const int label = dirFromName(text);
  if (label < 0) return false;
  text = dirName(turn((label - offset) & 3, rotation, mirror) + offset);
  return true;
}

// Four horizontal bits of a bitmask, ordered by Dir; vines and glow lichen share the idea with different bit positions.
inline int turnBits(int value, const std::array<int, 4> &bitOfDir, int rotation, bool mirror) {
  int out = value;
  for (int d = 0; d < 4; ++d) out &= ~bitOfDir[d];
  for (int d = 0; d < 4; ++d)
    if (value & bitOfDir[d]) out |= bitOfDir[turn(d, rotation, mirror)];
  return out;
}

// rail_direction 0..9: 0 north_south, 1 east_west, 2..5 ascending east/west/north/south, 6..9 curves.
inline int turnRail(int v, int rotation, bool mirror) {
  if (v == 0 || v == 1) return (rotation & 1) ? 1 - v : v;
  if (v >= 2 && v <= 5) {
    static constexpr int dirOf[] = {E, W, N, S};
    const int d = turn(dirOf[v - 2], rotation, mirror);
    for (int k = 0; k < 4; ++k)
      if (dirOf[k] == d) return 2 + k;
  }
  if (v >= 6 && v <= 9) {
    static constexpr int curve[4][2] = {{S, E}, {S, W}, {N, W}, {N, E}};
    const int a = turn(curve[v - 6][0], rotation, mirror), b = turn(curve[v - 6][1], rotation, mirror);
    for (int k = 0; k < 4; ++k)
      if ((curve[k][0] == a && curve[k][1] == b) || (curve[k][0] == b && curve[k][1] == a)) return 6 + k;
  }
  return v;
}

}  // namespace detail

// `block` is the Bedrock block name (with or without "minecraft:"), `key` the state name. `rotation` is quarter turns
// clockwise seen from above (any int), `mirror` flips X before rotating (same convention as View in mesh.hpp).
// Returns true when the value was changed.
inline bool transformState(std::string_view block, std::string_view key, StateValue &v, int rotation, bool mirror) {
  using namespace detail;
  rotation = ((rotation % 4) + 4) & 3;
  if (rotation == 0 && !mirror) return false;
  constexpr std::string_view prefix = "minecraft:";
  if (block.substr(0, prefix.size()) == prefix) block.remove_prefix(prefix.size());
  if (key.substr(0, prefix.size()) == prefix) key.remove_prefix(prefix.size());
  const StateValue before = v;

  if (key == "cardinal_direction") {
    if (!v.isText) return false;
    const int offset = endsWith(block, "_door") ? 1 : block == "small_dripleaf" ? -1 : 0;
    turnName(v.s, offset, rotation, mirror);
  } else if (key == "facing_direction") {
    if (v.isText) {
      if (dirFromName(v.s) >= 0) turnName(v.s, 0, rotation, mirror);
    } else {
      static constexpr std::array<int, 6> byValue{-1, -1, N, S, W, E};  // 0 down, 1 up, 2 north, 3 south, 4 west, 5 east
      if (!turnIndex(byValue, v.i, rotation, mirror)) return false;
    }
  } else if (key == "weirdo_direction") {
    static constexpr std::array<int, 4> byValue{E, W, S, N};
    if (v.isText || !turnIndex(byValue, v.i, rotation, mirror)) return false;
  } else if (key == "direction") {
    static constexpr std::array<int, 4> trapdoor{E, W, S, N};
    static constexpr std::array<int, 4> southFirst{S, W, N, E};  // beds, beehives, cocoa, grindstone, loom...
    static constexpr std::array<int, 4> northFirst{N, E, S, W};  // bell, decorated_pot
    if (v.isText) return false;
    if (endsWith(block, "trapdoor")) turnIndex(trapdoor, v.i, rotation, mirror);
    else if (block == "bell" || block == "decorated_pot") turnIndex(northFirst, v.i, rotation, mirror);
    else if (endsWith(block, "_bed") || block == "beehive" || block == "bee_nest" || block == "chiseled_bookshelf" ||
             block == "cocoa" || block == "grindstone" || block == "loom" || block == "tripwire_hook")
      turnIndex(southFirst, v.i, rotation, mirror);
  } else if (key == "coral_direction") {
    static constexpr std::array<int, 4> byValue{W, E, N, S};
    if (v.isText || !turnIndex(byValue, v.i, rotation, mirror)) return false;
  } else if (key == "torch_facing_direction") {
    if (v.isText) turnName(v.s, 0, rotation, mirror);  // "top" / "unknown" are not directions and stay
  } else if (key == "lever_direction") {
    if (!v.isText) return false;
    if (dirFromName(v.s) >= 0) turnName(v.s, 0, rotation, mirror);
    else if (rotation & 1) {
      if (v.s == "up_east_west") v.s = "up_north_south";
      else if (v.s == "up_north_south") v.s = "up_east_west";
      else if (v.s == "down_east_west") v.s = "down_north_south";
      else if (v.s == "down_north_south") v.s = "down_east_west";
    }
  } else if (key == "pillar_axis" || key == "portal_axis") {
    if (v.isText && (rotation & 1)) {
      if (v.s == "x") v.s = "z";
      else if (v.s == "z") v.s = "x";
    }
  } else if (key == "door_hinge_bit") {
    if (!v.isText && mirror) v.i = v.i ? 0 : 1;
  } else if (key == "ground_sign_direction") {  // 0..15 in 22.5 degree steps, 0 south, clockwise
    if (v.isText || v.i < 0 || v.i > 15) return false;
    const int r = mirror ? (16 - v.i) & 15 : v.i;
    v.i = (r + 4 * rotation) & 15;
  } else if (key == "rail_direction") {
    if (!v.isText) v.i = turnRail(v.i, rotation, mirror);
  } else if (key == "vine_direction_bits") {  // south 1, west 2, north 4, east 8
    if (!v.isText) v.i = turnBits(v.i, {4, 8, 1, 2}, rotation, mirror);
  } else if (key == "multi_face_direction_bits") {  // down 1, up 2, south 4, west 8, north 16, east 32
    if (!v.isText) v.i = turnBits(v.i, {16, 32, 4, 8}, rotation, mirror);
  }
  return v.i != before.i || v.s != before.s;
}

// The face to click so the game itself places `block` facing the wanted way, or -1 when the block's facing does not come
// from the clicked face. Wall signs, wall banners and wall heads take facing_direction straight from the clicked face of
// their support block (Bedrock face numbers: 2 north, 3 south, 4 west, 5 east). `v` is the already transformed value.
inline int clickFace(std::string_view block, std::string_view key, const StateValue &v) {
  using namespace detail;
  constexpr std::string_view prefix = "minecraft:";
  if (block.substr(0, prefix.size()) == prefix) block.remove_prefix(prefix.size());
  if (key.substr(0, prefix.size()) == prefix) key.remove_prefix(prefix.size());
  if (key != "facing_direction" || v.isText || v.i < 2 || v.i > 5) return -1;
  if (endsWith(block, "wall_sign") || endsWith(block, "wall_banner") || block == "skull") return v.i;
  return -1;
}

// Block entities whose facing the server derives from the placing player's yaw (not from the clicked face), and which of
// the game's getPlacementBlock implementations decides it: 1 the base one (block traits), 2 BarrelBlock's, 4 the
// dispenser/dropper one; 0 for every other block.
inline int placementHook(std::string_view block) {
  constexpr std::string_view prefix = "minecraft:";
  if (block.substr(0, prefix.size()) == prefix) block.remove_prefix(prefix.size());
  if (block == "barrel") return 2;
  if (block == "dispenser" || block == "dropper") return 4;
  for (const std::string_view b : {"chest", "trapped_chest", "ender_chest", "furnace", "blast_furnace", "smoker", "lectern",
                                   "campfire", "soul_campfire", "unpowered_comparator"})
    if (block == b) return 1;
  return 0;
}

// The states a player-direction placement fills in from the player's yaw.
inline bool isDirectionKey(std::string_view key) {
  constexpr std::string_view prefix = "minecraft:";
  if (key.substr(0, prefix.size()) == prefix) key.remove_prefix(prefix.size());
  return key == "cardinal_direction" || key == "facing_direction" || key == "direction" || key == "weirdo_direction";
}

}  // namespace bematica::orient
