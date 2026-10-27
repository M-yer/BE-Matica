#include "materials_panel.hpp"

#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>

namespace bematica {

namespace mm = pl::modmenu;

namespace {

constexpr const char *kButtonId = "bematica.blocks";
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

void MaterialsPanel::init(std::string moduleId, std::string modId) {
  moduleId_ = std::move(moduleId);
  modId_ = std::move(modId);
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
    buttonRegistered_ = mm::ButtonBuilder(kButtonId, "BE-Matica blocks")
                            .moduleId(moduleId_)
                            .modId(modId_)
                            .label("Blocks")
                            .sizeScale(1.6f, 0.8f)
                            .onEvent([this](std::string_view, mm::ButtonEvent e, float) {
                              if (e == mm::ButtonEvent::Click) toggleOpen();
                            })
                            .registerButton();
  } else if (!visible && buttonRegistered_) {
    mm::unregisterButton(kButtonId);
    buttonRegistered_ = false;
  }
  if (!visible) open_ = false;
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
    if (std::abs(e.y - downY_) > row_ * 0.3f) dragged_ = true;
    if (dragged_) {
      scroll_ -= e.y - lastY_;
      redrawLocked();
    }
    lastY_ = e.y;
  } else if (action == kUp || action == kPointerUp || action == kCancel) {
    if (!dragged_ && action != kCancel && e.y > y_ + headerH_) {
      const auto idx = static_cast<size_t>((e.y - y_ - headerH_ + scroll_) / row_);
      if (idx < materials_.size()) {
        const auto &name = materials_[idx].name;
        if (!done_.erase(name)) done_.insert(name);
        redrawLocked();
      }
    }
    activePointer_ = -1;
  }
  return true;
}

void MaterialsPanel::redrawLocked() {
  std::vector<mm::DrawCommand> cmds;
  if (visible_ && open_) {
    const auto surface = mm::getHudSurfaceSize();
    const float sh = surface.height > 0 ? surface.height : 1080, sw = surface.width > 0 ? surface.width : 2400;
    const float font = sh * 0.034f;
    row_ = font * 1.55f;
    headerH_ = row_ * 1.4f;
    w_ = std::min(sw * 0.36f, font * 20);
    h_ = sh * 0.66f;
    x_ = sw * 0.02f;
    y_ = sh * 0.14f;

    const float contentH = row_ * materials_.size();
    const float maxScroll = std::max(0.0f, contentH - (h_ - headerH_));
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);

    cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x_, .y = y_, .w = w_, .h = h_, .color = 0xD8101418});
    cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x_, .y = y_, .w = w_, .h = headerH_,
                    .color = 0xFF1E2A24});
    cmds.push_back({.type = mm::DrawCommandType::Text, .x = x_ + font * 0.6f, .y = y_ + (headerH_ - font) / 2,
                    .color = 0xFFFFFFFF, .size = font,
                    .text = title_ + "   " + std::to_string(done_.size()) + "/" + std::to_string(materials_.size())});

    const float top = y_ + headerH_;
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
      const float trackH = h_ - headerH_, barH = std::max(row_, trackH * trackH / contentH);
      cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x_ + w_ - font * 0.3f,
                      .y = top + (trackH - barH) * (scroll_ / maxScroll), .w = font * 0.2f, .h = barH,
                      .color = 0x80FFFFFF});
    }
  }
  mm::submitDrawCommands(moduleId_, cmds);
}

}  // namespace bematica
