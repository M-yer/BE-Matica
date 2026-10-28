#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// NBT as used by .mcstructure files (Bedrock: little-endian) and .litematic files (Java: big-endian,
// gzipped; inflate first).
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

enum class Endian { Little, Big };

// Parses a complete file whose root is a named Compound. On failure returns false and sets error.
bool parse(std::span<const uint8_t> data, Tag &root, std::string &error, Endian endian = Endian::Little);

// Little-endian NBT bytes, written piece by piece. Lets a caller stream data too large to copy
// into a Tag tree first.
class Writer {
public:
  template <typename T> void put(T v) {
    uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, &v, sizeof(T));
    out.insert(out.end(), bytes, bytes + sizeof(T));
  }
  void string(const std::string &s);
  void ints(const std::vector<int64_t> &v, Type t);
  void payload(const Tag &tag);

  std::vector<uint8_t> out;
};

// Little-endian bytes of `root` as a named root Compound. Integer lists are written with `listType`.
std::vector<uint8_t> write(const Tag &root);

// Builders for writing.
Tag makeInt(Type type, int64_t v);
Tag makeString(std::string s);
Tag makeCompound();
void put(Tag &compound, std::string key, Tag value);

}  // namespace bematica::nbt
