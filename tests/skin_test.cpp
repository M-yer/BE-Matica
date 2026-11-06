// Host-side test: pixel font metrics and drop shadow, 9-slice maths, nineslice JSON.
#include "skin_core.hpp"

#include <cstdio>
#include <string>

using namespace bematica;
using namespace bematica::skin;

namespace {
int failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

void setPx(Image &im, int x, int y, uint8_t a = 255) {
  uint8_t *p = &im.rgba[(static_cast<size_t>(y) * im.w + x) * 4];
  p[0] = p[1] = p[2] = 255, p[3] = a;
}

// 16x16 cells of 8x8. 'A' is 5 wide (columns 0..4), 'i' is 1 wide with a pixel at row 0, '?' is 3 wide; the rest is blank.
Font makeFont() {
  Image sheet;
  sheet.w = sheet.h = 128;
  sheet.rgba.assign(128 * 128 * 4, 0);
  auto cell = [&](int code, int x, int y) { setPx(sheet, (code % 16) * 8 + x, (code / 16) * 8 + y); };
  for (int x = 0; x < 5; ++x) cell('A', x, 0);
  cell('i', 0, 0);
  for (int x = 0; x < 3; ++x) cell('?', x, 7);
  cell('.', 0, 6);
  Font f;
  CHECK(buildFont(std::move(sheet), f));
  return f;
}
}  // namespace

int main() {
  const Font f = makeFont();
  CHECK(f.cellW == 8 && f.cellH == 8);
  CHECK(f.width['A'] == 5 && f.width['i'] == 1 && f.width['?'] == 3 && f.width['B'] == 0);
  CHECK(advance(f, 'A') == 6);  // visible width + 1 pixel of spacing
  CHECK(advance(f, 'i') == 2);
  CHECK(advance(f, ' ') == 4);  // half a cell

  const auto codes = glyphCodes(f, "A iB");
  CHECK((codes == std::vector<uint8_t>{'A', ' ', 'i', '?'}));  // B is blank in this sheet
  CHECK(textWidth(f, codes) == 6 + 4 + 2 + 4 - 1);  // no spacing after the last glyph
  CHECK(textWidth(f, glyphCodes(f, "")) == 0);
  CHECK(glyphCodes(f, "\xE2\x80\xA6").size() == 3);                     // "…" becomes "..."
  CHECK((glyphCodes(f, "\xE2\x9C\x93") == std::vector<uint8_t>{'?'}));  // no '+' in this sheet, so '?'
  CHECK((glyphCodes(f, "\xC3\xA9") == std::vector<uint8_t>{'?'}));      // multi-byte char counts once

  // Glyph at scale 3 with shadow: 5 + 1 columns, 8 + 1 rows, each font pixel is a 3x3 block.
  const Image g = renderGlyph(f, 'A', 0xFFFF8000, 3, true);
  CHECK(g.w == 18 && g.h == 27);
  auto px = [&](int x, int y) { return &g.rgba[(static_cast<size_t>(y) * g.w + x) * 4]; };
  CHECK(px(0, 0)[0] == 255 && px(0, 0)[1] == 128 && px(0, 0)[2] == 0 && px(0, 0)[3] == 255);  // text colour
  CHECK(px(2, 2)[0] == 255 && px(2, 2)[3] == 255);                                              // still inside the first block
  CHECK(px(3, 3)[3] == 255 && px(3, 3)[0] == 63 && px(3, 3)[1] == 32 && px(3, 3)[2] == 0);       // row 1 has no text: this is the shadow of (0,0), colour * 0.25
  CHECK(px(17, 0)[3] == 0);                                                                     // right of the shadow column at row 0
  CHECK(px(15, 3)[3] == 255);                                                                   // shadow of the last column
  CHECK(renderGlyph(f, 'A', 0xFFFFFFFF, 2, false).w == 10);
  CHECK(renderGlyph(f, 'B', 0xFFFFFFFF, 2, true).w == 0);

  // Glyph alpha follows the colour's alpha.
  CHECK(renderGlyph(f, 'i', 0x80FFFFFF, 1, false).rgba[3] == 128);

  // nineslice json
  NineSlice ns;
  CHECK(parseNineSlice("{\n  \"nineslice_size\": 1,\n  \"base_size\": [\n    4,\n    4\n  ]\n}", 4, 4, ns));
  CHECK(ns.l == 1 && ns.t == 1 && ns.r == 1 && ns.b == 1);
  CHECK(parseNineSlice("{\"nineslice_size\": [8, 23, 8, 8], \"base_size\": [18, 33]}", 18, 33, ns));
  CHECK(ns.l == 8 && ns.t == 23 && ns.r == 8 && ns.b == 8);
  CHECK(parseNineSlice("{\"nineslice_size\": 1, \"base_size\": [5, 5]}", 10, 10, ns));  // texture twice its base size
  CHECK(ns.l == 2 && ns.b == 2);
  CHECK(!parseNineSlice("{\"base_size\": [4, 4]}", 4, 4, ns));
  CHECK(!parseNineSlice("{\"nineslice_size\": 2, \"base_size\": [4, 4]}", 4, 4, ns));  // borders meet in the middle

  // slice maths: 16 px source with 4 px borders drawn at scale 2 into 40 px.
  CHECK(sliceSource(0, 40, 4, 4, 16, 2) == 0 && sliceSource(7, 40, 4, 4, 16, 2) == 3);
  CHECK(sliceSource(8, 40, 4, 4, 16, 2) == 4 && sliceSource(31, 40, 4, 4, 16, 2) == 11);  // stretched middle
  CHECK(sliceSource(32, 40, 4, 4, 16, 2) == 12 && sliceSource(39, 40, 4, 4, 16, 2) == 15);
  CHECK(sliceSource(0, 6, 4, 4, 16, 2) == 0 && sliceSource(5, 6, 4, 4, 16, 2) == 15);  // too small for the borders: halves

  // 3x3 texture: corners 1, edges 2, middle 3, drawn 7x5 at scale 2.
  Image tex;
  tex.w = tex.h = 3;
  tex.rgba.assign(36, 0);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x) tex.rgba[(y * 3 + x) * 4] = (x == 1 && y == 1) ? 3 : (x == 1 || y == 1) ? 2 : 1;
  const Image out = renderNineSlice(tex, {1, 1, 1, 1}, 7, 5, 2);
  auto at = [&](int x, int y) { return out.rgba[(static_cast<size_t>(y) * out.w + x) * 4]; };
  CHECK(out.w == 7 && out.h == 5);
  CHECK(at(0, 0) == 1 && at(1, 1) == 1 && at(2, 0) == 2 && at(2, 2) == 3 && at(4, 2) == 3 && at(5, 2) == 2 && at(6, 4) == 1);
  CHECK(renderNineSlice(tex, {1, 1, 1, 1}, 0, 5, 2).w == 0);

  if (failures) return 1;
  std::printf("skin_test: ok\n");
  return 0;
}
