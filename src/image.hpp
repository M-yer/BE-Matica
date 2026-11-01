#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bematica {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // w * h * 4, row 0 on top
};

// PNG or TGA to 8-bit RGBA.
bool decodeImage(std::span<const uint8_t> data, Image &out);

}  // namespace bematica
