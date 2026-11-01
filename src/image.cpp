#include "image.hpp"

#include <cstdlib>

// stb_image is public domain and already part of the build (lunasvg's plutovg bundles it). Only PNG and TGA are needed.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "plutovg-stb-image.h"
#pragma GCC diagnostic pop

namespace bematica {

bool decodeImage(std::span<const uint8_t> data, Image &out) {
  int w = 0, h = 0, channels = 0;
  stbi_uc *px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &channels, 4);
  if (!px) return false;
  out.w = w, out.h = h;
  out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
  stbi_image_free(px);
  return true;
}

}  // namespace bematica
