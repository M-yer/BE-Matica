#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bematica {

// Read-only zip / apk. Only the central directory is kept in memory; entries are inflated on demand.
class ZipArchive {
public:
  struct Entry {
    uint64_t localOffset = 0, compressedSize = 0, size = 0;
    uint16_t method = 0;  // 0 stored, 8 deflate
  };

  ZipArchive() = default;
  ~ZipArchive();
  ZipArchive(const ZipArchive &) = delete;
  ZipArchive &operator=(const ZipArchive &) = delete;

  bool open(const std::string &path, std::string &error);
  const std::unordered_map<std::string, Entry> &entries() const { return entries_; }
  bool read(const std::string &name, std::vector<uint8_t> &out, size_t maxOut, std::string &error) const;

private:
  bool readAt(uint64_t offset, void *dst, size_t n) const;

  int fd_ = -1;
  uint64_t size_ = 0;
  std::unordered_map<std::string, Entry> entries_;
};

// Files the running process has mapped that may be the game's apk (from /proc/self/maps, plus apks next to the mapped
// libraries). Empty when the process has no such mapping.
std::vector<std::string> gameArchiveCandidates();

}  // namespace bematica
