#pragma once

#include <cstdint>
#include <string>

namespace bematica::icons {

// Lucide icons (lucide.dev, ISC licence): only their SVG path data is embedded here.
enum class Icon { List, Move, ArrowUp, ArrowDown, ArrowLeft, ArrowRight, ChevronUp, ChevronDown, Lock, LockOpen, Ellipsis, Settings };

// The overlay image id of `icon` drawn `px` pixels square in `argb`, rendered and registered on first use.
// Empty if the SVG could not be drawn.
std::string image(Icon icon, int px, uint32_t argb);

}  // namespace bematica::icons
