#pragma once

#include "library.hpp"

#include <pl/Input.hpp>

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace bematica {

// Floating control panel, on screen while the Controls module is on. The header shows build progress,
// switches between the Move and Blocks tabs, collapses the panel and drags it around. Move holds the nudge,
// rotate, mirror and layer controls plus the hologram / Easy Place toggles; Blocks holds the material
// list, which scrolls by dragging, and tapping a row ticks it off. A row's ⋯ opens the Substitute view: search
// the block list with the keyboard, take the held block, or undo.
class MaterialsPanel {
public:
  // `control` receives the same keys as the settings page: forward, back, left, right, up, down, rotate, mirror,
  // layer_down, layer_up, layers, hologram, easyplace, lock; and from the Substitute view "substitute" (value
  // "from>to"), "sub_held" (value: from) and "sub_undo" (value: the substituted block).
  void init(std::string moduleId, std::string modId,
            std::function<void(std::string_view key, std::string_view value)> control);
  void shutdown();

  void setContent(std::string title, std::vector<Material> materials);
  void setVisible(bool visible);  // whether the panel is on screen at all
  void toast(std::string text);   // short message near the top of the screen
  // `layer` counts from 0; `single` is false while every layer is shown.
  void setLayer(int layer, int layerCount, bool single);
  void setState(bool hologram, bool easyPlace, bool mirror, bool locked);
  void setProgress(size_t built, size_t total);

private:
  bool onTouch(const pl::input::TouchEvent &e);
  bool onText(const pl::input::TextInputEvent &e);
  bool onKey(const pl::input::KeyEvent &e);
  void searchLocked();
  void closeSubLocked();
  void redrawLocked();
  bool insideLocked(float x, float y) const;

  struct Control {
    float x, y, w, h;
    const char *key, *value;
  };

  std::recursive_mutex mu_;
  std::string moduleId_, modId_;
  std::function<void(std::string_view, std::string_view)> control_;
  int layer_ = 0, layerCount_ = 0;
  bool single_ = false;
  bool hologram_ = false, easyPlace_ = false, mirror_ = false, locked_ = false;
  size_t built_ = 0, total_ = 0;
  enum class Tab { Move, Blocks } tab_ = Tab::Move;
  bool collapsed_ = false;
  std::vector<Control> controls_;  // tappable buttons from the last redraw
  std::string title_;
  std::vector<Material> materials_;
  std::set<std::string> done_;
  std::string subFor_, search_;       // the block being substituted (empty: list view) and the search text
  bool typing_ = false;               // the search box has the keyboard
  std::vector<std::string> results_;  // block names matching search_
  bool visible_ = false;

  // Layout, recomputed on every redraw from the HUD surface size. px_/py_ is where the panel was dragged
  // to (negative until the first redraw places it).
  float px_ = -1, py_ = -1;
  float x_ = 0, y_ = 0, w_ = 0, h_ = 0, row_ = 0, headerH_ = 0, listTop_ = 0, listBottom_ = 0, scroll_ = 0;

  int activePointer_ = -1;
  float downX_ = 0, downY_ = 0, lastX_ = 0, lastY_ = 0;
  bool dragged_ = false, movingPanel_ = false;

  std::string toast_;
  uint64_t toastId_ = 0;
};

}  // namespace bematica
