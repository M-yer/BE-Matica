#include "materials_panel.hpp"

#include <pl/ModMenu.hpp>

#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>

namespace bematica {

namespace mm = pl::modmenu;

namespace {

constexpr const char *kButtonId = "bematica.blocks";
// BE-Matica's own pixel-art chest, drawn on a 16x16 grid.
constexpr const char *kChestIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" shape-rendering="crispEdges">
<rect x="1" y="2" width="14" height="13" fill="#2b1a0e"/>
<rect x="2" y="3" width="12" height="4" fill="#a8743a"/>
<rect x="2" y="3" width="12" height="1" fill="#c99350"/>
<rect x="2" y="7" width="12" height="1" fill="#2b1a0e"/>
<rect x="2" y="8" width="12" height="6" fill="#8a5a2b"/>
<rect x="2" y="13" width="12" height="1" fill="#5e3a1a"/>
<rect x="2" y="8" width="1" height="6" fill="#a8743a"/>
<rect x="6" y="5" width="4" height="5" fill="#2b1a0e"/>
<rect x="7" y="6" width="2" height="3" fill="#f2c14e"/>
<rect x="7" y="8" width="2" height="1" fill="#c8962c"/>
</svg>)";
// Android MotionEvent actions.
constexpr int kDown = 0, kUp = 1, kMove = 2, kCancel = 3, kPointerDown = 5, kPointerUp = 6;

std::string countText(size_t n) {
  std::string s = std::to_string(n);
  if (n >= 64) {
    s += "  (" + std::to_string(n / 64) + " st";
    if (n % 64) s += " + " + std::to_string(n % 64);
    s += ")";
  }
  return s;
}

}  // namespace

void MaterialsPanel::init(std::string moduleId, std::string modId,
                          std::function<void(std::string_view, std::string_view)> control) {
  moduleId_ = std::move(moduleId);
  modId_ = std::move(modId);
  control_ = std::move(control);
  pl::input::registerTouchCallback([this](const pl::input::TouchEvent &e) { return onTouch(e); });
}

void MaterialsPanel::shutdown() {
  std::lock_guard lock(mu_);
  if (buttonRegistered_) mm::unregisterButton(kButtonId);
  buttonRegistered_ = false;
  visible_ = open_ = false;
  redrawLocked();
}

void MaterialsPanel::setContent(std::string title, std::vector<Material> materials) {
  std::lock_guard lock(mu_);
  title_ = std::move(title);
  materials_ = std::move(materials);
  done_.clear();
  scroll_ = 0;
  redrawLocked();
}

void MaterialsPanel::setVisible(bool visible) {
  std::lock_guard lock(mu_);
  visible_ = visible;
  if (visible && !buttonRegistered_) {
    buttonRegistered_ = mm::ButtonBuilder(kButtonId, "BE-Matica controls")
                            .moduleId(moduleId_)
                            .modId(modId_)
                            .label("Controls")
                            .svgIcon(kChestIcon)
                            .sizeScale(1.0f, 1.0f)
                            .onEvent([this](std::string_view, mm::ButtonEvent e, float) {
                              if (e == mm::ButtonEvent::Click) toggleOpen();
                            })
                            .registerButton();
    // A rejected button fails silently in the launcher; this shows it in logcat.
    if (!buttonRegistered_) __android_log_print(ANDROID_LOG_WARN, "BE-Matica", "Controls button was not registered");
  } else if (!visible && buttonRegistered_) {
    mm::unregisterButton(kButtonId);
    buttonRegistered_ = false;
  }
  if (!visible) open_ = false;
  redrawLocked();
}

void MaterialsPanel::toast(std::string text) {
  uint64_t id;
  {
    std::lock_guard lock(mu_);
    if (text == toast_) return;  // repeated taps keep the current message
    toast_ = std::move(text);
    id = ++toastId_;
    redrawLocked();
  }
  std::thread([this, id] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    std::lock_guard lock(mu_);
    if (toastId_ != id) return;
    toast_.clear();
    redrawLocked();
  }).detach();
}

void MaterialsPanel::setLayer(int layer, int layerCount, bool single) {
  std::lock_guard lock(mu_);
  if (layer == layer_ && layerCount == layerCount_ && single == single_) return;
  layer_ = layer, layerCount_ = layerCount, single_ = single;
  redrawLocked();
}

void MaterialsPanel::toggleOpen() {
  std::lock_guard lock(mu_);
  open_ = !open_;
  redrawLocked();
}

bool MaterialsPanel::insideLocked(float x, float y) const {
  return open_ && x >= x_ && x <= x_ + w_ && y >= y_ && y <= y_ + h_;
}

bool MaterialsPanel::onTouch(const pl::input::TouchEvent &e) {
  std::string key, value;  // a tapped control, run once the lock is released
  {
    std::lock_guard lock(mu_);
    const int action = e.action & 0xff;
    if (activePointer_ < 0) {
      if ((action == kDown || action == kPointerDown) && insideLocked(e.x, e.y)) {
        activePointer_ = e.pointerId;
        downY_ = lastY_ = e.y;
        dragged_ = false;
        return true;
      }
      return false;
    }
    if (e.pointerId != activePointer_) return insideLocked(e.x, e.y);

    if (action == kMove) {
      if (downY_ > listTop_ && std::abs(e.y - downY_) > row_ * 0.3f) dragged_ = true;
      if (dragged_) {
        scroll_ -= e.y - lastY_;
        redrawLocked();
      }
      lastY_ = e.y;
      return true;
    }
    if (action != kUp && action != kPointerUp && action != kCancel) return true;
    activePointer_ = -1;
    if (dragged_ || action == kCancel) return true;
    for (const auto &c : controls_)
      if (e.x >= c.x && e.x <= c.x + c.w && e.y >= c.y && e.y <= c.y + c.h) key = c.key, value = c.value;
    if (key.empty() && e.y > listTop_) {
      const auto idx = static_cast<size_t>((e.y - listTop_ + scroll_) / row_);
      if (idx < materials_.size()) {
        const auto &name = materials_[idx].name;
        if (!done_.erase(name)) done_.insert(name);
        redrawLocked();
      }
    }
  }
  // The callback locks the mod, which calls back into this panel.
  if (!key.empty() && control_) control_(key, value);
  return true;
}

void MaterialsPanel::redrawLocked() {
  std::vector<mm::DrawCommand> cmds;
  if (!toast_.empty()) {
    const auto surface = mm::getHudSurfaceSize();
    const float sh = surface.height > 0 ? surface.height : 1080, sw = surface.width > 0 ? surface.width : 2400;
    const float font = sh * 0.04f, w = font * 0.55f * toast_.size() + font * 1.6f, h = font * 1.8f;
    const float x = (sw - w) / 2, y = sh * 0.18f;
    cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x, .y = y, .w = w, .h = h, .color = 0xE0101418});
    cmds.push_back({.type = mm::DrawCommandType::Text, .x = x + font * 0.8f, .y = y + (h - font) / 2,
                    .color = 0xFFFFD866, .size = font, .text = toast_});
  }
  if (visible_ && open_) {
    const auto surface = mm::getHudSurfaceSize();
    const float sh = surface.height > 0 ? surface.height : 1080, sw = surface.width > 0 ? surface.width : 2400;
    const float font = sh * 0.034f;
    row_ = font * 1.55f;
    headerH_ = row_ * 1.4f;
    w_ = std::min(sw * 0.36f, font * 20);
    h_ = sh * 0.74f;
    x_ = sw * 0.02f;
    y_ = sh * 0.14f;

    // Controls: [<] Layer n/m [>] [All], then [Move up] [Move down].
    controls_.clear();
    const float pad = font * 0.4f, ctrlH = row_ * 1.15f, rowY1 = y_ + headerH_ + pad, rowY2 = rowY1 + ctrlH + pad;
    const float unit = (w_ - pad * 5) / 6;  // row 1: arrow, label (3 units), arrow, All
    auto button = [&](float bx, float by, float bw, const std::string &text, const char *key, const char *value,
                      bool active) {
      cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = bx, .y = by, .w = bw, .h = ctrlH,
                      .color = active ? 0xFF2F7D5B : 0xFF3A4048});
      cmds.push_back({.type = mm::DrawCommandType::Text, .x = bx + (bw - font * 0.55f * text.size()) / 2,
                      .y = by + (ctrlH - font) / 2, .color = 0xFFFFFFFF, .size = font, .text = text});
      controls_.push_back({bx, by, bw, ctrlH, key, value});
    };
    const std::string layerText = single_ ? "Layer " + std::to_string(layer_ + 1) + "/" + std::to_string(layerCount_)
                                          : "All " + std::to_string(layerCount_) + " layers";
    button(x_ + pad, rowY1, unit, "<", "layer_down", "", false);
    cmds.push_back({.type = mm::DrawCommandType::Text,
                    .x = x_ + pad * 2 + unit + (unit * 3 - font * 0.55f * layerText.size()) / 2,
                    .y = rowY1 + (ctrlH - font) / 2, .color = 0xFFFFFFFF, .size = font, .text = layerText});
    button(x_ + pad * 3 + unit * 4, rowY1, unit, ">", "layer_up", "", false);
    button(x_ + pad * 4 + unit * 5, rowY1, unit, "All", "layers", "all", !single_);
    const float half = (w_ - pad * 3) / 2;
    button(x_ + pad, rowY2, half, "Move up", "up", "", false);
    button(x_ + pad * 2 + half, rowY2, half, "Move down", "down", "", false);
    listTop_ = rowY2 + ctrlH + pad;

    const float contentH = row_ * materials_.size();
    const float maxScroll = std::max(0.0f, contentH - (y_ + h_ - listTop_));
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);

    cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x_, .y = y_, .w = w_, .h = h_, .color = 0xD8101418});
    cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x_, .y = y_, .w = w_, .h = headerH_,
                    .color = 0xFF1E2A24});
    cmds.push_back({.type = mm::DrawCommandType::Text, .x = x_ + font * 0.6f, .y = y_ + (headerH_ - font) / 2,
                    .color = 0xFFFFFFFF, .size = font,
                    .text = title_ + "   " + std::to_string(done_.size()) + "/" + std::to_string(materials_.size())});

    const float top = listTop_;
    const size_t first = static_cast<size_t>(scroll_ / row_);
    for (size_t k = first; k < materials_.size(); ++k) {
      const float ry = top + k * row_ - scroll_;
      if (ry + row_ > y_ + h_) break;
      const bool done = done_.count(materials_[k].name) > 0;
      if (k % 2) cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x_, .y = ry, .w = w_, .h = row_,
                                 .color = 0x18FFFFFF});
      const uint32_t color = done ? 0xFF6B8F71 : 0xFFFFFFFF;
      cmds.push_back({.type = mm::DrawCommandType::Text, .x = x_ + font * 0.6f, .y = ry + (row_ - font) / 2,
                      .color = color, .size = font, .text = (done ? "\xE2\x9C\x93 " : "") + materials_[k].name});
      cmds.push_back({.type = mm::DrawCommandType::Text, .x = x_ + w_ * 0.62f, .y = ry + (row_ - font) / 2,
                      .color = color, .size = font, .text = countText(materials_[k].count)});
    }

    if (maxScroll > 0) {  // scrollbar
      const float trackH = y_ + h_ - listTop_, barH = std::max(row_, trackH * trackH / contentH);
      cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x_ + w_ - font * 0.3f,
                      .y = top + (trackH - barH) * (scroll_ / maxScroll), .w = font * 0.2f, .h = barH,
                      .color = 0x80FFFFFF});
    }
  }
  mm::submitDrawCommands(moduleId_, cmds);
}

}  // namespace bematica
