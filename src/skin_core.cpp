#include "skin_core.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace bematica::skin {

bool buildFont(Image sheet, Font &out) {
  if (sheet.w < 16 || sheet.h < 16 || sheet.w % 16 || sheet.h % 16 ||
      sheet.rgba.size() != static_cast<size_t>(sheet.w) * sheet.h * 4)
    return false;
  out.cellW = sheet.w / 16, out.cellH = sheet.h / 16;
  for (int code = 0; code < 256; ++code) {
    const int cx = (code % 16) * out.cellW, cy = (code / 16) * out.cellH;
    int last = -1;
    for (int y = 0; y < out.cellH; ++y)
      for (int x = out.cellW - 1; x > last; --x)
        if (sheet.rgba[((static_cast<size_t>(cy + y) * sheet.w) + cx + x) * 4 + 3]) {
          last = x;
          break;
        }
    out.width[code] = last + 1;
  }
  out.sheet = std::move(sheet);
  return out.width['A'] > 0;
}

int advance(const Font &f, uint8_t code) { return code == ' ' ? f.cellW / 2 : f.width[code] + 1; }

std::vector<uint8_t> glyphCodes(const Font &f, std::string_view s) {
  std::vector<uint8_t> out;
  auto put = [&](uint32_t cp) {
    if (cp == 0x2026) {
      out.insert(out.end(), 3, uint8_t{'.'});
      return;
    }
    if (cp == 0x2713) cp = '+';
    else if (cp == 0xB0) cp = 0xF8;
    if (cp >= 256 || (cp != ' ' && f.width[cp] == 0)) cp = '?';
    out.push_back(static_cast<uint8_t>(cp));
  };
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<uint8_t>(s[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    uint32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
    for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
    i += n;
    put(cp);
  }
  return out;
}

int textWidth(const Font &f, std::span<const uint8_t> codes) {
  int w = 0;
  for (uint8_t c : codes) w += advance(f, c);
  return w > 0 ? w - 1 : 0;  // the spacing after the last glyph is not part of the text
}

Image renderGlyph(const Font &f, uint8_t code, uint32_t argb, int scale, bool shadow) {
  Image out;
  const int gw = f.width[code];
  if (gw == 0 || scale < 1) return out;
  const int extra = shadow ? 1 : 0;
  out.w = (gw + extra) * scale, out.h = (f.cellH + extra) * scale;
  out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
  const int cx = (code % 16) * f.cellW, cy = (code / 16) * f.cellH;
  const uint32_t alpha = argb >> 24;
  const uint32_t rgb[2] = {argb & 0xFFFFFF, ((argb >> 18 & 0x3F) << 16) | ((argb >> 10 & 0x3F) << 8) | (argb >> 2 & 0x3F)};
  auto paint = [&](int gx, int gy, uint32_t color, uint32_t a) {
    for (int dy = 0; dy < scale; ++dy)
      for (int dx = 0; dx < scale; ++dx) {
        uint8_t *p = &out.rgba[((static_cast<size_t>(gy * scale + dy) * out.w) + gx * scale + dx) * 4];
        p[0] = color >> 16 & 0xFF, p[1] = color >> 8 & 0xFF, p[2] = color & 0xFF, p[3] = static_cast<uint8_t>(a);
      }
  };
  for (int pass = shadow ? 0 : 1; pass < 2; ++pass)
    for (int y = 0; y < f.cellH; ++y)
      for (int x = 0; x < gw; ++x) {
        const uint32_t a = f.sheet.rgba[((static_cast<size_t>(cy + y) * f.sheet.w) + cx + x) * 4 + 3];
        if (a) paint(x + (pass == 0 ? extra : 0), y + (pass == 0 ? extra : 0), rgb[pass == 0], a * alpha / 255);
      }
  return out;
}

namespace {

// Integers following `key` in `json`: after the colon either one number or a bracketed list.
std::vector<int> readInts(std::string_view json, std::string_view key) {
  std::vector<int> v;
  size_t at = json.find(key);
  if (at == std::string_view::npos || (at = json.find(':', at)) == std::string_view::npos) return v;
  ++at;
  while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at]))) ++at;
  const bool list = at < json.size() && json[at] == '[';
  for (; at < json.size() && json[at] != ']'; ++at) {
    if (!std::isdigit(static_cast<unsigned char>(json[at]))) continue;
    int n = 0;
    while (at < json.size() && std::isdigit(static_cast<unsigned char>(json[at]))) n = n * 10 + (json[at++] - '0');
    v.push_back(n);
    --at;
    if (!list) break;
  }
  return v;
}

}  // namespace

bool parseNineSlice(std::string_view json, int imgW, int imgH, NineSlice &out) {
  const std::vector<int> size = readInts(json, "nineslice_size"), base = readInts(json, "base_size");
  if ((size.size() != 1 && size.size() != 4) || base.size() != 2 || base[0] <= 0 || base[1] <= 0) return false;
  const int k = std::max(1, std::min(imgW / base[0], imgH / base[1]));
  out = size.size() == 1 ? NineSlice{size[0] * k, size[0] * k, size[0] * k, size[0] * k}
                         : NineSlice{size[0] * k, size[1] * k, size[2] * k, size[3] * k};
  return out.l + out.r < imgW && out.t + out.b < imgH;
}

int sliceSource(int d, int D, int before, int after, int size, int scale) {
  const int db = std::min(before * scale, D / 2), da = std::min(after * scale, D / 2);
  if (d < db) return std::min(d / scale, before - 1);
  if (d >= D - da) return size - 1 - std::min((D - 1 - d) / scale, after - 1);
  return before + (d - db) * (size - before - after) / (D - db - da);
}

Image renderNineSlice(const Image &tex, const NineSlice &ns, int w, int h, int scale) {
  Image out;
  if (w < 1 || h < 1 || scale < 1) return out;
  out.w = w, out.h = h;
  out.rgba.resize(static_cast<size_t>(w) * h * 4);
  std::vector<int> sx(w);
  for (int x = 0; x < w; ++x) sx[x] = sliceSource(x, w, ns.l, ns.r, tex.w, scale);
  for (int y = 0; y < h; ++y) {
    const int sy = sliceSource(y, h, ns.t, ns.b, tex.h, scale);
    for (int x = 0; x < w; ++x)
      std::copy_n(&tex.rgba[(static_cast<size_t>(sy) * tex.w + sx[x]) * 4], 4, &out.rgba[(static_cast<size_t>(y) * w + x) * 4]);
  }
  return out;
}

}  // namespace bematica::skin
