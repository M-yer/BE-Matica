#include "blockmap.hpp"

#include "gzip.hpp"

#include <cstdlib>
#include <unordered_set>
#include <string_view>

extern "C" const uint8_t bematica_block_map[], bematica_block_map_end[];

namespace bematica {
namespace {

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  for (size_t start = 0;;) {
    const size_t end = s.find(sep, start);
    out.push_back(s.substr(start, end - start));
    if (end == std::string_view::npos) return out;
    start = end + 1;
  }
}

// "key=Tvalue,..." where T is b (byte), i (int) or s (string).
nbt::Tag parseStates(std::string_view s) {
  nbt::Tag states = nbt::makeCompound();
  if (s.empty()) return states;
  for (auto field : split(s, ',')) {
    const size_t eq = field.find('=');
    const std::string key(field.substr(0, eq));
    const char type = field[eq + 1];
    const std::string value(field.substr(eq + 2));
    if (type == 's') nbt::put(states, key, nbt::makeString(value));
    else nbt::put(states, key, nbt::makeInt(type == 'b' ? nbt::Type::Byte : nbt::Type::Int, std::atoll(value.c_str())));
  }
  return states;
}

// Blocks Java renamed after the table's source version wrote them.
const char *renamedJava(const std::string &name) {
  if (name == "minecraft:grass") return "minecraft:short_grass";
  if (name == "minecraft:grass_path") return "minecraft:dirt_path";
  if (name == "minecraft:chain") return "minecraft:iron_chain";
  return nullptr;
}

}  // namespace

BlockMap BlockMap::forBlocks(const std::vector<std::string> &javaNames) {
  std::unordered_set<std::string> wanted(javaNames.begin(), javaNames.end());
  for (const auto &name : javaNames)
    if (const char *renamed = renamedJava(name)) wanted.insert(renamed);

  BlockMap m;
  int32_t version = 0;
  auto takeLine = [&](std::string_view line) {
    if (!version) {
      version = std::atoi(std::string(line).c_str());
      return;
    }
    const size_t bracket = line.find('[');
    if (bracket == std::string_view::npos || !wanted.count(std::string(line.substr(0, bracket)))) return;
    const auto cols = split(line, '\t');
    if (cols.size() != 3) return;
    const auto java = cols[0];
    Entry e;
    const std::string_view props = java.substr(bracket + 1, java.size() - bracket - 2);
    if (!props.empty())
      for (auto p : split(props, ',')) {
        const size_t eq = p.find('=');
        e.javaProps.emplace_back(std::string(p.substr(0, eq)), std::string(p.substr(eq + 1)));
      }
    e.bedrock = {std::string(cols[1]), parseStates(cols[2]), version};
    m.byName_[std::string(java.substr(0, bracket))].push_back(std::move(e));
  };
  std::string partial;  // a line cut in two by the chunk boundary
  std::string error;
  const bool ok = gunzipChunks({bematica_block_map, bematica_block_map_end}, [&](std::span<const uint8_t> chunk) {
    std::string_view rest(reinterpret_cast<const char *>(chunk.data()), chunk.size());
    for (size_t nl; (nl = rest.find('\n')) != std::string_view::npos; rest.remove_prefix(nl + 1)) {
      if (partial.empty()) {
        takeLine(rest.substr(0, nl));
      } else {
        partial.append(rest.substr(0, nl));
        takeLine(partial);
        partial.clear();
      }
    }
    partial.append(rest);
  }, error);
  if (!partial.empty()) takeLine(partial);
  if (ok) m.version_ = version;
  return m;
}

bool BlockMap::toBedrock(const std::string &javaName, const Props &props, BlockState &out) const {
  auto it = byName_.find(javaName);
  if (it == byName_.end())
    if (const char *renamed = renamedJava(javaName)) it = byName_.find(renamed);
  if (it == byName_.end()) return false;
  const Entry *best = nullptr;
  int bestScore = -1;
  for (const auto &e : it->second) {
    int score = 0;
    for (const auto &p : props)
      for (const auto &q : e.javaProps)
        if (p.first == q.first && p.second == q.second) ++score;
    if (score == static_cast<int>(props.size()) && score == static_cast<int>(e.javaProps.size())) {
      best = &e;  // exact match
      break;
    }
    if (score > bestScore) best = &e, bestScore = score;
  }
  out = best->bedrock;
  return true;
}

}  // namespace bematica
