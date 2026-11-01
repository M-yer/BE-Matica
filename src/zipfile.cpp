#include "zipfile.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace bematica {
namespace {

uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return u16(p) | static_cast<uint32_t>(u16(p + 2)) << 16; }
uint64_t u64(const uint8_t *p) { return u32(p) | static_cast<uint64_t>(u32(p + 4)) << 32; }

}  // namespace

ZipArchive::~ZipArchive() {
  if (fd_ >= 0) close(fd_);
}

bool ZipArchive::readAt(uint64_t offset, void *dst, size_t n) const {
  auto *p = static_cast<uint8_t *>(dst);
  while (n > 0) {
    const ssize_t got = pread(fd_, p, n, static_cast<off_t>(offset));
    if (got <= 0) return false;
    p += got, offset += static_cast<uint64_t>(got), n -= static_cast<size_t>(got);
  }
  return true;
}

bool ZipArchive::open(const std::string &path, std::string &error) {
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd_ < 0) {
    error = "cannot open " + path;
    return false;
  }
  size_ = static_cast<uint64_t>(lseek(fd_, 0, SEEK_END));
  // The end-of-central-directory record sits in the last 64 KiB + 22 bytes (the archive comment is variable).
  const size_t tailLen = static_cast<size_t>(std::min<uint64_t>(size_, 0x10000 + 22));
  std::vector<uint8_t> tail(tailLen);
  if (!readAt(size_ - tailLen, tail.data(), tailLen)) {
    error = "cannot read " + path;
    return false;
  }
  size_t eocd = std::string::npos;
  for (size_t i = tailLen >= 22 ? tailLen - 22 + 1 : 0; i-- > 0;)
    if (u32(&tail[i]) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd == std::string::npos) {
    error = "not a zip file: " + path;
    return false;
  }
  uint64_t count = u16(&tail[eocd + 10]), cdSize = u32(&tail[eocd + 12]), cdOffset = u32(&tail[eocd + 16]);
  if (count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF) {  // zip64
    uint8_t loc[20], rec[56];
    const uint64_t eocdPos = size_ - tailLen + eocd;
    if (eocdPos < 20 || !readAt(eocdPos - 20, loc, 20) || u32(loc) != 0x07064b50 || !readAt(u64(loc + 8), rec, 56) ||
        u32(rec) != 0x06064b50) {
      error = "unsupported zip64 layout: " + path;
      return false;
    }
    count = u64(rec + 32), cdSize = u64(rec + 40), cdOffset = u64(rec + 48);
  }
  if (cdOffset + cdSize > size_ || cdSize > (256u << 20)) {
    error = "damaged zip directory: " + path;
    return false;
  }
  std::vector<uint8_t> cd(cdSize);
  if (!readAt(cdOffset, cd.data(), cdSize)) {
    error = "cannot read zip directory: " + path;
    return false;
  }
  entries_.reserve(count);
  size_t p = 0;
  while (p + 46 <= cd.size() && u32(&cd[p]) == 0x02014b50) {
    Entry e;
    e.method = u16(&cd[p + 10]);
    e.compressedSize = u32(&cd[p + 20]);
    e.size = u32(&cd[p + 24]);
    e.localOffset = u32(&cd[p + 42]);
    const size_t nameLen = u16(&cd[p + 28]), extraLen = u16(&cd[p + 30]), commentLen = u16(&cd[p + 32]);
    if (p + 46 + nameLen + extraLen + commentLen > cd.size()) break;
    std::string name(reinterpret_cast<const char *>(&cd[p + 46]), nameLen);
    // zip64 extra field: only the values that are 0xFFFFFFFF in the fixed header are present, in this order.
    const uint8_t *x = &cd[p + 46 + nameLen], *xEnd = x + extraLen;
    while (x + 4 <= xEnd) {
      const size_t id = u16(x), len = u16(x + 2);
      if (x + 4 + len > xEnd) break;
      if (id == 1) {
        const uint8_t *v = x + 4;
        if (e.size == 0xFFFFFFFF && v + 8 <= x + 4 + len) e.size = u64(v), v += 8;
        if (e.compressedSize == 0xFFFFFFFF && v + 8 <= x + 4 + len) e.compressedSize = u64(v), v += 8;
        if (e.localOffset == 0xFFFFFFFF && v + 8 <= x + 4 + len) e.localOffset = u64(v);
      }
      x += 4 + len;
    }
    entries_.emplace(std::move(name), e);
    p += 46 + nameLen + extraLen + commentLen;
  }
  if (entries_.empty()) {
    error = "empty zip directory: " + path;
    return false;
  }
  return true;
}

bool ZipArchive::read(const std::string &name, std::vector<uint8_t> &out, size_t maxOut, std::string &error) const {
  const auto it = entries_.find(name);
  if (it == entries_.end()) {
    error = "no such entry: " + name;
    return false;
  }
  const Entry &e = it->second;
  uint8_t head[30];
  if (e.size > maxOut || !readAt(e.localOffset, head, 30) || u32(head) != 0x04034b50) {
    error = "cannot read entry: " + name;
    return false;
  }
  const uint64_t dataAt = e.localOffset + 30 + u16(head + 26) + u16(head + 28);
  std::vector<uint8_t> raw(e.compressedSize);
  if (dataAt + e.compressedSize > size_ || !readAt(dataAt, raw.data(), raw.size())) {
    error = "truncated entry: " + name;
    return false;
  }
  if (e.method == 0) {
    out = std::move(raw);
    return true;
  }
  if (e.method != 8) {
    error = "unsupported compression in " + name;
    return false;
  }
  out.assign(e.size, 0);
  z_stream z{};
  if (inflateInit2(&z, -15) != Z_OK) {
    error = "cannot start decompression";
    return false;
  }
  z.next_in = raw.data(), z.avail_in = static_cast<uInt>(raw.size());
  z.next_out = out.data(), z.avail_out = static_cast<uInt>(out.size());
  const int rc = inflate(&z, Z_FINISH);
  const bool ok = rc == Z_STREAM_END && z.avail_out == 0;
  inflateEnd(&z);
  if (!ok) error = "damaged entry: " + name;
  return ok;
}

std::vector<std::string> gameArchiveCandidates() {
  namespace fs = std::filesystem;
  std::set<std::string> seen;
  std::vector<std::string> out;
  auto add = [&](const fs::path &p) {
    std::error_code ec;
    if (p.extension() == ".apk" && fs::is_regular_file(p, ec) && seen.insert(p.string()).second) out.push_back(p.string());
  };
  std::ifstream maps("/proc/self/maps");
  for (std::string line; std::getline(maps, line);) {
    const size_t slash = line.find('/');
    if (slash == std::string::npos) continue;
    const fs::path file(line.substr(slash));
    add(file);
    if (file.filename() == "libminecraftpe.so") {  // loose libs: the apk sits a few folders up
      fs::path dir = file.parent_path();
      for (int up = 0; up < 3 && dir.has_parent_path(); ++up, dir = dir.parent_path()) {
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(dir, ec)) add(e.path());
      }
    }
  }
  return out;
}

}  // namespace bematica
