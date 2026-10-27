#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bematica {

// Inflates gzip (or zlib) data. Fails rather than producing more than `maxOut` bytes.
bool gunzip(std::span<const uint8_t> in, std::vector<uint8_t> &out, size_t maxOut, std::string &error);

}  // namespace bematica
