#include "skin.hpp"

#include "atlas.hpp"
#include "skin_core.hpp"
#include "zipfile.hpp"

#include <android/log.h>
#include <plutovg.h>

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

// Newer games (1.26+) ship their pixel font only as Mojangles.ttf. It is drawn into a 16x16 sheet of 8x8 cells like
// default8.png: at 10 px one font pixel is one sheet pixel, glyphs start half a pixel in, baseline on row 7.
bool fontFromTtf(const std::vector<std::string> &apks, Image &sheet) {
  std::vector<uint8_t> ttf;
  for (const std::string &apk : apks) {
    ZipArchive zip;
    std::string error;
    if (!zip.open(apk, error)) continue;
    for (const auto &kv : zip.entries())
      if (kv.first.ends_with("fonts/Mojangles.ttf") && zip.read(kv.first, ttf, 8u << 20, error)) break;
    if (!ttf.empty()) break;
  }
  if (ttf.empty()) return false;
  plutovg_font_face_t *face = plutovg_font_face_load_from_data(ttf.data(), static_cast<unsigned>(ttf.size()), 0, nullptr, nullptr);
  if (!face) return false;
  plutovg_surface_t *surface = plutovg_surface_create(128, 128);
  plutovg_canvas_t *canvas = plutovg_canvas_create(surface);
  plutovg_canvas_set_font(canvas, face, 10);
  plutovg_canvas_set_rgba(canvas, 1, 1, 1, 1);
  for (int code = 33; code < 256; ++code) {
    const uint32_t cp = code == 0xF8 ? 0xB0 : code < 127 ? code : 0;  // 0xF8 is the degree sign, as in the ascii sheets
    if (!cp) continue;
    plutovg_canvas_fill_text(canvas, &cp, 1, PLUTOVG_TEXT_ENCODING_UTF32, (code % 16) * 8 - 0.5f, (code / 16) * 8 + 7);
  }
  // plutovg's pixels are premultiplied BGRA; the sheet wants white with hard alpha.
  sheet.w = sheet.h = 128;
  sheet.rgba.assign(128 * 128 * 4, 0);
  const uint8_t *px = plutovg_surface_get_data(surface);
  const int stride = plutovg_surface_get_stride(surface);
  for (int y = 0; y < 128; ++y)
    for (int x = 0; x < 128; ++x)
      if (px[y * stride + x * 4 + 3] >= 128) std::fill_n(&sheet.rgba[(y * 128 + x) * 4], 4, uint8_t{255});
  plutovg_canvas_destroy(canvas);
  plutovg_surface_destroy(surface);
  plutovg_font_face_destroy(face);
  return true;
}

// Newer games draw their menus in code, so the old UI textures are gone; the same bevelled look is drawn here instead.
// 8x8 pixels with 3-pixel borders: outline, highlight on the top and left, shadow on the bottom and right.
void bevel(Texture &t, uint32_t outline, uint32_t light, uint32_t fill, uint32_t dark, int lightW, int darkW) {
  t.image.w = t.image.h = 8;
  t.image.rgba.assign(8 * 8 * 4, 0);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      const bool edge = x == 0 || y == 0 || x == 7 || y == 7, corner = (x == 0 || x == 7) && (y == 0 || y == 7);
      if (corner) continue;
      const uint32_t c = edge                                    ? outline
                         : x >= 7 - darkW || y >= 7 - darkW      ? dark
                         : x <= lightW || y <= lightW            ? light
                                                                 : fill;
      uint8_t *p = &t.image.rgba[(y * 8 + x) * 4];
      p[0] = c >> 16 & 0xFF, p[1] = c >> 8 & 0xFF, p[2] = c & 0xFF, p[3] = 0xFF;
    }
  t.slice = {3, 3, 3, 3};
}

void drawnTexture(int box, Texture &t) {
  switch (static_cast<Box>(box)) {
    case Box::Panel: return bevel(t, 0x000000, 0xFFFFFF, 0xC6C6C6, 0x555555, 2, 2);
    case Box::Button: return bevel(t, 0x000000, 0x5A5A5A, 0x3A3A3A, 0x1E1E1E, 1, 2);
    case Box::ButtonOn: return bevel(t, 0x000000, 0x6CC349, 0x3C8527, 0x1D4D13, 1, 2);
    case Box::Well: return bevel(t, 0x1E1E1E, 0x2E2E2E, 0x585858, 0x7A7A7A, 1, 1);
  }
}

}  // namespace

void load() {
  std::lock_guard lock(gMu);
  if (gReady) return;
  ResourcePack pack;
  std::string error;
  const std::vector<std::string> apks = gameArchiveCandidates();
  const bool havePack = pack.open(apks, error);
  // Mojangles.ttf first: in 1.26 games default8.png still exists (inside font.brarchive) but no longer is a plain
  // 16x16 grid of 8x8 glyphs, so it draws garbage. The sheets are for older games that have no Mojangles.ttf.
  bool font = false;
  if (Image sheet; fontFromTtf(apks, sheet) && buildFont(std::move(sheet), gFont)) {
    info("BE-Matica: Skin font %s", "Mojangles.ttf");
    font = true;
  }
  for (const char *path : kFonts) {
    Image sheet;
    if (font) break;
    if (havePack && readImage(pack, path, sheet) && buildFont(std::move(sheet), gFont)) {
      info("BE-Matica: Skin font %s", path);
      font = true;
    }
  }
  if (!font) {
    info("BE-Matica: Skin unavailable: no font (%s)", havePack ? "no font sheet or Mojangles.ttf" : error.c_str());
    return;
  }
  const bool all = font;
  for (int i = 0; i < static_cast<int>(std::size(gTextures)); ++i)
    if (!havePack || !loadTexture(pack, gTextures[i])) drawnTexture(i, gTextures[i]);
  // The game's pressed green is very saturated next to the grey panel; mute it halfway to grey and darken it a bit.
  for (size_t i = 0; i + 3 < gTextures[static_cast<int>(Box::ButtonOn)].image.rgba.size(); i += 4) {
    uint8_t *p = &gTextures[static_cast<int>(Box::ButtonOn)].image.rgba[i];
    const float l = 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2];
    for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(std::clamp((l + (p[c] - l) * 0.5f) * 0.85f, 0.0f, 255.0f));
  }
  if (all) gReady = true;
}

std::atomic<bool> gEnabled{true};

void setEnabled(bool on) { gEnabled = on; }
bool available() { return gReady && gEnabled; }

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
