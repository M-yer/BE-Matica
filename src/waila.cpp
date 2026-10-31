#include "waila.hpp"

#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bematica {

namespace mm = pl::modmenu;

namespace {

// The control panel's palette (materials_panel.cpp), plus a red for a block the structure doesn't want.
constexpr uint32_t kPanel = 0xFF18191C, kBorder = 0xFF343A40, kAccent = 0xFF4AE0A0, kText = 0xFFE9ECEF;
constexpr uint32_t kMiss = 0xFFE5534B;

void rect(std::vector<mm::DrawCommand> &cmds, float x, float y, float w, float h, uint32_t color) {
  cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x, .y = y, .w = w, .h = h, .color = color});
}

// Same as materials_panel.cpp: four quarter-circle slices per corner.
void roundRect(std::vector<mm::DrawCommand> &cmds, float x, float y, float w, float h, float r, uint32_t color) {
  r = std::min({r, w / 2, h / 2});
  constexpr int kSlices = 4;
  const float step = r / kSlices;
  for (int i = 0; i < kSlices; ++i) {
    const float dy = r - (i + 0.5f) * step, d = r - std::sqrt(std::max(0.0f, r * r - dy * dy));
    rect(cmds, x + d, y + i * step, w - 2 * d, step, color);
    rect(cmds, x + d, y + h - (i + 1) * step, w - 2 * d, step, color);
  }
  rect(cmds, x, y + r, w, h - 2 * r, color);
}

void card(std::vector<mm::DrawCommand> &cmds, float x, float y, float w, float h, float r, float e, uint32_t fill,
          uint32_t border) {
  roundRect(cmds, x, y, w, h, r, border);
  roundRect(cmds, x + e, y + e, w - 2 * e, h - 2 * e, r - e, fill);
}

void text(std::vector<mm::DrawCommand> &cmds, float x, float y, float size, uint32_t color, std::string s) {
  cmds.push_back({.type = mm::DrawCommandType::Text, .x = x, .y = y + size * 0.85f, .color = color, .size = size,
                  .text = std::move(s)});
}

// Rough label width, as in materials_panel.cpp (the launcher's font is proportional).
float textWidth(const std::string &s, float font) {
  float em = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) == 0x80) continue;  // UTF-8 continuation byte
    if (std::strchr("ijl|.,:;!'", c)) em += 0.25f;
    else if (c == ' ') em += 0.25f;
    else if (std::strchr("ftrI", c)) em += 0.34f;
    else if (std::strchr("mwMW", c)) em += 0.85f;
    else if (c >= 'A' && c <= 'Z') em += 0.65f;
    else if (c >= 0x80) em += 0.4f;
    else em += 0.53f;
  }
  return font * em;
}

}  // namespace

void LookCard::init(std::string moduleId) {
  std::lock_guard lock(mu_);
  moduleId_ = std::move(moduleId);
}

void LookCard::update(const hologram::LookInfo &info) {
  std::lock_guard lock(mu_);
  std::vector<mm::DrawCommand> cmds;
  if (info.valid) {
    const auto surface = mm::getHudSurfaceSize();
    const float sh = surface.height > 0 ? surface.height : 1080, sw = surface.width > 0 ? surface.width : 2400;
    // Two lines still fit under the launcher's toolbar and above the toast (18%): the card stays under 8% of the height.
    const float font = sh * 0.02f, pad = font * 0.6f, lineH = font * 1.35f;
    const std::string wants = !info.note.empty() ? info.note : info.match ? "Wants: " + info.want + " ✓" : "Wants: " + info.want;
    float w = textWidth(info.name, font);
    if (info.inside) w = std::max(w, textWidth(wants, font));
    w += font * 1.6f;
    const float h = pad * 2 + font + (info.inside ? lineH : 0);
    const float x = (sw - w) / 2, y = sh * 0.085f;
    const float edge = std::max(1.0f, std::round(font * 0.06f));
    card(cmds, x, y, w, h, font * 0.6f, edge, kPanel, kBorder);
    text(cmds, x + (w - textWidth(info.name, font)) / 2, y + pad, font, kText, info.name);
    if (info.inside)
      text(cmds, x + (w - textWidth(wants, font)) / 2, y + pad + lineH, font, info.match ? kAccent : kMiss, wants);
  }
  mm::submitDrawCommands(moduleId_, cmds);
}

}  // namespace bematica
