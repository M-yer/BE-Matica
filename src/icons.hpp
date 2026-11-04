#pragma once

#include <cstdint>
#include <string>

namespace bematica::icons {

// Lucide icons (lucide.dev, ISC licence): only their SVG path data is embedded here.
enum class Icon { List, Move, ArrowUp, ArrowDown, ArrowLeft, ArrowRight, ChevronUp, ChevronDown, Lock, LockOpen, Ellipsis, Settings, Box };

// The overlay image id of `icon` drawn `px` pixels square in `argb`, rendered and registered on first use.
// Empty if the SVG could not be drawn.
std::string image(Icon icon, int px, uint32_t argb);

// `icon` in a filled circle with a thin ring, `px` pixels square, as one anti-aliased image (same caching as image()).
std::string badge(Icon icon, int px, uint32_t fill, uint32_t ring, uint32_t iconColor);

}  // namespace bematica::icons
