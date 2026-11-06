#pragma once

#include "image.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Pure parts of the Bedrock-style panel skin: the pixel font and 9-slice rendering. No game or launcher types, so
// tests/skin_test.cpp can run them on the host.
namespace bematica::skin {

// A 16x16 sheet of glyph cells (code = row * 16 + column), glyphs white on transparent.
struct Font {
  int cellW = 0, cellH = 0;
  std::array<int, 256> width{};  // visible pixels from the cell's left edge to its last non-transparent column; 0 = blank
  Image sheet;
};

bool buildFont(Image sheet, Font &out);

// Advance in font pixels: visible width plus one pixel of spacing; a space is half a cell.
int advance(const Font &f, uint8_t code);

// Glyph codes to draw for a UTF-8 string. Characters the sheet cannot show are replaced: "…" by three dots, "✓" by '+',
// "°" by the sheet's degree sign (0xF8, as in the Java and Bedrock ascii sheets); blank or unknown ones by '?'.
std::vector<uint8_t> glyphCodes(const Font &f, std::string_view utf8);

int textWidth(const Font &f, std::span<const uint8_t> codes);

// One glyph at `scale` (nearest neighbour), in 0xAARRGGBB `argb`, with the Minecraft drop shadow (one font pixel down and
// right, colour * 0.25) when `shadow`. Empty (w == 0) for a blank glyph.
Image renderGlyph(const Font &f, uint8_t code, uint32_t argb, int scale, bool shadow);

// Border widths in texture pixels.
struct NineSlice {
  int l = 0, t = 0, r = 0, b = 0;
};

// Reads "nineslice_size" (a number or [l, t, r, b]) and "base_size" from a texture's .json. Borders are given in base
// pixels; a texture drawn larger than its base size has them scaled up.
bool parseNineSlice(std::string_view json, int imgW, int imgH, NineSlice &out);

// Source pixel (along one axis) for destination pixel `d` of `D`: borders are `scale` times larger than in the texture,
// the middle is stretched.
int sliceSource(int d, int D, int before, int after, int size, int scale);

Image renderNineSlice(const Image &tex, const NineSlice &ns, int w, int h, int scale);

}  // namespace bematica::skin
