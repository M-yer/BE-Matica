#include "skin.hpp"

#include "atlas.hpp"
#include "skin_core.hpp"
#include "zipfile.hpp"

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>

namespace bematica::skin {
namespace {

namespace mm = pl::modmenu;

struct Texture {
  const char *path;
  const char *fallback;  // tried when `path` is missing
  Image image;
  NineSlice slice;
};

// Font sheets, best first: the 8x8 ASCII sheet, then the unicode page 0 sheet.
constexpr const char *kFonts[] = {"font/default8.png", "font/glyph_00.png", "font/ascii.png"};
Texture gTextures[] = {
    {"textures/ui/dialog_background_opaque.png", nullptr, {}, {}},
    {"textures/ui/button_borderless_dark.png", nullptr, {}, {}},
    {"textures/ui/button_borderless_lightpressed.png", "textures/ui/button_borderless_lighthover.png", {}, {}},
    {"textures/ui/edit_box_indent.png", nullptr, {}, {}},
};

Font gFont;
std::atomic_bool gReady{false};
std::mutex gMu;                    // guards loading and the registered-image set
std::set<std::string> gRegistered;  // overlay image ids already handed over
int gBoxImages = 0;
constexpr int kMaxBoxImages = 256;  // box sizes are few; the cap only guards against a runaway layout

void info(const char *fmt, const char *what) { __android_log_print(ANDROID_LOG_INFO, "BE-Matica", fmt, what); }

bool readImage(const ResourcePack &pack, const std::string &path, Image &out) {
  std::vector<uint8_t> bytes;
  std::string error;
  return pack.read(path, bytes, error) && decodeImage(bytes, out);
}

bool loadTexture(const ResourcePack &pack, Texture &t) {
  for (const char *path : {t.path, t.fallback}) {
    if (!path || !readImage(pack, path, t.image)) continue;
    std::vector<uint8_t> json;
    std::string error;
    const std::string jsonPath = std::string(path, std::string_view(path).size() - 4) + ".json";
    if (pack.read(jsonPath, json, error) &&
        parseNineSlice({reinterpret_cast<const char *>(json.data()), json.size()}, t.image.w, t.image.h, t.slice))
      return true;
    info("BE-Matica: Skin texture %s has no usable nine-slice data", path);
  }
  info("BE-Matica: Skin texture %s not found", t.path);
  return false;
}

// Hands `img` to the overlay under `id`, once. Called with gMu held.
bool registerOnce(const std::string &id, const Image &img) {
  if (gRegistered.count(id)) return true;
  if (img.w == 0 || !mm::registerImage(id, {img.rgba.data(), img.rgba.size()}, img.w, img.h)) return false;
  gRegistered.insert(id);
  return true;
}

}  // namespace

void load() {
  std::lock_guard lock(gMu);
  if (gReady) return;
  ResourcePack pack;
  std::string error;
  const std::vector<std::string> apks = gameArchiveCandidates();
  if (!pack.open(apks, error)) {
    info("BE-Matica: Skin unavailable: %s", error.c_str());
    return;
  }
  bool font = false;
  for (const char *path : kFonts) {
    Image sheet;
    if (readImage(pack, path, sheet) && buildFont(std::move(sheet), gFont)) {
      info("BE-Matica: Skin font %s", path);
      font = true;
      break;
    }
  }
  if (!font) info("BE-Matica: Skin unavailable: %s", "no font sheet");
  bool all = font;
  for (Texture &t : gTextures) all = loadTexture(pack, t) && all;
  // The game's pressed green is very saturated next to the grey panel; mute it halfway to grey and darken it a bit.
  for (size_t i = 0; i + 3 < gTextures[static_cast<int>(Box::ButtonOn)].image.rgba.size(); i += 4) {
    uint8_t *p = &gTextures[static_cast<int>(Box::ButtonOn)].image.rgba[i];
    const float l = 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2];
    for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(std::clamp((l + (p[c] - l) * 0.5f) * 0.85f, 0.0f, 255.0f));
  }
  if (all) gReady = true;
}

bool available() { return gReady; }

// A glyph is 7 of its 8 pixels tall, against about 0.7 em of cap height in the launcher's font; a scale a little under
// that keeps labels from outgrowing the buttons they were laid out for.
int textScale(float fontPx) { return std::max(1, static_cast<int>(std::lround(fontPx / (gFont.cellH * 1.25f)))); }
int boxScale(int scale) { return std::max(1, (scale + 1) / 2); }

float textWidth(const std::string &s, int scale) {
  return static_cast<float>(skin::textWidth(gFont, glyphCodes(gFont, s)) * scale);
}

float lineHeight(int scale) { return static_cast<float>(gFont.cellH * scale); }

void drawText(std::vector<mm::DrawCommand> &cmds, float x, float y, int scale, uint32_t color, const std::string &s,
              bool shadow) {
  const float x0 = std::round(x), y0 = std::round(y);
  int pen = 0;  // font pixels
  std::lock_guard lock(gMu);
  for (uint8_t code : glyphCodes(gFont, s)) {
    if (gFont.width[code]) {
      char id[64];
      std::snprintf(id, sizeof id, "bematica.skin.g.%u.%d.%08x.%d", code, scale, color, shadow);
      if (!gRegistered.count(id)) registerOnce(id, renderGlyph(gFont, code, color, scale, shadow));
      if (gRegistered.count(id)) {
        const float extra = shadow ? 1.0f : 0.0f;
        cmds.push_back({.type = mm::DrawCommandType::Image, .x = x0 + pen * scale, .y = y0,
                        .w = (gFont.width[code] + extra) * scale, .h = (gFont.cellH + extra) * scale,
                        .color = 0xFFFFFFFF, .imageId = id});
      }
    }
    pen += advance(gFont, code);
  }
}

bool drawBox(std::vector<mm::DrawCommand> &cmds, Box box, float x, float y, float w, float h, int scale) {
  if (!gReady) return false;
  const int iw = static_cast<int>(std::lround(w)), ih = static_cast<int>(std::lround(h));
  if (iw < 1 || ih < 1) return false;
  char id[64];
  const Texture &tex = gTextures[static_cast<int>(box)];
  std::snprintf(id, sizeof id, "bematica.skin.n.%d.%d.%d.%d", static_cast<int>(box), iw, ih, scale);
  {
    std::lock_guard lock(gMu);
    if (!gRegistered.count(id)) {
      if (gBoxImages >= kMaxBoxImages || !registerOnce(id, renderNineSlice(tex.image, tex.slice, iw, ih, scale))) return false;
      ++gBoxImages;
    }
  }
  cmds.push_back({.type = mm::DrawCommandType::Image, .x = std::round(x), .y = std::round(y), .w = static_cast<float>(iw),
                  .h = static_cast<float>(ih), .color = 0xFFFFFFFF, .imageId = id});
  return true;
}

}  // namespace bematica::skin
