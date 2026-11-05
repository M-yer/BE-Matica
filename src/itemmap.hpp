// Blocks that are placed by an item of another name (doors, buckets, seeds...). Pure functions, no game types.
// Block names are Bedrock ones ("minecraft:reeds"); an unlisted block returns "" and is picked by its block type.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace bematica::itemmap {

// Redstone parts saved while powered or unlit: the item always places the other form, and the circuit switches between
// the two by itself. Names without "minecraft:".
struct PoweredForm {
  std::string_view saved, placed;
};
inline constexpr PoweredForm kPoweredForms[] = {
    {"powered_repeater", "unpowered_repeater"},
    {"powered_comparator", "unpowered_comparator"},
    {"unlit_redstone_torch", "redstone_torch"},
    {"lit_redstone_lamp", "redstone_lamp"},
};

// The block the item places for `block` (with or without "minecraft:"), or `block` itself.
inline std::string placedForm(std::string_view block) {
  constexpr std::string_view ns = "minecraft:";
  const bool prefixed = block.substr(0, ns.size()) == ns;
  const std::string_view name = prefixed ? block.substr(ns.size()) : block;
  for (const PoweredForm &f : kPoweredForms)
    if (name == f.saved) return (prefixed ? std::string(ns) : std::string()) + std::string(f.placed);
  return std::string(block);
}

inline std::string itemFor(std::string_view block) {
  constexpr std::string_view ns = "minecraft:";
  if (block.substr(0, ns.size()) != ns) return {};
  const std::string_view name = block.substr(ns.size());
  struct Entry {
    std::string_view block, item;
  };
  static constexpr Entry table[] = {
      {"water", "water_bucket"},          {"flowing_water", "water_bucket"},
      {"lava", "lava_bucket"},            {"flowing_lava", "lava_bucket"},
      {"reeds", "sugar_cane"},            {"cake", "cake"},
      {"cauldron", "cauldron"},           {"flower_pot", "flower_pot"},
      {"kelp", "kelp"},                   {"redstone_wire", "redstone"},
      {"unpowered_repeater", "repeater"}, {"powered_repeater", "repeater"},
      {"unpowered_comparator", "comparator"}, {"powered_comparator", "comparator"},
      {"cocoa", "cocoa_beans"},           {"wheat", "wheat_seeds"},
      {"beetroot", "beetroot_seeds"},     {"carrots", "carrot"},
      {"potatoes", "potato"},             {"melon_stem", "melon_seeds"},
      {"pumpkin_stem", "pumpkin_seeds"},
  };
  for (const Entry &e : table)
    if (name == e.block) return std::string(ns) + std::string(e.item);
  // Signs are placed by the sign item (it picks wall or standing from the clicked face and sets the facing); the block's
  // own item places the default permutation instead. Oak has no wood prefix and dark oak is "darkoak" in block names.
  for (const std::string_view suffix : {"wall_sign", "standing_sign"})
    if (name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix) {
      std::string_view wood = name.substr(0, name.size() - suffix.size());
      if (!wood.empty() && wood.back() == '_') wood.remove_suffix(1);
      else if (!wood.empty()) continue;  // e.g. "foowall_sign": not a sign block name
      if (wood.empty()) wood = "oak";
      if (wood == "darkoak") wood = "dark_oak";
      return std::string(ns) + std::string(wood) + "_sign";
    }
  // Every door (wood, iron, copper and their waxed forms) is placed by the item of the same name. "_door" with the
  // underscore keeps trapdoors out.
  constexpr std::string_view door = "_door";
  if (name.size() > door.size() && name.substr(name.size() - door.size()) == door) return std::string(block);
  return {};
}

// How many of the item for `block` fit in one inventory slot (with or without "minecraft:").
inline int stackSize(std::string_view block) {
  constexpr std::string_view ns = "minecraft:";
  if (block.substr(0, ns.size()) == ns) block.remove_prefix(ns.size());
  const auto endsWith = [&](std::string_view tail) {
    return block.size() >= tail.size() && block.substr(block.size() - tail.size()) == tail;
  };
  // Liquids and powder snow are placed from filled buckets.
  if (block == "bed" || block == "cake" || endsWith("shulker_box") || block == "water" || block == "flowing_water" ||
      block == "lava" || block == "flowing_lava" || block == "powder_snow")
    return 1;
  if (endsWith("sign") || endsWith("banner")) return 16;
  return 64;
}

// `count` items as full shulker boxes (27 slots), then full stacks, then the rest. Unstackable items have no stacks.
struct Breakdown {
  size_t shulkers = 0, stacks = 0, rest = 0;
};
inline Breakdown breakdown(size_t count, int stack) {
  const size_t box = 27 * static_cast<size_t>(stack);
  if (stack == 1) return {count / box, 0, count % box};
  return {count / box, count % box / stack, count % stack};
}

}  // namespace bematica::itemmap
