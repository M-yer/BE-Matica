#include "materials_panel.hpp"

#include <pl/ModMenu.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>

namespace bematica {

namespace mm = pl::modmenu;

namespace {

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
  visible_ = false;
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

void MaterialsPanel::setState(bool hologram, bool easyPlace, bool mirror, bool locked) {
  std::lock_guard lock(mu_);
  if (hologram == hologram_ && easyPlace == easyPlace_ && mirror == mirror_ && locked == locked_) return;
  hologram_ = hologram, easyPlace_ = easyPlace, mirror_ = mirror, locked_ = locked;
  redrawLocked();
}

void MaterialsPanel::setProgress(size_t built, size_t total) {
  std::lock_guard lock(mu_);
  if (built == built_ && total == total_) return;
  built_ = built, total_ = total;
  redrawLocked();
}

bool MaterialsPanel::insideLocked(float x, float y) const {
  return visible_ && x >= x_ && x <= x_ + w_ && y >= y_ && y <= y_ + h_;
}

bool MaterialsPanel::onTouch(const pl::input::TouchEvent &e) {
  std::string key, value;  // a tapped control, run once the lock is released
  {
    std::lock_guard lock(mu_);
    const int action = e.action & 0xff;
    if (activePointer_ < 0) {
      if ((action == kDown || action == kPointerDown) && insideLocked(e.x, e.y)) {
        activePointer_ = e.pointerId;
        downX_ = lastX_ = e.x;
        downY_ = lastY_ = e.y;
        dragged_ = false;
        movingPanel_ = e.y < y_ + headerH_;  // the header drags the panel; its buttons still work as taps
        return true;
      }
      return false;
    }
    if (e.pointerId != activePointer_) return insideLocked(e.x, e.y);

    if (action == kMove) {
      if (std::hypot(e.x - downX_, e.y - downY_) > row_ * 0.3f) dragged_ = true;
      if (dragged_ && movingPanel_) {
        px_ += e.x - lastX_, py_ += e.y - lastY_;
        redrawLocked();
      } else if (dragged_ && tab_ == Tab::Blocks && downY_ > listTop_) {
        scroll_ -= e.y - lastY_;
        redrawLocked();
      }
      lastX_ = e.x, lastY_ = e.y;
      return true;
    }
    if (action != kUp && action != kPointerUp && action != kCancel) return true;
    activePointer_ = -1;
    if (dragged_ || action == kCancel) return true;
    for (const auto &c : controls_)
      if (e.x >= c.x && e.x <= c.x + c.w && e.y >= c.y && e.y <= c.y + c.h) key = c.key, value = c.value;
    // Panel-only buttons start with '_' and never reach the mod.
    if (key == "_tab") {
      tab_ = value == "blocks" ? Tab::Blocks : Tab::Move;
      collapsed_ = false;
      redrawLocked();
      return true;
    }
    if (key == "_collapse") {
      collapsed_ = !collapsed_;
      redrawLocked();
      return true;
    }
    if (key.empty() && tab_ == Tab::Blocks && e.y > listTop_ && e.y < listBottom_) {
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

namespace {

// Dark translucent panel with flat buttons; blue marks what is on or selected.
constexpr uint32_t kPanel = 0xE6202327, kHeader = 0xF016181B, kButton = 0xFF353A40, kActive = 0xFF1C7ED6;
constexpr uint32_t kWell = 0xFF15171A, kStripe = 0x10FFFFFF, kText = 0xFFF1F3F5, kDim = 0xFFA6ACB3, kDone = 0xFF7BD389;

void rect(std::vector<mm::DrawCommand> &cmds, float x, float y, float w, float h, uint32_t color) {
  cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x, .y = y, .w = w, .h = h, .color = color});
}

// `y` is the top of a line `size` tall; the launcher places text by its baseline, so it is moved down to it.
void text(std::vector<mm::DrawCommand> &cmds, float x, float y, float size, uint32_t color, std::string s) {
  cmds.push_back({.type = mm::DrawCommandType::Text, .x = x, .y = y + size * 0.85f, .color = color, .size = size,
                  .text = std::move(s)});
}

// Rough width of a label; multi-byte UTF-8 characters count once.
float textWidth(const std::string &s, float font) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return font * 0.55f * n;
}

enum class Icon { List, Move, Up, Down, Left, Right, Lock };

// Icons from plain rectangles, centred in an s x s square at (x, y).
void icon(std::vector<mm::DrawCommand> &cmds, Icon which, float x, float y, float s) {
  const float cx = x + s / 2, cy = y + s / 2, t = std::max(2.0f, std::round(s * 0.09f));
  switch (which) {
  case Icon::List:
    for (int k = -1; k <= 1; ++k) rect(cmds, cx - s * 0.25f, cy + k * s * 0.2f - t / 2, s * 0.5f, t, kText);
    break;
  case Icon::Move:
    rect(cmds, cx - s * 0.3f, cy - t / 2, s * 0.6f, t, kText);
    rect(cmds, cx - t / 2, cy - s * 0.3f, t, s * 0.6f, kText);
    for (int k = 1; k <= 2; ++k) {  // stepped arrowheads at the four tips
      const float a = s * 0.3f - k * t, b = t * (2 * k + 1);
      rect(cmds, cx - b / 2, cy - a - t / 2, b, t, kText);
      rect(cmds, cx - b / 2, cy + a - t / 2, b, t, kText);
      rect(cmds, cx - a - t / 2, cy - b / 2, t, b, kText);
      rect(cmds, cx + a - t / 2, cy - b / 2, t, b, kText);
    }
    break;
  case Icon::Lock: {  // shackle (three bars) over a body with a keyhole
    const float bw = s * 0.46f, bh = s * 0.3f, top = cy - s * 0.02f;
    rect(cmds, cx - bw * 0.35f, top - s * 0.24f, t, s * 0.24f, kText);
    rect(cmds, cx + bw * 0.35f - t, top - s * 0.24f, t, s * 0.24f, kText);
    rect(cmds, cx - bw * 0.35f, top - s * 0.24f, bw * 0.7f, t, kText);
    rect(cmds, cx - bw / 2, top, bw, bh, kText);
    rect(cmds, cx - t / 2, top + bh * 0.3f, t, bh * 0.4f, kHeader);
    break;
  }
  case Icon::Up:
  case Icon::Down:
    for (int k = 0; k < 4; ++k) {  // a stepped triangle, 4 rows
      const float rowY = which == Icon::Up ? cy - s * 0.12f + k * t : cy + s * 0.12f - (k + 1) * t;
      rect(cmds, cx - (k + 0.5f) * t, rowY, (2 * k + 1) * t, t, kText);
    }
    break;
  case Icon::Left:
  case Icon::Right:
    for (int k = 0; k < 4; ++k) {  // the same triangle on its side
      const float colX = which == Icon::Left ? cx - s * 0.12f + k * t : cx + s * 0.12f - (k + 1) * t;
      rect(cmds, colX, cy - (k + 0.5f) * t, t, (2 * k + 1) * t, kText);
    }
    break;
  }
}

}  // namespace

void MaterialsPanel::redrawLocked() {
  std::vector<mm::DrawCommand> cmds;
  controls_.clear();
  const auto surface = mm::getHudSurfaceSize();
  const float sh = surface.height > 0 ? surface.height : 1080, sw = surface.width > 0 ? surface.width : 2400;
  if (!toast_.empty()) {
    const float font = sh * 0.04f, w = textWidth(toast_, font) + font * 1.6f, h = font * 1.9f;
    const float x = (sw - w) / 2, y = sh * 0.18f;
    rect(cmds, x, y, w, h, kPanel);
    text(cmds, x + font * 0.8f, y + (h - font) / 2, font, kText, toast_);
  }
  if (visible_) {
    const float font = sh * 0.032f, pad = font * 0.5f, ctrlH = font * 2.3f;
    row_ = font * 1.55f;
    headerH_ = font * 2.4f;
    w_ = font * 15;
    if (px_ < 0) px_ = sw * 0.21f, py_ = sh * 0.12f;  // right of the launcher's own buttons at the top left
    px_ = std::clamp(px_, 0.0f, sw - w_);
    py_ = std::clamp(py_, 0.0f, sh - headerH_);
    x_ = px_, y_ = py_;

    auto button = [&](float bx, float by, float bw, float bh, const std::string &label, const char *key,
                      const char *value, bool active) {
      rect(cmds, bx, by, bw, bh, active ? kActive : kButton);
      if (!label.empty()) text(cmds, bx + (bw - textWidth(label, font)) / 2, by + (bh - font) / 2, font, kText, label);
      controls_.push_back({bx, by, bw, bh, key, value});
    };

    // Body first, so the header is drawn over it.
    float bodyH = 0;
    if (!collapsed_) {
      const float top = y_ + headerH_ + pad, half = (w_ - pad * 3) / 2;
      float yy = top;
      if (tab_ == Tab::Move) {
        // D-pad moves along X and Z like a map (up = north, -Z); the column on the right moves along Y.
        const float c = ctrlH, gap = pad * 0.5f, dx = x_ + (w_ - (c * 4 + gap * 2 + pad * 2)) / 2;
        auto arrow = [&](float bx, float by, Icon which, const char *key) {
          button(bx, by, c, c, "", key, "", false);
          icon(cmds, which, bx, by, c);
        };
        arrow(dx + c + gap, yy, Icon::Up, "north");
        arrow(dx, yy + c + gap, Icon::Left, "west");
        arrow(dx + (c + gap) * 2, yy + c + gap, Icon::Right, "east");
        arrow(dx + c + gap, yy + (c + gap) * 2, Icon::Down, "south");
        text(cmds, dx + c + gap + (c - textWidth("X/Z", font)) / 2, yy + c + gap + (c - font) / 2, font, kDim, "X/Z");
        const float yx = dx + (c + gap) * 3 + pad * 2;
        arrow(yx, yy, Icon::Up, "up");
        text(cmds, yx + (c - textWidth("Y", font)) / 2, yy + c + gap + (c - font) / 2, font, kDim, "Y");
        arrow(yx, yy + (c + gap) * 2, Icon::Down, "down");
        yy += (c + gap) * 3 + pad;
        button(x_ + pad, yy, half, ctrlH, "Rotate +90°", "rotate", "", false);
        button(x_ + pad * 2 + half, yy, half, ctrlH, "Mirror", "mirror", "", mirror_);
        yy += ctrlH + pad * 2;

        const float unit = (w_ - pad * 5) / 6;  // arrow, label (3 units), arrow, All
        const std::string layerText = layerCount_ == 0 ? "No layers"
                                      : single_ ? "Layer " + std::to_string(layer_ + 1) + "/" + std::to_string(layerCount_)
                                                : "All " + std::to_string(layerCount_) + " layers";
        button(x_ + pad, yy, unit, ctrlH, "<", "layer_down", "", false);
        text(cmds, x_ + pad * 2 + unit + (unit * 3 - textWidth(layerText, font)) / 2, yy + (ctrlH - font) / 2, font,
             kText, layerText);
        button(x_ + pad * 3 + unit * 4, yy, unit, ctrlH, ">", "layer_up", "", false);
        button(x_ + pad * 4 + unit * 5, yy, unit, ctrlH, "All", "layers", "all", !single_);
        yy += ctrlH + pad * 2;

        button(x_ + pad, yy, half, ctrlH, "Hologram", "hologram", hologram_ ? "0" : "1", hologram_);
        button(x_ + pad * 2 + half, yy, half, ctrlH, "Easy Place", "easyplace", easyPlace_ ? "0" : "1", easyPlace_);
        yy += ctrlH + pad;
      } else {
        const std::string title = title_.empty() ? "No structure loaded"
                                                 : title_ + "   " + std::to_string(done_.size()) + "/" +
                                                       std::to_string(materials_.size());
        text(cmds, x_ + pad * 2, yy + (row_ - font) / 2, font, kDim, title);
        yy += row_ + pad;
        listTop_ = yy;
        listBottom_ = listTop_ + row_ * 6;
        const float wellX = x_ + pad, wellW = w_ - pad * 2;
        rect(cmds, wellX, listTop_, wellW, listBottom_ - listTop_, kWell);

        const float contentH = row_ * materials_.size();
        const float maxScroll = std::max(0.0f, contentH - (listBottom_ - listTop_));
        scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
        for (size_t k = static_cast<size_t>(scroll_ / row_); k < materials_.size(); ++k) {
          const float ry = listTop_ + k * row_ - scroll_;
          if (ry + row_ > listBottom_) break;
          const bool done = done_.count(materials_[k].name) > 0;
          if (k % 2) rect(cmds, wellX, ry, wellW, row_, kStripe);
          const uint32_t color = done ? kDone : kText;
          text(cmds, wellX + font * 0.5f, ry + (row_ - font) / 2, font, color,
               (done ? "\xE2\x9C\x93 " : "") + materials_[k].name);
          text(cmds, x_ + w_ * 0.62f, ry + (row_ - font) / 2, font, done ? kDone : kDim,
               countText(materials_[k].count));
        }
        if (maxScroll > 0) {
          const float trackH = listBottom_ - listTop_, barH = std::max(row_, trackH * trackH / contentH);
          rect(cmds, wellX + wellW - font * 0.4f, listTop_ + (trackH - barH) * (scroll_ / maxScroll), font * 0.3f,
               barH, kDim);
        }
        yy = listBottom_ + pad;
      }
      bodyH = yy - (y_ + headerH_);
    }
    h_ = headerH_ + bodyH;
    // The background's height is only known now, so it goes in under the body (after the toast's 2 commands).
    cmds.insert(cmds.begin() + (toast_.empty() ? 0 : 2),
                mm::DrawCommand{.type = mm::DrawCommandType::RectFilled, .x = x_, .y = y_, .w = w_, .h = h_,
                                .color = kPanel});

    // Header: progress, then Blocks / Move tabs and collapse at the right.
    rect(cmds, x_, y_, w_, headerH_, kHeader);
    const std::string progress =
        total_ == 0 ? "--" : std::to_string(static_cast<int>(built_ * 100 / total_)) + "% built";
    const float dot = std::max(2.0f, std::round(font * 0.14f));  // 2x3 grip dots: the header drags the panel
    for (int c = 0; c < 2; ++c)
      for (int r = -1; r <= 1; ++r)
        rect(cmds, x_ + pad * 1.5f + c * dot * 2.5f, y_ + headerH_ / 2 + r * dot * 2.5f - dot / 2, dot, dot, kDim);
    text(cmds, x_ + pad * 2.5f + dot * 4, y_ + (headerH_ - font) / 2, font, total_ && built_ == total_ ? kDone : kText,
         progress);
    const float s = headerH_ - pad * 2;
    float bx = x_ + w_ - pad - s;
    button(bx, y_ + pad, s, s, "", "_collapse", "", false);
    icon(cmds, collapsed_ ? Icon::Down : Icon::Up, bx, y_ + pad, s);
    bx -= s + pad;
    button(bx, y_ + pad, s, s, "", "_tab", "move", !collapsed_ && tab_ == Tab::Move);
    icon(cmds, Icon::Move, bx, y_ + pad, s);
    bx -= s + pad;
    button(bx, y_ + pad, s, s, "", "_tab", "blocks", !collapsed_ && tab_ == Tab::Blocks);
    icon(cmds, Icon::List, bx, y_ + pad, s);
    bx -= s + pad;
    button(bx, y_ + pad, s, s, "", "lock", locked_ ? "0" : "1", locked_);
    icon(cmds, Icon::Lock, bx, y_ + pad, s);
  }
  mm::submitDrawCommands(moduleId_, cmds);
}

}  // namespace bematica
