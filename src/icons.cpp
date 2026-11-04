#include "icons.hpp"

#include <lunasvg.h>
#include <pl/ModMenu.hpp>

#include <cstdio>
#include <mutex>
#include <set>

namespace bematica::icons {
namespace {

const char *body(Icon icon) {
  switch (icon) {
  case Icon::List:
    return R"(<path d="M3 5h.01"/><path d="M3 12h.01"/><path d="M3 19h.01"/><path d="M8 5h13"/><path d="M8 12h13"/><path d="M8 19h13"/>)";
  case Icon::Move:
    return R"(<path d="M12 2v20"/><path d="m15 19-3 3-3-3"/><path d="m19 9 3 3-3 3"/><path d="M2 12h20"/><path d="m5 9-3 3 3 3"/><path d="m9 5 3-3 3 3"/>)";
  case Icon::ArrowUp: return R"(<path d="m5 12 7-7 7 7"/><path d="M12 19V5"/>)";
  case Icon::ArrowDown: return R"(<path d="M12 5v14"/><path d="m19 12-7 7-7-7"/>)";
  case Icon::ArrowLeft: return R"(<path d="m12 19-7-7 7-7"/><path d="M19 12H5"/>)";
  case Icon::ArrowRight: return R"(<path d="M5 12h14"/><path d="m12 5 7 7-7 7"/>)";
  case Icon::ChevronUp: return R"(<path d="m18 15-6-6-6 6"/>)";
  case Icon::ChevronDown: return R"(<path d="m6 9 6 6 6-6"/>)";
  case Icon::Lock: return R"(<rect width="18" height="11" x="3" y="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/>)";
  case Icon::LockOpen: return R"(<rect width="18" height="11" x="3" y="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 9.9-1"/>)";
  case Icon::Settings:
    return R"(<path d="M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915"/><circle cx="12" cy="12" r="3"/>)";
  case Icon::Box:
    return R"(<path d="M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z"/><path d="m3.3 7 8.7 5 8.7-5"/><path d="M12 22V12"/>)";
  case Icon::Ellipsis: return R"(<circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/><circle cx="5" cy="12" r="1"/>)";
  }
  return "";
}

std::mutex gMu;
std::set<std::string> gRegistered;  // image ids already handed to the overlay

// Renders `svg` at px x px and registers it under `id`, once.
std::string render(const std::string &id, const char *svg, int px) {
  std::lock_guard lock(gMu);
  if (gRegistered.count(id)) return id;
  const auto doc = lunasvg::Document::loadFromData(svg);
  if (!doc) return {};
  auto bitmap = doc->renderToBitmap(px, px);
  if (bitmap.isNull()) return {};
  bitmap.convertToRGBA();
  // The overlay wants tightly packed rows; lunasvg's stride is 4 * width for a bitmap it allocates itself.
  const auto *data = bitmap.data();
  if (!pl::modmenu::registerImage(id, {data, static_cast<size_t>(bitmap.stride()) * bitmap.height()}, bitmap.width(),
                                  bitmap.height()))
    return {};
  gRegistered.insert(id);
  return id;
}

}  // namespace

std::string image(Icon icon, int px, uint32_t argb) {
  if (px < 4) return {};
  char id[64], svg[1024];
  std::snprintf(id, sizeof id, "bematica.icon.%d.%d.%08x", static_cast<int>(icon), px, argb);
  std::snprintf(svg, sizeof svg,
                R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#%06x" )"
                R"(stroke-opacity="%.3f" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">%s</svg>)",
                argb & 0xFFFFFF, (argb >> 24) / 255.0, body(icon));
  return render(id, svg, px);
}

std::string badge(Icon icon, int px, uint32_t fill, uint32_t ring, uint32_t iconColor) {
  if (px < 8) return {};
  char id[96], svg[1536];
  std::snprintf(id, sizeof id, "bematica.badge.%d.%d.%08x.%08x.%08x", static_cast<int>(icon), px, fill, ring, iconColor);
  // The 24-unit icon sits in the middle half of the 48-unit circle.
  std::snprintf(svg, sizeof svg,
                R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">)"
                R"(<circle cx="24" cy="24" r="23" fill="#%06x" fill-opacity="%.3f" stroke="#%06x" stroke-opacity="%.3f" stroke-width="1.5"/>)"
                R"svg(<g transform="translate(12 12)" fill="none" stroke="#%06x" stroke-opacity="%.3f" stroke-width="2" )svg"
                R"(stroke-linecap="round" stroke-linejoin="round">%s</g></svg>)",
                fill & 0xFFFFFF, (fill >> 24) / 255.0, ring & 0xFFFFFF, (ring >> 24) / 255.0, iconColor & 0xFFFFFF,
                (iconColor >> 24) / 255.0, body(icon));
  return render(id, svg, px);
}

}  // namespace bematica::icons
