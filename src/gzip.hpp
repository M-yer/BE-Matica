#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bematica {

// Inflates gzip (or zlib) data. Fails rather than producing more than `maxOut` bytes.
bool gunzip(std::span<const uint8_t> in, std::vector<uint8_t> &out, size_t maxOut, std::string &error);

// Inflates in small pieces handed to `chunk`, so the whole output is never in memory at once.
bool gunzipChunks(std::span<const uint8_t> in, const std::function<void(std::span<const uint8_t>)> &chunk,
                  std::string &error);

}  // namespace bematica
