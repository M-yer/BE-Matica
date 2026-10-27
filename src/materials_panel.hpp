#pragma once

#include "library.hpp"

#include <pl/Input.hpp>

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace bematica {

// On-screen build panel: the chest HUD button opens layer and move controls above the material
// list. The list scrolls by dragging, and tapping a row ticks it off.
class MaterialsPanel {
public:
  // `control` receives the same keys as the settings page: layer_down, layer_up, layers, up, down.
  void init(std::string moduleId, std::string modId,
            std::function<void(std::string_view key, std::string_view value)> control);
  void shutdown();

  void setContent(std::string title, std::vector<Material> materials);
  void setVisible(bool visible);  // whether the button is offered at all
  void toast(std::string text);   // short message near the top of the screen
  // `layer` counts from 0; `single` is false while every layer is shown.
  void setLayer(int layer, int layerCount, bool single);

private:
  bool onTouch(const pl::input::TouchEvent &e);
  void toggleOpen();
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
  std::vector<Control> controls_;  // tappable buttons from the last redraw
  std::string title_;
  std::vector<Material> materials_;
  std::set<std::string> done_;
  bool visible_ = false, open_ = false, buttonRegistered_ = false;

  // Layout, recomputed on every redraw from the HUD surface size.
  float x_ = 0, y_ = 0, w_ = 0, h_ = 0, row_ = 0, headerH_ = 0, listTop_ = 0, scroll_ = 0;

  int activePointer_ = -1;
  float downY_ = 0, lastY_ = 0;
  bool dragged_ = false;

  std::string toast_;
  uint64_t toastId_ = 0;
};

}  // namespace bematica
