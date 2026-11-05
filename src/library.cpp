#include "library.hpp"
#include "itemmap.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace bematica {

namespace fs = std::filesystem;

std::vector<fs::path> findStructures(const std::vector<fs::path> &dirs) {
  std::vector<fs::path> files;
  for (const auto &dir : dirs) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const auto &p = it->path();
      if (it->is_regular_file(ec) && p.extension() == ".mcstructure") files.push_back(p);
    }
  }
  std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) { return a.filename() < b.filename(); });
  return files;
}

bool loadStructureFile(const fs::path &file, Structure &out, std::string &error) {
  // Bigger files are far beyond any structure a phone can preview.
  constexpr std::uintmax_t kMaxFileSize = 32u << 20;
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  if (ec) {
    error = "cannot read file";
    return false;
  }
  if (size > kMaxFileSize) {
    error = "file larger than 32 MB";
    return false;
  }
  std::ifstream in(file, std::ios::binary);
  std::vector<uint8_t> data(size);
  if (!in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size))) {
    error = "cannot read file";
    return false;
  }
  return loadMcstructure(data, out, error);
}

std::vector<Material> countMaterials(const Structure &s) {
  std::vector<size_t> perEntry(s.palette.size());
  for (const auto &layer : s.layers)
    for (int32_t idx : layer)
      if (idx >= 0) ++perEntry[idx];

  // Several palette entries can be the same block with different states (e.g. stair directions).
  std::vector<Material> out;
  for (size_t k = 0; k < s.palette.size(); ++k) {
    if (!perEntry[k]) continue;
    std::string name = s.palette[k].name;
    if (name.rfind("minecraft:", 0) == 0) name.erase(0, 10);
    if (name == "air" || name == "structure_void") continue;
    auto it = std::find_if(out.begin(), out.end(), [&](const Material &m) { return m.name == name; });
    if (it == out.end()) out.push_back({std::move(name), perEntry[k]});
    else it->count += perEntry[k];
  }
  std::sort(out.begin(), out.end(), [](const Material &a, const Material &b) {
    return a.count != b.count ? a.count > b.count : a.name < b.name;
  });
  return out;
}

void substituteBlock(Structure &s, const std::string &from, const BlockState &to) {
  const std::string full = "minecraft:" + from;
  for (auto &entry : s.palette) {
    if (entry.name != full && entry.name != from) continue;
    BlockState next = to;
    for (size_t k = 0; k < next.states.keys.size(); ++k)
      if (const nbt::Tag *old = entry.states.get(next.states.keys[k], next.states.items[k].type))
        next.states.items[k] = *old;
    entry = std::move(next);
  }
}

void unpowerBlocks(Structure &s) {
  for (auto &entry : s.palette) {
    const std::string placed = itemmap::placedForm(entry.name);
    if (placed == entry.name) continue;
    entry.name = placed;
    for (size_t k = 0; k < entry.states.keys.size(); ++k)
      if (entry.states.keys[k] == "output_lit_bit") entry.states.items[k].i = 0;
  }
}

}  // namespace bematica
