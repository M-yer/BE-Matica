#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Reader for Bedrock's little-endian, uncompressed NBT (as used by .mcstructure files).
namespace bematica::nbt {

enum class Type : uint8_t {
  End = 0,
  Byte,
  Short,
  Int,
  Long,
  Float,
  Double,
  ByteArray,
  String,
  List,
  Compound,
  IntArray,
  LongArray,
};

struct Tag {
  Type type = Type::End;
  int64_t i = 0;       // Byte, Short, Int, Long
  double f = 0;        // Float, Double
  std::string s;       // String
  // ByteArray/IntArray/LongArray, and Lists of integer tags. Kept flat because a structure's
  // block index lists hold millions of entries.
  std::vector<int64_t> ints;
  Type listType = Type::End;
  std::vector<Tag> items;         // List of non-integer tags, or Compound values
  std::vector<std::string> keys;  // Compound keys, parallel to items

  const Tag *get(std::string_view key) const;
  const Tag *get(std::string_view key, Type t) const;
};

// Parses a complete file whose root is a named Compound. On failure returns false and sets error.
bool parse(std::span<const uint8_t> data, Tag &root, std::string &error);

}  // namespace bematica::nbt
