#pragma once

#include "mcstructure.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bematica {

// Java Edition block states -> Bedrock block states, from the table built into BE-Matica.
class BlockMap {
public:
  using Props = std::vector<std::pair<std::string, std::string>>;  // sorted by key

  static const BlockMap &builtIn();

  // False when the Java block has no Bedrock equivalent. Unknown property combinations fall back
  // to the closest known state of the same block, so files from older Java versions still convert.
  bool toBedrock(const std::string &javaName, const Props &props, BlockState &out) const;

  int32_t version() const { return version_; }

private:
  struct Entry {
    Props javaProps;
    BlockState bedrock;
  };
  std::unordered_map<std::string, std::vector<Entry>> byName_;
  int32_t version_ = 0;
};

}  // namespace bematica
