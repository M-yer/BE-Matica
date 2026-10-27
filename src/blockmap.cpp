#include "blockmap.hpp"

#include "gzip.hpp"

#include <cstdlib>
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

const BlockMap &BlockMap::builtIn() {
  static const BlockMap map = [] {
    BlockMap m;
    std::vector<uint8_t> text;
    std::string error;
    if (!gunzip({bematica_block_map, bematica_block_map_end}, text, 16u << 20, error)) return m;
    const std::string_view all(reinterpret_cast<const char *>(text.data()), text.size());
    const auto lines = split(all, '\n');
    m.version_ = std::atoi(std::string(lines[0]).c_str());
    for (size_t k = 1; k < lines.size(); ++k) {
      const auto cols = split(lines[k], '\t');
      if (cols.size() != 3) continue;
      const auto java = cols[0];
      const size_t bracket = java.find('[');
      Entry e;
      const std::string_view props = java.substr(bracket + 1, java.size() - bracket - 2);
      if (!props.empty())
        for (auto p : split(props, ',')) {
          const size_t eq = p.find('=');
          e.javaProps.emplace_back(std::string(p.substr(0, eq)), std::string(p.substr(eq + 1)));
        }
      e.bedrock = {std::string(cols[1]), parseStates(cols[2]), m.version_};
      m.byName_[std::string(java.substr(0, bracket))].push_back(std::move(e));
    }
    return m;
  }();
  return map;
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
