// Blocks that are placed by an item of another name (doors, buckets, seeds...). Pure functions, no game types.
// Block names are Bedrock ones ("minecraft:reeds"); an unlisted block returns "" and is picked by its block type.
#pragma once

#include <string>
#include <string_view>

namespace bematica::itemmap {

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

}  // namespace bematica::itemmap
