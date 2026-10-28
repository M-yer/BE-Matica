#include "mcstructure.hpp"

namespace bematica {
namespace {

bool readVec3(const nbt::Tag *tag, std::array<int32_t, 3> &out) {
  if (!tag || tag->type != nbt::Type::List || tag->listType != nbt::Type::Int || tag->ints.size() != 3) return false;
  for (int k = 0; k < 3; ++k) out[k] = static_cast<int32_t>(tag->ints[k]);
  return true;
}

bool fail(std::string &error, const char *msg) {
  error = msg;
  return false;
}

}  // namespace

bool loadMcstructure(std::span<const uint8_t> data, Structure &out, std::string &error) {
  nbt::Tag root;
  if (!nbt::parse(data, root, error)) return false;

  out = Structure{};
  const auto *format = root.get("format_version", nbt::Type::Int);
  // Version 2 stores each block layer as an IntArray instead of a List of Int; the data is the same.
  if (!format || (format->i != 1 && format->i != 2)) return fail(error, "unsupported format_version");
  if (!readVec3(root.get("size"), out.size)) return fail(error, "missing size");
  readVec3(root.get("structure_world_origin"), out.origin);
  for (int v : out.size)
    if (v <= 0) return fail(error, "empty structure");
  // Guards the volume math against overflow; far above what a structure block can save.
  constexpr uint64_t kMaxVolume = 1u << 26;
  if (static_cast<uint64_t>(out.size[0]) * out.size[1] > kMaxVolume / out.size[2]) return fail(error, "structure too large");

  const auto *structure = root.get("structure", nbt::Type::Compound);
  if (!structure) return fail(error, "missing structure");

  // Entity-only structures (e.g. a saved XP orb) have no palette at all.
  const nbt::Tag *palette = nullptr;
  if (const auto *p = structure->get("palette", nbt::Type::Compound))
    if (const auto *def = p->get("default", nbt::Type::Compound)) palette = def->get("block_palette", nbt::Type::List);
  if (palette && !palette->items.empty() && palette->listType != nbt::Type::Compound) return fail(error, "bad block_palette");
  static const std::vector<nbt::Tag> kNoEntries;
  for (const auto &entry : palette ? palette->items : kNoEntries) {
    const auto *name = entry.get("name", nbt::Type::String);
    if (!name) return fail(error, "palette entry without name");
    BlockState bs{name->s, {}, 0};
    if (const auto *states = entry.get("states", nbt::Type::Compound)) bs.states = *states;
    if (const auto *version = entry.get("version", nbt::Type::Int)) bs.version = static_cast<int32_t>(version->i);
    out.palette.push_back(std::move(bs));
  }

  const auto *indices = structure->get("block_indices", nbt::Type::List);
  if (!indices || (!indices->items.empty() && indices->listType != nbt::Type::List &&
                   indices->listType != nbt::Type::IntArray))
    return fail(error, "missing block_indices");

  const size_t volume = out.volume();
  const auto paletteSize = static_cast<int64_t>(out.palette.size());
  for (size_t layer = 0; layer < 2; ++layer) {
    auto &dst = out.layers[layer];
    if (layer >= indices->items.size() || indices->items[layer].ints.empty()) {
      dst.assign(volume, -1);  // layer not saved: no blocks in it
      continue;
    }
    const auto &src = indices->items[layer].ints;
    if (src.size() != volume) return fail(error, "block count does not match size");
    dst.reserve(volume);
    for (int64_t v : src) {
      if (v < -1 || v >= paletteSize) return fail(error, "block index outside palette");
      dst.push_back(static_cast<int32_t>(v));
    }
  }
  return true;
}

std::vector<uint8_t> saveMcstructure(const Structure &s) {
  using namespace nbt;
  // Written field by field instead of as one Tag tree: the tree would copy every block index into
  // 64-bit storage first, roughly tripling the memory a big structure needs while it saves.
  Writer w;
  w.out.reserve(s.volume() * 2 * sizeof(int32_t) + s.palette.size() * 128 + 512);
  auto field = [&](Type t, const char *key) {
    w.put<uint8_t>(static_cast<uint8_t>(t));
    w.string(key);
  };
  auto intList = [&](const char *key, std::span<const int32_t> values) {
    field(Type::List, key);
    w.put<uint8_t>(static_cast<uint8_t>(Type::Int));
    w.put<int32_t>(static_cast<int32_t>(values.size()));
    // Android and desktop hosts are little-endian, as the format is, so the ints copy as they are.
    const auto *bytes = reinterpret_cast<const uint8_t *>(values.data());
    w.out.insert(w.out.end(), bytes, bytes + values.size_bytes());
  };

  w.put<uint8_t>(static_cast<uint8_t>(Type::Compound));
  w.string("");
  field(Type::Int, "format_version");
  w.put<int32_t>(1);
  intList("size", s.size);
  field(Type::Compound, "structure");

  field(Type::List, "block_indices");
  w.put<uint8_t>(static_cast<uint8_t>(Type::List));
  w.put<int32_t>(2);
  for (const auto &layer : s.layers) {
    w.put<uint8_t>(static_cast<uint8_t>(Type::Int));
    w.put<int32_t>(static_cast<int32_t>(layer.size()));
    const auto *bytes = reinterpret_cast<const uint8_t *>(layer.data());
    w.out.insert(w.out.end(), bytes, bytes + layer.size() * sizeof(int32_t));
  }
  field(Type::List, "entities");
  w.put<uint8_t>(static_cast<uint8_t>(Type::Compound));
  w.put<int32_t>(0);

  field(Type::Compound, "palette");
  field(Type::Compound, "default");
  field(Type::List, "block_palette");
  w.put<uint8_t>(static_cast<uint8_t>(Type::Compound));
  w.put<int32_t>(static_cast<int32_t>(s.palette.size()));
  for (const auto &bs : s.palette) {
    Tag entry = makeCompound();
    put(entry, "name", makeString(bs.name));
    put(entry, "states", bs.states.type == Type::Compound ? bs.states : makeCompound());
    put(entry, "version", makeInt(Type::Int, bs.version));
    w.payload(entry);
  }
  field(Type::Compound, "block_position_data");
  w.put<uint8_t>(0);
  w.put<uint8_t>(0);  // end of default
  w.put<uint8_t>(0);  // end of palette
  w.put<uint8_t>(0);  // end of structure

  intList("structure_world_origin", s.origin);
  w.put<uint8_t>(0);  // end of root
  return std::move(w.out);
}

}  // namespace bematica
