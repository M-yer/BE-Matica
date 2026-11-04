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

  // Reads the built-in table, keeping only the given Java blocks. The full table is tens of MB
  // once parsed, while one schematic uses a few hundred states, so nothing else is kept.
  static BlockMap forBlocks(const std::vector<std::string> &javaNames);

  // False when the Java block has no Bedrock equivalent. Unknown property combinations fall back
  // to the closest known state of the same block, so files from older Java versions still convert.
  bool toBedrock(const std::string &javaName, const Props &props, BlockState &out) const;

  int32_t version() const { return version_; }

  // One state per Bedrock block in the table (the first one listed for it), sorted by name. Read once, then kept.
  static const std::vector<BlockState> &bedrockBlocks();

private:
  struct Entry {
    Props javaProps;
    BlockState bedrock;
  };
  std::unordered_map<std::string, std::vector<Entry>> byName_;
  int32_t version_ = 0;
};

}  // namespace bematica
