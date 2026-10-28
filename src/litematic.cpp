#include "litematic.hpp"

#include "blockmap.hpp"
#include "gzip.hpp"

#include <algorithm>
#include <bit>
#include <climits>
#include <map>

namespace bematica {
namespace {

bool fail(std::string &error, std::string msg) {
  error = std::move(msg);
  return false;
}

bool readXyz(const nbt::Tag *tag, std::array<int32_t, 3> &out) {
  if (!tag) return false;
  static constexpr const char *kAxes[] = {"x", "y", "z"};
  for (int k = 0; k < 3; ++k) {
    const auto *v = tag->get(kAxes[k], nbt::Type::Int);
    if (!v) return false;
    out[k] = static_cast<int32_t>(v->i);
  }
  return true;
}

struct Region {
  const nbt::Tag *tag;
  std::array<int32_t, 3> min, size;  // size is positive; min is the lowest corner
};

bool isEmpty(const std::string &name) {
  return name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air" ||
         name == "minecraft:structure_void";
}

}  // namespace

bool loadLitematic(std::span<const uint8_t> data, Structure &out, LitematicInfo &info, std::string &error) {
  std::vector<uint8_t> raw;
  if (!gunzip(data, raw, 256u << 20, error)) return false;
  nbt::Tag root;
  if (!nbt::parse(raw, root, error, nbt::Endian::Big)) return false;
  std::vector<uint8_t>().swap(raw);  // the parsed tree holds everything now

  out = Structure{};
  info = LitematicInfo{};
  if (const auto *meta = root.get("Metadata", nbt::Type::Compound)) {
    if (const auto *n = meta->get("Name", nbt::Type::String)) info.name = n->s;
    if (const auto *a = meta->get("Author", nbt::Type::String)) info.author = a->s;
  }
  const auto *regions = root.get("Regions", nbt::Type::Compound);
  if (!regions || regions->items.empty()) return fail(error, "no regions in file");

  // A region's Size can be negative: it then extends from Position towards smaller coordinates.
  std::vector<Region> list;
  std::array<int32_t, 3> lo{INT_MAX, INT_MAX, INT_MAX}, hi{INT_MIN, INT_MIN, INT_MIN};
  for (const auto &tag : regions->items) {
    Region r{&tag, {}, {}};
    std::array<int32_t, 3> pos, size;
    if (!readXyz(tag.get("Position", nbt::Type::Compound), pos) || !readXyz(tag.get("Size", nbt::Type::Compound), size))
      return fail(error, "region without position or size");
    for (int k = 0; k < 3; ++k) {
      if (size[k] == 0) return fail(error, "empty region");
      r.size[k] = std::abs(size[k]);
      r.min[k] = size[k] > 0 ? pos[k] : pos[k] + size[k] + 1;
      lo[k] = std::min(lo[k], r.min[k]);
      hi[k] = std::max(hi[k], r.min[k] + r.size[k]);
    }
    list.push_back(r);
  }
  info.regions = static_cast<int>(list.size());
  for (int k = 0; k < 3; ++k) out.size[k] = hi[k] - lo[k];
  constexpr uint64_t kMaxVolume = 1u << 26;  // same cap as .mcstructure loading
  if (static_cast<uint64_t>(out.size[0]) * out.size[1] > kMaxVolume / out.size[2]) return fail(error, "schematic too large");
  out.layers[0].assign(out.volume(), -1);
  out.layers[1].assign(out.volume(), -1);

  std::vector<std::string> javaNames;
  for (const auto &r : list)
    if (const auto *palette = r.tag->get("BlockStatePalette", nbt::Type::List))
      for (const auto &entry : palette->items)
        if (const auto *name = entry.get("Name", nbt::Type::String)) javaNames.push_back(name->s);
  const BlockMap map = BlockMap::forBlocks(javaNames);
  if (!map.version()) return fail(error, "block table missing");
  std::map<std::string, int32_t> paletteIndex;  // Bedrock state text -> index in out.palette
  auto addBedrock = [&](const BlockState &bs) {
    std::string key = bs.name;
    for (size_t k = 0; k < bs.states.keys.size(); ++k)
      key += "|" + bs.states.keys[k] + "=" + std::to_string(bs.states.items[k].i) + bs.states.items[k].s;
    auto [it, added] = paletteIndex.emplace(key, static_cast<int32_t>(out.palette.size()));
    if (added) out.palette.push_back(bs);
    return it->second;
  };
  BlockState water{"minecraft:water", nbt::makeCompound(), map.version()};
  nbt::put(water.states, "liquid_depth", nbt::makeInt(nbt::Type::Int, 0));

  for (const auto &r : list) {
    const auto *palette = r.tag->get("BlockStatePalette", nbt::Type::List);
    const auto *states = r.tag->get("BlockStates", nbt::Type::LongArray);
    if (!palette || palette->items.empty() || !states) return fail(error, "region without blocks");

    // Each Java palette entry becomes a Bedrock palette index (-1 = nothing) and maybe water.
    std::vector<int32_t> toLayer0, toLayer1;
    for (const auto &entry : palette->items) {
      const auto *name = entry.get("Name", nbt::Type::String);
      if (!name) return fail(error, "palette entry without name");
      BlockMap::Props props;
      bool waterlogged = false;
      if (const auto *p = entry.get("Properties", nbt::Type::Compound))
        for (size_t k = 0; k < p->keys.size(); ++k) {
          props.emplace_back(p->keys[k], p->items[k].s);
          if (p->keys[k] == "waterlogged" && p->items[k].s == "true") waterlogged = true;
        }
      std::sort(props.begin(), props.end());
      BlockState bs;
      if (isEmpty(name->s)) {
        toLayer0.push_back(-1);
      } else if (map.toBedrock(name->s, props, bs)) {
        toLayer0.push_back(addBedrock(bs));
      } else {
        if (std::find(info.unknownBlocks.begin(), info.unknownBlocks.end(), name->s) == info.unknownBlocks.end())
          info.unknownBlocks.push_back(name->s);
        toLayer0.push_back(-1);
      }
      toLayer1.push_back(waterlogged ? addBedrock(water) : -1);
    }

    // Litematica packs entries tightly, letting one entry span two longs; index order is y, z, x.
    const int bits = std::max(2, std::bit_width(palette->items.size() - 1));
    const uint64_t mask = (uint64_t{1} << bits) - 1;
    const size_t count = static_cast<size_t>(r.size[0]) * r.size[1] * r.size[2];
    if (states->ints.size() * 64 < count * bits) return fail(error, "block data shorter than region");
    for (size_t idx = 0; idx < count; ++idx) {
      const size_t bit = idx * bits, word = bit >> 6, offset = bit & 63;
      uint64_t v = static_cast<uint64_t>(states->ints[word]) >> offset;
      if (offset + bits > 64) v |= static_cast<uint64_t>(states->ints[word + 1]) << (64 - offset);
      v &= mask;
      if (v >= toLayer0.size()) return fail(error, "block outside palette");
      const int x = static_cast<int>(idx % r.size[0]);
      const int z = static_cast<int>(idx / r.size[0] % r.size[2]);
      const int y = static_cast<int>(idx / (static_cast<size_t>(r.size[0]) * r.size[2]));
      const size_t at = out.index(r.min[0] - lo[0] + x, r.min[1] - lo[1] + y, r.min[2] - lo[2] + z);
      if (toLayer0[v] >= 0) out.layers[0][at] = toLayer0[v];
      if (toLayer1[v] >= 0) out.layers[1][at] = toLayer1[v];
    }
  }
  return true;
}

}  // namespace bematica
