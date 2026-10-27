#pragma once

#include "library.hpp"

#include <pl/Input.hpp>

#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace bematica {

// On-screen material list: a launcher HUD button opens a large list that scrolls by dragging,
// and tapping a row ticks it off.
class MaterialsPanel {
public:
  void init(std::string moduleId, std::string modId);
  void shutdown();

  void setContent(std::string title, std::vector<Material> materials);
  void setVisible(bool visible);  // whether the button is offered at all
  void toast(std::string text);   // short message near the top of the screen

private:
  bool onTouch(const pl::input::TouchEvent &e);
  void toggleOpen();
  void redrawLocked();
  bool insideLocked(float x, float y) const;

  std::recursive_mutex mu_;
  std::string moduleId_, modId_;
  std::string title_;
  std::vector<Material> materials_;
  std::set<std::string> done_;
  bool visible_ = false, open_ = false, buttonRegistered_ = false;

  // Layout, recomputed on every redraw from the HUD surface size.
  float x_ = 0, y_ = 0, w_ = 0, h_ = 0, row_ = 0, headerH_ = 0, scroll_ = 0;

  int activePointer_ = -1;
  float downY_ = 0, lastY_ = 0;
  bool dragged_ = false;

  std::string toast_;
  uint64_t toastId_ = 0;
};

}  // namespace bematica
