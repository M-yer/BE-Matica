#include <cstring>
#include "materials_panel.hpp"

#include "blockmap.hpp"

#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cctype>
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
  pl::input::registerTextInputCallback([this](const pl::input::TextInputEvent &e) { return onText(e); });
  pl::input::registerKeyCallback([this](const pl::input::KeyEvent &e) { return onKey(e); });
}

void MaterialsPanel::searchLocked() {
  results_.clear();
  std::string needle;
  for (unsigned char c : search_) needle += c == ' ' ? '_' : static_cast<char>(std::tolower(c));
  if (needle.empty()) return;
  for (const auto &b : BlockMap::bedrockBlocks()) {
    std::string name = b.name.substr(b.name.rfind(':') + 1);
    if (name.find(needle) == std::string::npos || name == "air" || name == "structure_void" || name == subFor_) continue;
    results_.push_back(std::move(name));
  }
  // Names starting with the search come first: "stone" before "cobblestone".
  std::stable_partition(results_.begin(), results_.end(), [&](const std::string &n) { return n.rfind(needle, 0) == 0; });
}

void MaterialsPanel::closeSubLocked() {
  if (typing_) pl::input::hideKeyboard();
  typing_ = false;
  subFor_.clear();
  search_.clear();
  results_.clear();
  scroll_ = 0;
}

bool MaterialsPanel::onText(const pl::input::TextInputEvent &e) {
  std::lock_guard lock(mu_);
  if (!typing_) return false;
  search_ += e.text;
  searchLocked();
  scroll_ = 0;
  redrawLocked();
  return true;
}

// Android key codes: DEL (backspace) 67, ENTER 66, BACK 4. A hardware keyboard (scrcpy) types through here too.
bool MaterialsPanel::onKey(const pl::input::KeyEvent &e) {
  std::lock_guard lock(mu_);
  if (!typing_) return false;
  if (!e.isKeyDown) return true;
  if (e.keyCode == 67) {
    while (!search_.empty() && (static_cast<unsigned char>(search_.back()) & 0xC0) == 0x80) search_.pop_back();
    if (!search_.empty()) search_.pop_back();
  } else if (e.keyCode == 66 || e.keyCode == 4) {
    typing_ = false;
    pl::input::hideKeyboard();
  } else if (e.unicodeChar >= 32 && e.unicodeChar < 127) {
    search_ += static_cast<char>(e.unicodeChar);
  } else {
    return true;
  }
  searchLocked();
  scroll_ = 0;
  redrawLocked();
  return true;
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
  closeSubLocked();
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
    if (key == "_sub") {  // a row's ⋯
      subFor_ = value;
      search_.clear();
      results_.clear();
      scroll_ = 0;
      typing_ = true;
      pl::input::showKeyboard();
      redrawLocked();
      return true;
    }
    if (key == "_search") {
      typing_ = true;
      pl::input::showKeyboard();
      redrawLocked();
      return true;
    }
    if (key == "_pick" || key == "_held" || key == "_undo" || key == "_back") {
      const std::string from = subFor_;
      closeSubLocked();
      redrawLocked();
      if (key == "_back") return true;
      value = key == "_pick" ? from + ">" + value : from;
      key = key == "_pick" ? "substitute" : key == "_held" ? "sub_held" : "sub_undo";
    }
    if (key == "_collapse") {
      collapsed_ = !collapsed_;
      redrawLocked();
      return true;
    }
    if (key.empty() && tab_ == Tab::Blocks && subFor_.empty() && e.y > listTop_ && e.y < listBottom_) {
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

// LeviLauncher's mod menu look: charcoal surfaces, rounded cards with a thin grey border, and mint green for
// whatever is on or selected (green text on a dark green fill).
constexpr uint32_t kPanel = 0xFF18191C, kBorder = 0xFF343A40, kButton = 0xFF24282C, kActiveFill = 0xFF25463B;
constexpr uint32_t kAccent = 0xFF4AE0A0, kWell = 0xFF151719, kStripe = 0x0AFFFFFF, kText = 0xFFE9ECEF, kDim = 0xFF8D949B;

void rect(std::vector<mm::DrawCommand> &cmds, float x, float y, float w, float h, uint32_t color) {
  cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = x, .y = y, .w = w, .h = h, .color = color});
}

// The overlay has no rounded rectangle, so each corner is built from four slices of a quarter circle.
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

// A rounded card: a border-coloured shape with the fill inset by `e`.
void card(std::vector<mm::DrawCommand> &cmds, float x, float y, float w, float h, float r, float e, uint32_t fill,
          uint32_t border) {
  roundRect(cmds, x, y, w, h, r, border);
  roundRect(cmds, x + e, y + e, w - 2 * e, h - 2 * e, r - e, fill);
}

// `y` is the top of a line `size` tall; the launcher places text by its baseline, so it is moved down to it.
void text(std::vector<mm::DrawCommand> &cmds, float x, float y, float size, uint32_t color, std::string s) {
  cmds.push_back({.type = mm::DrawCommandType::Text, .x = x, .y = y + size * 0.85f, .color = color, .size = size,
                  .text = std::move(s)});
}

// Rough width of a label; multi-byte UTF-8 characters count once.
// The launcher's font is proportional, so labels are centred with rough per-character advances (in ems,
// measured from on-screen labels).
float textWidth(const std::string &s, float font) {
  float em = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) == 0x80) continue;  // UTF-8 continuation byte
    if (std::strchr("ijl|.,:;!'", c)) em += 0.25f;
    else if (c == ' ') em += 0.25f;
    else if (std::strchr("ftrI", c)) em += 0.34f;
    else if (std::strchr("mwMW", c)) em += 0.85f;
    else if (c >= 'A' && c <= 'Z') em += 0.65f;
    else if (c >= 0x80) em += 0.4f;  // the symbols used here (degree sign, arrows) are narrow
    else em += 0.53f;
  }
  return font * em;
}

enum class Icon { List, Move, Up, Down, Left, Right, Lock };

// Icons from plain rectangles, centred in an s x s square at (x, y).
void icon(std::vector<mm::DrawCommand> &cmds, Icon which, float x, float y, float s, uint32_t color = kText,
          uint32_t hole = kButton) {
  const float cx = x + s / 2, cy = y + s / 2, t = std::max(2.0f, std::round(s * 0.09f));
  switch (which) {
  case Icon::List:
    for (int k = -1; k <= 1; ++k) rect(cmds, cx - s * 0.25f, cy + k * s * 0.2f - t / 2, s * 0.5f, t, color);
    break;
  case Icon::Move:
    rect(cmds, cx - s * 0.3f, cy - t / 2, s * 0.6f, t, color);
    rect(cmds, cx - t / 2, cy - s * 0.3f, t, s * 0.6f, color);
    for (int k = 1; k <= 2; ++k) {  // stepped arrowheads at the four tips
      const float a = s * 0.3f - k * t, b = t * (2 * k + 1);
      rect(cmds, cx - b / 2, cy - a - t / 2, b, t, color);
      rect(cmds, cx - b / 2, cy + a - t / 2, b, t, color);
      rect(cmds, cx - a - t / 2, cy - b / 2, t, b, color);
      rect(cmds, cx + a - t / 2, cy - b / 2, t, b, color);
    }
    break;
  case Icon::Lock: {  // shackle (three bars) over a body with a keyhole
    const float bw = s * 0.46f, bh = s * 0.3f, top = cy - s * 0.02f;
    rect(cmds, cx - bw * 0.35f, top - s * 0.24f, t, s * 0.24f, color);
    rect(cmds, cx + bw * 0.35f - t, top - s * 0.24f, t, s * 0.24f, color);
    rect(cmds, cx - bw * 0.35f, top - s * 0.24f, bw * 0.7f, t, color);
    rect(cmds, cx - bw / 2, top, bw, bh, color);
    rect(cmds, cx - t / 2, top + bh * 0.3f, t, bh * 0.4f, hole);
    break;
  }
  case Icon::Up:
  case Icon::Down:
    for (int k = 0; k < 4; ++k) {  // a stepped triangle, 4 rows
      const float rowY = which == Icon::Up ? cy - s * 0.12f + k * t : cy + s * 0.12f - (k + 1) * t;
      rect(cmds, cx - (k + 0.5f) * t, rowY, (2 * k + 1) * t, t, color);
    }
    break;
  case Icon::Left:
  case Icon::Right:
    for (int k = 0; k < 4; ++k) {  // the same triangle on its side
      const float colX = which == Icon::Left ? cx - s * 0.12f + k * t : cx + s * 0.12f - (k + 1) * t;
      rect(cmds, colX, cy - (k + 0.5f) * t, t, (2 * k + 1) * t, color);
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
    card(cmds, x, y, w, h, font * 0.6f, std::max(1.0f, std::round(font * 0.06f)), kPanel, kBorder);
    text(cmds, x + font * 0.8f, y + (h - font) / 2, font, kText, toast_);
  }
  const size_t toastCount = cmds.size();
  if (visible_) {
    const float font = sh * 0.032f, pad = font * 0.5f, ctrlH = font * 2.3f;
    row_ = font * 1.55f;
    headerH_ = font * 2.4f;
    w_ = font * 17;
    if (px_ < 0) px_ = sw * 0.21f, py_ = sh * 0.12f;  // right of the launcher's own buttons at the top left
    px_ = std::clamp(px_, 0.0f, sw - w_);
    py_ = std::clamp(py_, 0.0f, sh - headerH_);
    x_ = px_, y_ = py_;
    const float radius = font * 0.45f, edge = std::max(1.0f, std::round(font * 0.06f));

    auto button = [&](float bx, float by, float bw, float bh, const std::string &label, const char *key,
                      const char *value, bool active) {
      card(cmds, bx, by, bw, bh, radius, edge, active ? kActiveFill : kButton, active ? kActiveFill : kBorder);
      if (!label.empty())
        text(cmds, bx + (bw - textWidth(label, font)) / 2, by + (bh - font) / 2, font, active ? kAccent : kText, label);
      controls_.push_back({bx, by, bw, bh, key, value});
    };

    // Body first, so the header is drawn over it.
    float bodyH = 0;
    if (!collapsed_) {
      const float top = y_ + headerH_ + pad, half = (w_ - pad * 3) / 2;
      float yy = top;
      if (tab_ == Tab::Move) {
        // D-pad moves along the player's view (up = forward, left = their left); the column on the right moves along Y.
        const float c = ctrlH, gap = pad * 0.5f, dx = x_ + (w_ - (c * 4 + gap * 2 + pad * 2)) / 2;
        auto arrow = [&](float bx, float by, Icon which, const char *key) {
          button(bx, by, c, c, "", key, "", false);
          icon(cmds, which, bx, by, c);
        };
        arrow(dx + c + gap, yy, Icon::Up, "forward");
        arrow(dx, yy + c + gap, Icon::Left, "left");
        arrow(dx + (c + gap) * 2, yy + c + gap, Icon::Right, "right");
        arrow(dx + c + gap, yy + (c + gap) * 2, Icon::Down, "back");
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
        yy += ctrlH + pad * 2;
      } else if (!subFor_.empty()) {
        // Substitute view: what is replaced, the search box, matching blocks, then Held block / Undo / Back.
        size_t count = 0;
        bool substituted = false;
        for (const auto &m : materials_)
          if (m.name == subFor_) count = m.count, substituted = !m.was.empty();
        text(cmds, x_ + pad * 2, yy + (row_ - font) / 2, font, kDim, "Replace " + subFor_ + "  (" + countText(count) + ")");
        yy += row_ + pad;
        const float boxX = x_ + pad, boxW = w_ - pad * 2;
        card(cmds, boxX, yy, boxW, ctrlH, radius, edge, kWell, typing_ ? kAccent : kBorder);
        controls_.push_back({boxX, yy, boxW, ctrlH, "_search", ""});
        const std::string shown = search_.empty() && !typing_ ? "Search blocks..." : search_ + (typing_ ? "|" : "");
        text(cmds, boxX + font * 0.6f, yy + (ctrlH - font) / 2, font, search_.empty() && !typing_ ? kDim : kText, shown);
        yy += ctrlH + pad;
        listTop_ = yy;
        listBottom_ = listTop_ + row_ * 5;
        const float wellX = x_ + pad, wellW = w_ - pad * 2;
        roundRect(cmds, wellX, listTop_, wellW, listBottom_ - listTop_, radius, kWell);
        if (results_.empty()) {
          text(cmds, wellX + font * 0.5f, listTop_ + (row_ - font) / 2, font, kDim,
               search_.empty() ? "Type a block name" : "No block matches");
        }
        const float contentH = row_ * results_.size();
        const float maxScroll = std::max(0.0f, contentH - (listBottom_ - listTop_));
        scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
        for (size_t k = static_cast<size_t>(scroll_ / row_); k < results_.size(); ++k) {
          const float ry = listTop_ + k * row_ - scroll_;
          if (ry + row_ > listBottom_) break;
          if (k % 2) rect(cmds, wellX, ry, wellW, row_, kStripe);
          text(cmds, wellX + font * 0.5f, ry + (row_ - font) / 2, font, kText, results_[k]);
          controls_.push_back({wellX, ry, wellW, row_, "_pick", results_[k].c_str()});
        }
        yy = listBottom_ + pad;
        const int n = substituted ? 3 : 2;
        const float bw = (w_ - pad * (n + 1)) / n;
        float bx = x_ + pad;
        button(bx, yy, bw, ctrlH, "Held block", "_held", "", false);
        bx += bw + pad;
        if (substituted) {
          button(bx, yy, bw, ctrlH, "Undo", "_undo", "", false);
          bx += bw + pad;
        }
        button(bx, yy, bw, ctrlH, "Back", "_back", "", false);
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
        roundRect(cmds, wellX, listTop_, wellW, listBottom_ - listTop_, radius, kWell);

        const float contentH = row_ * materials_.size();
        const float maxScroll = std::max(0.0f, contentH - (listBottom_ - listTop_));
        scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
        const float dotsW = row_ * 1.4f;  // the row's ⋯ button, at its right end
        for (size_t k = static_cast<size_t>(scroll_ / row_); k < materials_.size(); ++k) {
          const float ry = listTop_ + k * row_ - scroll_;
          if (ry + row_ > listBottom_) break;
          const bool done = done_.count(materials_[k].name) > 0;
          if (k % 2) rect(cmds, wellX, ry, wellW, row_, kStripe);
          const uint32_t color = done ? kAccent : kText;
          // A substituted block is marked with *; its ⋯ view offers Undo.
          text(cmds, wellX + font * 0.5f, ry + (row_ - font) / 2, font, color,
               (done ? "\xE2\x9C\x93 " : "") + materials_[k].name + (materials_[k].was.empty() ? "" : " *"));
          text(cmds, x_ + w_ * 0.58f, ry + (row_ - font) / 2, font, done ? kAccent : kDim,
               countText(materials_[k].count));
          const float dx = wellX + wellW - dotsW - font * 0.5f;
          for (int d = -1; d <= 1; ++d)
            rect(cmds, dx + dotsW / 2 + d * font * 0.32f - font * 0.07f, ry + row_ / 2 - font * 0.07f, font * 0.14f,
                 font * 0.14f, kDim);
          controls_.push_back({dx, ry, dotsW + font * 0.5f, row_, "_sub", materials_[k].name.c_str()});
        }
        if (maxScroll > 0) {
          const float trackH = listBottom_ - listTop_, barH = std::max(row_, trackH * trackH / contentH);
          roundRect(cmds, wellX + wellW - font * 0.45f, listTop_ + (trackH - barH) * (scroll_ / maxScroll), font * 0.3f,
                    barH, font * 0.15f, kBorder);
        }
        yy = listBottom_ + pad;
      }
      bodyH = yy - (y_ + headerH_);
    }
    h_ = headerH_ + bodyH;
    // The background's height is only known now, so it goes in under the body, after the toast.
    std::vector<mm::DrawCommand> bg;
    card(bg, x_, y_, w_, h_, font * 0.8f, edge, kPanel, kBorder);
    cmds.insert(cmds.begin() + static_cast<std::ptrdiff_t>(toastCount), bg.begin(), bg.end());

    // Header: progress, then lock, Blocks / Move tabs and collapse at the right, over a divider.
    if (!collapsed_) rect(cmds, x_ + edge, y_ + headerH_, w_ - 2 * edge, edge, kBorder);
    const std::string progress =
        total_ == 0 ? "--" : std::to_string(static_cast<int>(built_ * 100 / total_)) + "% built";
    const float dot = std::max(2.0f, std::round(font * 0.14f));  // 2x3 grip dots: the header drags the panel
    for (int c = 0; c < 2; ++c)
      for (int r = -1; r <= 1; ++r)
        rect(cmds, x_ + pad * 1.5f + c * dot * 2.5f, y_ + headerH_ / 2 + r * dot * 2.5f - dot / 2, dot, dot, kDim);
    text(cmds, x_ + pad * 2.5f + dot * 4, y_ + (headerH_ - font) / 2, font, total_ ? kAccent : kDim, progress);
    const float s = headerH_ - pad * 2;
    float bx = x_ + w_ - pad - s;
    button(bx, y_ + pad, s, s, "", "_collapse", "", false);
    icon(cmds, collapsed_ ? Icon::Down : Icon::Up, bx, y_ + pad, s, kDim);
    bx -= s + pad;
    button(bx, y_ + pad, s, s, "", "_tab", "move", !collapsed_ && tab_ == Tab::Move);
    icon(cmds, Icon::Move, bx, y_ + pad, s, !collapsed_ && tab_ == Tab::Move ? kAccent : kDim);
    bx -= s + pad;
    button(bx, y_ + pad, s, s, "", "_tab", "blocks", !collapsed_ && tab_ == Tab::Blocks);
    icon(cmds, Icon::List, bx, y_ + pad, s, !collapsed_ && tab_ == Tab::Blocks ? kAccent : kDim);
    bx -= s + pad;
    button(bx, y_ + pad, s, s, "", "lock", locked_ ? "0" : "1", locked_);
    icon(cmds, Icon::Lock, bx, y_ + pad, s, locked_ ? kAccent : kDim, locked_ ? kActiveFill : kButton);
  }
  mm::submitDrawCommands(moduleId_, cmds);
}

}  // namespace bematica
