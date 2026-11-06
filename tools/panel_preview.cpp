// Renders the overlay panel to PNG files on the desktop, so the skin can be judged without a phone.
// The real MaterialsPanel / skin / icons sources are linked; this file stands in for the launcher side (pl::modmenu,
// pl::input) and rasterises the draw commands the panel submits. Usage: panel_preview <outdir> skin|flat
#include "library.hpp"
#include <pl/Input.hpp>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#define private public  // the tool taps and inspects the panel's layout; nothing else is touched
#include "materials_panel.hpp"
#undef private

#include "skin.hpp"

#include <pl/ModMenu.hpp>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "plutovg-stb-image-write.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "plutovg-stb-truetype.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace mm = pl::modmenu;

constexpr int kW = 2400, kH = 1080;  // Infinix X670

// ---- the launcher side -------------------------------------------------------------------------------------------

namespace {
struct Bitmap {
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;
};
std::mutex gMu;
std::map<std::string, Bitmap> gImages;
std::vector<mm::DrawCommand> gCmds;
pl::input::TouchCallback gTouch;
pl::input::TextInputCallback gTextIn;
}  // namespace

namespace pl::modmenu {
bool registerImage(std::string_view id, std::span<const unsigned char> data, int w, int h) {
  std::lock_guard lock(gMu);
  gImages[std::string(id)] = {w, h, {data.begin(), data.end()}};
  return true;
}
void submitDrawCommands(std::string_view, std::span<const DrawCommand> cmds) {
  std::lock_guard lock(gMu);
  gCmds.assign(cmds.begin(), cmds.end());
}
HudSurfaceSize getHudSurfaceSize() { return {kW, kH}; }
void requestOpenMenu() {}
}  // namespace pl::modmenu

namespace pl::input {
void registerTouchCallback(TouchCallback cb) { gTouch = std::move(cb); }
void registerKeyCallback(KeyCallback) {}
void registerTextInputCallback(TextInputCallback cb) { gTextIn = std::move(cb); }
void showKeyboard() {}
void hideKeyboard() {}
}  // namespace pl::input

// ---- rasteriser --------------------------------------------------------------------------------------------------

namespace {

struct Canvas {
  std::vector<unsigned char> px = std::vector<unsigned char>(kW * kH * 4);

  void blend(int x, int y, unsigned r, unsigned g, unsigned b, unsigned a) {
    if (x < 0 || y < 0 || x >= kW || y >= kH || !a) return;
    unsigned char *p = &px[(y * kW + x) * 4];
    p[0] = static_cast<unsigned char>((r * a + p[0] * (255 - a) + 127) / 255);
    p[1] = static_cast<unsigned char>((g * a + p[1] * (255 - a) + 127) / 255);
    p[2] = static_cast<unsigned char>((b * a + p[2] * (255 - a) + 127) / 255);
    p[3] = 255;
  }
  void argb(int x, int y, uint32_t c, unsigned coverage = 255) {
    blend(x, y, (c >> 16) & 255, (c >> 8) & 255, c & 255, ((c >> 24) & 255) * coverage / 255);
  }
  void fill(float x, float y, float w, float h, uint32_t c) {
    const int x0 = std::lround(x), y0 = std::lround(y), x1 = std::lround(x + w), y1 = std::lround(y + h);
    for (int yy = y0; yy < y1; ++yy)
      for (int xx = x0; xx < x1; ++xx) argb(xx, yy, c);
  }
  void circle(float cx, float cy, float r, uint32_t c) {
    for (int yy = std::floor(cy - r); yy <= std::ceil(cy + r); ++yy)
      for (int xx = std::floor(cx - r); xx <= std::ceil(cx + r); ++xx)
        if (std::hypot(xx + 0.5f - cx, yy + 0.5f - cy) <= r) argb(xx, yy, c);
  }
};

int gScaledImages = 0, gLauncherText = 0;
std::set<std::string> gWarned;
void warnOnce(const std::string &what) {
  if (gWarned.insert(what).second) std::fprintf(stderr, "WARN: %s\n", what.c_str());
}

std::vector<unsigned char> gFontFile;
stbtt_fontinfo gFont;
bool gHaveFont = false;

void loadFont() {
  for (const char *path : {"/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/google-noto/NotoSans-Regular.ttf"}) {
    std::ifstream f(path, std::ios::binary);
    if (!f) continue;
    gFontFile.assign(std::istreambuf_iterator<char>(f), {});
    gHaveFont = stbtt_InitFont(&gFont, gFontFile.data(), 0);
    if (gHaveFont) return;
  }
}

// Stand-in for the launcher's own font (the real one is not available here). `y` is the baseline.
void drawLauncherText(Canvas &cv, const mm::DrawCommand &c) {
  ++gLauncherText;
  if (!gHaveFont) return cv.fill(c.x, c.y - c.size * 0.7f, c.size * 0.5f * c.text.size(), c.size * 0.7f, c.color);
  const float sc = stbtt_ScaleForPixelHeight(&gFont, c.size * 1.15f);
  float pen = c.x;
  for (size_t i = 0; i < c.text.size();) {
    unsigned char b = c.text[i];
    int cp = b, n = 1;
    if (b >= 0xF0) cp = b & 7, n = 4; else if (b >= 0xE0) cp = b & 15, n = 3; else if (b >= 0xC0) cp = b & 31, n = 2;
    for (int k = 1; k < n && i + k < c.text.size(); ++k) cp = (cp << 6) | (c.text[i + k] & 63);
    i += n;
    int w = 0, h = 0, ox = 0, oy = 0;
    unsigned char *bm = stbtt_GetCodepointBitmap(&gFont, sc, sc, cp, &w, &h, &ox, &oy);
    for (int yy = 0; yy < h; ++yy)
      for (int xx = 0; xx < w; ++xx) cv.argb(std::lround(pen) + ox + xx, std::lround(c.y) + oy + yy, c.color, bm[yy * w + xx]);
    stbtt_FreeBitmap(bm, nullptr);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&gFont, cp, &adv, &lsb);
    pen += adv * sc;
  }
}

void render(const std::vector<mm::DrawCommand> &cmds, const std::string &path, int cx0, int cy0, int cx1, int cy1) {
  Canvas cv;
  for (int y = 0; y < kH; ++y)  // sky over grass
    for (int x = 0; x < kW; ++x) {
      unsigned char *p = &cv.px[(y * kW + x) * 4];
      const bool grass = y > kH * 0.62f;
      const int n = (x * 7 + y * 13) % 9;
      p[0] = grass ? 86 + n : 130 + y / 20, p[1] = grass ? 140 + n : 180 + y / 30, p[2] = grass ? 62 : 235, p[3] = 255;
    }
  for (const auto &c : cmds) {
    switch (c.type) {
    case mm::DrawCommandType::RectFilled: cv.fill(c.x, c.y, c.w, c.h, c.color); break;
    case mm::DrawCommandType::Rect:
      cv.fill(c.x, c.y, c.w, 1, c.color), cv.fill(c.x, c.y + c.h - 1, c.w, 1, c.color);
      cv.fill(c.x, c.y, 1, c.h, c.color), cv.fill(c.x + c.w - 1, c.y, 1, c.h, c.color);
      break;
    case mm::DrawCommandType::CircleFilled: cv.circle(c.x, c.y, c.size, c.color); break;
    case mm::DrawCommandType::Text: drawLauncherText(cv, c); break;
    case mm::DrawCommandType::Image: {
      const auto it = gImages.find(c.imageId);
      if (it == gImages.end()) {
        warnOnce("image not registered: " + c.imageId);
        break;
      }
      const Bitmap &bm = it->second;
      const int dx = std::lround(c.x), dy = std::lround(c.y), dw = std::lround(c.w), dh = std::lround(c.h);
      if (dw != bm.w || dh != bm.h) {
        ++gScaledImages;
        warnOnce("image drawn scaled (" + std::to_string(bm.w) + "x" + std::to_string(bm.h) + " -> " + std::to_string(dw) +
                 "x" + std::to_string(dh) + "): " + c.imageId);
      }
      for (int yy = 0; yy < dh; ++yy)
        for (int xx = 0; xx < dw; ++xx) {
          const unsigned char *s = &bm.rgba[((yy * bm.h / dh) * bm.w + xx * bm.w / dw) * 4];
          const unsigned a = s[3] * ((c.color >> 24) & 255) / 255;
          cv.blend(dx + xx, dy + yy, s[0] * ((c.color >> 16) & 255) / 255, s[1] * ((c.color >> 8) & 255) / 255,
                   s[2] * (c.color & 255) / 255, a);
        }
      break;
    }
    default: warnOnce("draw command type not rasterised: " + std::to_string(static_cast<int>(c.type)));
    }
  }
  stbi_write_png(path.c_str(), kW, kH, 4, cv.px.data(), kW * 4);
  cx0 = std::clamp(cx0, 0, kW - 1), cy0 = std::clamp(cy0, 0, kH - 1), cx1 = std::clamp(cx1, cx0 + 1, kW), cy1 = std::clamp(cy1, cy0 + 1, kH);
  std::vector<unsigned char> crop;
  for (int y = cy0; y < cy1; ++y) crop.insert(crop.end(), &cv.px[(y * kW + cx0) * 4], &cv.px[(y * kW + cx1) * 4]);
  stbi_write_png((path.substr(0, path.size() - 4) + "_crop.png").c_str(), cx1 - cx0, cy1 - cy0, 4, crop.data(), (cx1 - cx0) * 4);
  std::printf("%s\n", path.c_str());
}

// ---- scenario ----------------------------------------------------------------------------------------------------

bool tap(float x, float y) {
  if (!gTouch) return false;
  gTouch({0, 0, x, y});
  gTouch({1, 0, x, y});
  return true;
}

bool tapControl(bematica::MaterialsPanel &p, const char *key, const char *value = nullptr) {
  for (const auto &c : p.controls_)
    if (std::string_view(c.key) == key && (!value || std::string_view(c.value) == value)) return tap(c.x + c.w / 2, c.y + c.h / 2);
  std::fprintf(stderr, "WARN: no control %s %s\n", key, value ? value : "");
  return false;
}

void snap(bematica::MaterialsPanel &p, const std::string &dir, const std::string &name) {
  std::vector<mm::DrawCommand> cmds;
  {
    std::lock_guard lock(gMu);
    cmds = gCmds;
  }
  const int m = 40;
  render(cmds, dir + "/" + name + ".png", std::min<int>(p.x_, p.bx_) - m, std::min<int>(p.y_, p.by_) - m,
         std::max<int>(p.x_ + p.w_, p.bx_ + p.bs_) + m, std::max<int>(p.y_ + p.h_, p.by_ + p.bs_) + m);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) return std::fprintf(stderr, "usage: panel_preview <outdir> skin|flat\n"), 2;
  const std::string dir = argv[1];
  const bool wantSkin = std::string(argv[2]) == "skin";
  loadFont();

  bematica::MaterialsPanel panel;
  panel.init("preview", "bematica", [](std::string_view, std::string_view) {});
  for (int i = 0; i < 100 && !bematica::skin::available(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the load thread's own redraw
  if (wantSkin && !bematica::skin::available()) return std::fprintf(stderr, "skin did not load\n"), 1;

  std::vector<bematica::Material> mats = {
      {"stripped_spruce_wood", 5134, ""}, {"spruce_planks", 1054, ""}, {"gray_concrete", 839, ""},
      {"spruce_stairs", 651, "oak_stairs"}, {"spruce_slab", 474, ""}, {"spruce_shelf", 335, ""},
      {"oak_sign", 40, ""}, {"red_bed", 3, ""}};
  panel.setContent("medieval house", mats);
  panel.setProgress(37, 100);
  panel.setLayer(2, 12, true);
  panel.setState(true, false, false, false);
  panel.setVisible(true);
  const std::string mode = wantSkin ? "" : "_fallback";

  if (wantSkin) snap(panel, dir, "move");
  tapControl(panel, "_tab", "blocks");
  // Tick the second row by tapping it, like a player would.
  tap(panel.x_ + panel.w_ * 0.4f, panel.listTop_ + panel.row_ * 1.5f);
  snap(panel, dir, "blocks" + mode);
  if (wantSkin) {
    tapControl(panel, "_sub", "spruce_slab");
    panel.onText({"spruce"});
    snap(panel, dir, "substitute");
    tapControl(panel, "_back");
    panel.toast("Layer 3/12");
    snap(panel, dir, "toast");
    tapControl(panel, "_collapse");
    snap(panel, dir, "collapsed");
    tapControl(panel, "_collapse");
    tap(panel.bx_ + panel.bs_ / 2, panel.by_ + panel.bs_ / 2);  // the round button closes the panel
    snap(panel, dir, "button");
  }
  std::fprintf(stderr, "launcher-font Text commands drawn: %d; scaled images: %d\n", gLauncherText, gScaledImages);
  if (wantSkin && gLauncherText) std::fprintf(stderr, "WARN: skin is on but text still goes through the launcher font\n");
  std::fflush(nullptr);
  std::_Exit(0);  // the panel's detached timer threads would otherwise outlive main
}
