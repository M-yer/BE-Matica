#pragma once

#include <pl/ModMenu.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Minecraft look for the panel: the game's own pixel font and button / panel textures, read from its apk at run time.
// Nothing of Mojang's is shipped with the mod. Until load() has found all of it, available() is false and the panel
// keeps its flat launcher look.
namespace bematica::skin {

// Opens the game's apk and reads the font and textures. Slow (inflates a few PNGs): call off the render thread.
// Safe to call again; it does nothing once it has succeeded.
void load();
bool available();

// The integer font scale whose line height is closest to `fontPx`, and the border scale that goes with it.
int textScale(float fontPx);
int boxScale(int textScale);

float textWidth(const std::string &s, int scale);
float lineHeight(int scale);

// Text with its top-left corner at (x, y), one image per glyph (so the overlay holds a small, fixed set of images
// whatever the strings are). The drop shadow is the game's: the text colour * 0.25.
void drawText(std::vector<pl::modmenu::DrawCommand> &cmds, float x, float y, int scale, uint32_t color,
              const std::string &s, bool shadow = true);

enum class Box { Panel, Button, ButtonOn, Well };

// A 9-sliced panel or button of the game's, w x h pixels. False (draw nothing) when the skin is off or the image
// budget is used up; the caller then draws its flat shape.
bool drawBox(std::vector<pl::modmenu::DrawCommand> &cmds, Box box, float x, float y, float w, float h, int scale);

}  // namespace bematica::skin
