#include "atlas.hpp"

#include "image.hpp"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

namespace bematica {
namespace {

using Json = nlohmann::json;

constexpr int kAtlasWidth = 1024;
constexpr int kMaxAtlasHeight = 4096;
constexpr int kPad = 1;  // edge pixels repeated around each tile so filtering and mip levels do not bleed neighbours
constexpr uint32_t kCacheMagic = 0x54414D42;  // "BMAT"
constexpr uint32_t kCacheFormat = 1;

constexpr uint32_t kGrass = 0x79C05A, kFoliage = 0x79C05A, kSpruce = 0x619961, kBirch = 0x80A755, kWater = 0x44AFF5;

// JSON with // comments (terrain_texture.json starts with one) and trailing commas.
Json parseLoose(const std::vector<uint8_t> &data) {
  std::string s(data.begin(), data.end());
  if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);
  std::string clean;
  clean.reserve(s.size());
  bool inString = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (inString) {
      clean += c;
      if (c == '\\' && i + 1 < s.size()) clean += s[++i];
      else if (c == '"') inString = false;
    } else if (c == '"') {
      inString = true, clean += c;
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') ++i;
    } else if (c == ',') {
      size_t j = i + 1;
      while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      if (j < s.size() && (s[j] == '}' || s[j] == ']')) continue;
      clean += c;
    } else {
      clean += c;
    }
  }
  return Json::parse(clean, nullptr, false, true);
}

constexpr const char *kFaceKeys[6] = {"up", "down", "north", "south", "west", "east"};

// "textures": "key"  or  {"up": .., "down": .., "side": .., "north": ..}
void parseFaces(const Json &t, std::array<std::string, 6> &out) {
  if (t.is_string()) {
    out.fill(t.get<std::string>());
    return;
  }
  if (!t.is_object()) return;
  auto get = [&](const char *k) -> std::string {
    const auto it = t.find(k);
    return it != t.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  const std::string side = get("side");
  for (int f = 0; f < 6; ++f) {
    std::string v = get(kFaceKeys[f]);
    if (v.empty() && f >= 2) v = side;
    if (v.empty() && f < 2) v = get(kFaceKeys[1 - f]);
    if (v.empty()) v = side;
    if (v.empty())
      for (const auto &kv : t.items())
        if (kv.value().is_string()) {
          v = kv.value().get<std::string>();
          break;
        }
    out[f] = v;
  }
}

struct Variant {
  std::string path;
  uint32_t color = 0;  // 0xRRGGBB from "overlay_color" / "tint_color", 0 when absent
};

uint32_t parseColor(const Json &j) {
  if (!j.is_string()) return 0;
  const std::string s = j.get<std::string>();
  return s.size() == 7 && s[0] == '#' ? static_cast<uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 16)) : 0;
}

void parseVariant(const Json &j, std::vector<Variant> &out) {
  if (j.is_string()) {
    out.push_back({j.get<std::string>(), 0});
  } else if (j.is_object() && j.contains("path") && j["path"].is_string()) {
    uint32_t c = j.contains("overlay_color") ? parseColor(j["overlay_color"]) : 0;
    if (!c && j.contains("tint_color")) c = parseColor(j["tint_color"]);
    out.push_back({j["path"].get<std::string>(), c});
  }
}

std::string baseName(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return path.substr(slash == std::string::npos ? 0 : slash + 1);
}

bool startsWith(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }
bool endsWith(const std::string &s, const char *p) {
  const size_t n = std::strlen(p);
  return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

// Bedrock stores the tintable part of some images (grass sides) with alpha 255 and the untinted part (dirt) with alpha 0;
// the other images are grey and simply multiplied.
void tintImage(Image &img, uint32_t rgb, bool alphaMask) {
  const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  for (size_t i = 0; i < img.rgba.size(); i += 4) {
    uint8_t *p = &img.rgba[i];
    if (alphaMask && p[3] == 0) {
      p[3] = 255;
      continue;
    }
    p[0] = static_cast<uint8_t>(p[0] * r / 255);
    p[1] = static_cast<uint8_t>(p[1] * g / 255);
    p[2] = static_cast<uint8_t>(p[2] * b / 255);
    if (alphaMask) p[3] = 255;
  }
}

Image missingImage() {
  Image img;
  img.w = img.h = 16;
  img.rgba.resize(16 * 16 * 4);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      uint8_t *p = &img.rgba[(y * 16 + x) * 4];
      const bool on = ((x / 8) + (y / 8)) & 1;
      p[0] = on ? 255 : 0, p[1] = 0, p[2] = on ? 255 : 0, p[3] = 255;
    }
  return img;
}

struct Packed {
  Image img;
  int x = 0, y = 0;
};

class Builder {
public:
  explicit Builder(const ResourcePack &pack) : pack_(pack) {}

  // Index into images_ for this texture, decoded, first-framed and tinted (shared between keys that use it).
  int image(const Variant &v) {
    const std::string cacheKey = v.path + "|" + std::to_string(v.color);
    if (const auto it = byKey_.find(cacheKey); it != byKey_.end()) return it->second;
    Image img;
    if (!load(v.path, img)) {
      if (missing_++ < 3) missingNames_ += (missingNames_.empty() ? "" : ", ") + v.path;
      img = missingImage();
    } else {
      if (img.h > img.w && img.h % img.w == 0) {  // flipbook strip: first frame only
        img.h = img.w;
        img.rgba.resize(static_cast<size_t>(img.w) * img.h * 4);
      }
      tint(v, img);
    }
    images_.push_back({std::move(img), 0, 0});
    return byKey_[cacheKey] = static_cast<int>(images_.size()) - 1;
  }

  bool pack(TextureAtlas &out, std::string &error) {
    std::vector<int> order(images_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
      return images_[a].img.h != images_[b].img.h ? images_[a].img.h > images_[b].img.h : images_[a].img.w > images_[b].img.w;
    });
    int x = 0, y = 0, shelf = 0;
    for (int i : order) {
      Packed &p = images_[i];
      const int cw = p.img.w + 2 * kPad, ch = p.img.h + 2 * kPad;
      if (cw > kAtlasWidth) {
        error = "texture wider than the atlas";
        return false;
      }
      if (x + cw > kAtlasWidth) x = 0, y += shelf, shelf = 0;
      p.x = x + kPad, p.y = y + kPad;
      x += cw, shelf = std::max(shelf, ch);
    }
    int height = 1;
    while (height < y + shelf) height *= 2;
    if (height > kMaxAtlasHeight) {
      error = "too many textures for one atlas";
      return false;
    }
    out.width = kAtlasWidth, out.height = height;
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    for (const Packed &p : images_)
      for (int dy = -kPad; dy < p.img.h + kPad; ++dy)
        for (int dx = -kPad; dx < p.img.w + kPad; ++dx) {
          const int sx = std::clamp(dx, 0, p.img.w - 1), sy = std::clamp(dy, 0, p.img.h - 1);
          std::memcpy(&out.rgba[(static_cast<size_t>(p.y + dy) * out.width + p.x + dx) * 4],
                      &p.img.rgba[(static_cast<size_t>(sy) * p.img.w + sx) * 4], 4);
        }
    return true;
  }

  Tile tile(int image) const {
    const Packed &p = images_[image];
    return {static_cast<uint16_t>(p.x), static_cast<uint16_t>(p.y), static_cast<uint16_t>(p.img.w), static_cast<uint16_t>(p.img.h)};
  }
  int missing() const { return missing_; }
  const std::string &missingNames() const { return missingNames_; }

private:
  bool load(const std::string &path, Image &img) const {
    std::vector<uint8_t> data;
    std::string err;
    for (const char *ext : {".png", ".tga", ""})
      if (pack_.read(path + ext, data, err) && decodeImage(data, img) && img.w > 0 && img.h > 0) return true;
    return false;
  }

  static void tint(const Variant &v, Image &img) {
    const std::string base = baseName(v.path);
    if (base == "grass_side") tintImage(img, kGrass, true);
    else if (base == "grass_top") tintImage(img, kGrass, false);
    else if (startsWith(base, "leaves_") && !endsWith(base, "_carried"))
      tintImage(img, base.find("spruce") != std::string::npos ? kSpruce : base.find("birch") != std::string::npos ? kBirch : kFoliage,
                false);
    else if (base == "vine") tintImage(img, kFoliage, false);
    else if (base == "still_water_grey" || base == "flowing_water_grey") tintImage(img, kWater, false);
    else if (v.color) tintImage(img, v.color, false);
  }

  const ResourcePack &pack_;
  std::vector<Packed> images_;
  std::map<std::string, int> byKey_;
  int missing_ = 0;
  std::string missingNames_;  // the first few, for the log
};

// Little-endian cache writer / reader.
struct Out {
  std::FILE *f;
  void u32(uint32_t v) { std::fwrite(&v, 4, 1, f); }
  void u16(uint16_t v) { std::fwrite(&v, 2, 1, f); }
  void str(const std::string &s) {
    u32(static_cast<uint32_t>(s.size()));
    std::fwrite(s.data(), 1, s.size(), f);
  }
};

struct In {
  std::FILE *f;
  bool ok = true;
  uint32_t u32() {
    uint32_t v = 0;
    ok = ok && std::fread(&v, 4, 1, f) == 1;
    return v;
  }
  uint16_t u16() {
    uint16_t v = 0;
    ok = ok && std::fread(&v, 2, 1, f) == 1;
    return v;
  }
  std::string str() {
    const uint32_t n = u32();
    std::string s;
    if (!ok || n > (1u << 20)) return ok = false, s;
    s.resize(n);
    ok = ok && std::fread(s.data(), 1, n, f) == n;
    return s;
  }
};

}  // namespace

bool ResourcePack::open(const std::string &archivePath, std::string &error) {
  if (!zip_.open(archivePath, error)) return false;
  // Several packs may be inside an apk; the vanilla one holds the block textures.
  const std::string suffix = "textures/terrain_texture.json";
  auto preferred = [](const std::string &root) {
    return endsWith(root, "/vanilla/") || endsWith(root, "/resource_pack/") || root == "resource_pack/";
  };
  std::string best;
  for (const auto &kv : zip_.entries()) {
    const std::string &n = kv.first;
    if (!endsWith(n, suffix.c_str()) || (n.size() > suffix.size() && n[n.size() - suffix.size() - 1] != '/')) continue;
    const std::string root = n.substr(0, n.size() - suffix.size());
    if (best.empty() || preferred(root) > preferred(best) || (preferred(root) == preferred(best) && root.size() < best.size()))
      best = root;
  }
  if (best.empty()) {
    error = "no resource pack with textures/terrain_texture.json in " + archivePath;
    return false;
  }
  root_ = best;
  return true;
}

bool ResourcePack::read(const std::string &relPath, std::vector<uint8_t> &out, std::string &error) const {
  return zip_.read(root_ + relPath, out, 64u << 20, error);
}

std::string archiveStamp(const std::string &archivePath) {
  struct stat st{};
  if (stat(archivePath.c_str(), &st) != 0) return archivePath;
  return baseName(archivePath) + ":" + std::to_string(static_cast<long long>(st.st_size)) + ":" +
         std::to_string(static_cast<long long>(st.st_mtime));
}

bool buildAtlas(const ResourcePack &pack, const std::string &stamp, TextureAtlas &out, std::string &error) {
  std::vector<uint8_t> raw;
  if (!pack.read("textures/terrain_texture.json", raw, error)) return false;
  const Json terrain = parseLoose(raw);
  if (!terrain.is_object() || !terrain.contains("texture_data") || !terrain["texture_data"].is_object()) {
    error = "terrain_texture.json is not readable";
    return false;
  }
  out = TextureAtlas{};
  out.stamp = stamp;

  Builder builder(pack);
  std::map<std::string, std::vector<int>> keyImages;
  for (const auto &kv : terrain["texture_data"].items()) {
    const Json &textures = kv.value().is_object() && kv.value().contains("textures") ? kv.value()["textures"] : Json();
    std::vector<Variant> variants;
    if (textures.is_array())
      for (const Json &j : textures) parseVariant(j, variants);
    else
      parseVariant(textures, variants);
    for (const Variant &v : variants) keyImages[kv.key()].push_back(builder.image(v));
  }
  if (keyImages.empty()) {
    error = "terrain_texture.json lists no textures";
    return false;
  }
  if (!builder.pack(out, error)) return false;
  for (const auto &[key, images] : keyImages)
    for (int i : images) out.tiles[key].push_back(builder.tile(i));

  if (!pack.read("blocks.json", raw, error)) return false;
  const Json blocks = parseLoose(raw);
  if (!blocks.is_object()) {
    error = "blocks.json is not readable";
    return false;
  }
  for (const auto &kv : blocks.items()) {
    if (!kv.value().is_object() || !kv.value().contains("textures")) continue;
    BlockModel m;
    parseFaces(kv.value()["textures"], m.faces);
    if (kv.value().contains("carried_textures")) parseFaces(kv.value()["carried_textures"], m.carried);
    out.blocks.emplace(kv.key(), std::move(m));
  }
  if (builder.missing() > 0) error = std::to_string(builder.missing()) + " texture files could not be read (" + builder.missingNames() + ")";
  return true;
}

bool saveAtlas(const TextureAtlas &a, const std::string &path) {
  const std::string tmp = path + ".tmp";
  std::FILE *f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  Out o{f};
  o.u32(kCacheMagic), o.u32(kCacheFormat), o.str(a.stamp), o.u32(a.width), o.u32(a.height);
  std::fwrite(a.rgba.data(), 1, a.rgba.size(), f);
  o.u32(static_cast<uint32_t>(a.tiles.size()));
  for (const auto &[key, tiles] : a.tiles) {
    o.str(key), o.u32(static_cast<uint32_t>(tiles.size()));
    for (const Tile &t : tiles) o.u16(t.x), o.u16(t.y), o.u16(t.w), o.u16(t.h);
  }
  o.u32(static_cast<uint32_t>(a.blocks.size()));
  for (const auto &[name, m] : a.blocks) {
    o.str(name);
    for (const auto &s : m.faces) o.str(s);
    for (const auto &s : m.carried) o.str(s);
  }
  const bool ok = std::fflush(f) == 0 && !std::ferror(f);
  std::fclose(f);
  return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool loadAtlas(const std::string &path, const std::string &stamp, TextureAtlas &out) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  In in{f};
  TextureAtlas a;
  bool ok = in.u32() == kCacheMagic && in.u32() == kCacheFormat;
  a.stamp = ok ? in.str() : std::string();
  ok = ok && in.ok && a.stamp == stamp;
  if (ok) {
    const uint32_t w = in.u32(), h = in.u32();
    ok = in.ok && w > 0 && h > 0 && w <= 8192 && h <= 8192;
    if (ok) {
      a.width = static_cast<int>(w), a.height = static_cast<int>(h);
      a.rgba.resize(static_cast<size_t>(w) * h * 4);
      ok = std::fread(a.rgba.data(), 1, a.rgba.size(), f) == a.rgba.size();
    }
  }
  if (ok) {
    const uint32_t n = in.u32();
    for (uint32_t i = 0; ok && i < n; ++i) {
      std::string key = in.str();
      const uint32_t count = in.u32();
      ok = in.ok && count <= 4096;
      auto &tiles = a.tiles[key];
      for (uint32_t k = 0; ok && k < count; ++k) {
        Tile t;
        t.x = in.u16(), t.y = in.u16(), t.w = in.u16(), t.h = in.u16();
        ok = in.ok && t.x + t.w <= a.width && t.y + t.h <= a.height;
        tiles.push_back(t);
      }
    }
  }
  if (ok) {
    const uint32_t n = in.u32();
    for (uint32_t i = 0; ok && i < n; ++i) {
      std::string name = in.str();
      BlockModel m;
      for (auto &s : m.faces) s = in.str();
      for (auto &s : m.carried) s = in.str();
      ok = in.ok;
      a.blocks.emplace(std::move(name), std::move(m));
    }
  }
  std::fclose(f);
  if (!ok) return false;
  out = std::move(a);
  return true;
}

}  // namespace bematica
