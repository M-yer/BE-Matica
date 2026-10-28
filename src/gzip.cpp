#include "gzip.hpp"

#include <zlib.h>

namespace bematica {

bool gunzip(std::span<const uint8_t> in, std::vector<uint8_t> &out, size_t maxOut, std::string &error) {
  z_stream z{};
  // 15 + 32: largest window, and detect a gzip or zlib header by itself.
  if (inflateInit2(&z, 15 + 32) != Z_OK) {
    error = "cannot start decompression";
    return false;
  }
  z.next_in = const_cast<Bytef *>(in.data());
  z.avail_in = static_cast<uInt>(in.size());
  out.clear();
  int rc = Z_OK;
  while (rc == Z_OK) {
    if (out.size() >= maxOut) {
      inflateEnd(&z);
      error = "file too large once unpacked";
      return false;
    }
    const size_t done = out.size();
    out.resize(std::min(maxOut, done + std::max<size_t>(in.size() * 4, 1 << 16)));
    z.next_out = out.data() + done;
    z.avail_out = static_cast<uInt>(out.size() - done);
    rc = inflate(&z, Z_NO_FLUSH);
    out.resize(out.size() - z.avail_out);
  }
  inflateEnd(&z);
  if (rc != Z_STREAM_END) {
    error = "file is damaged (decompression failed)";
    return false;
  }
  return true;
}

bool gunzipChunks(std::span<const uint8_t> in, const std::function<void(std::span<const uint8_t>)> &chunk,
                  std::string &error) {
  z_stream z{};
  if (inflateInit2(&z, 15 + 32) != Z_OK) {
    error = "cannot start decompression";
    return false;
  }
  z.next_in = const_cast<Bytef *>(in.data());
  z.avail_in = static_cast<uInt>(in.size());
  uint8_t buf[1 << 16];
  int rc = Z_OK;
  while (rc == Z_OK) {
    z.next_out = buf;
    z.avail_out = sizeof(buf);
    rc = inflate(&z, Z_NO_FLUSH);
    if (rc == Z_OK || rc == Z_STREAM_END) chunk({buf, sizeof(buf) - z.avail_out});
  }
  inflateEnd(&z);
  if (rc != Z_STREAM_END) {
    error = "file is damaged (decompression failed)";
    return false;
  }
  return true;
}

}  // namespace bematica
