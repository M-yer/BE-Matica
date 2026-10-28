#include "nbt.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

static_assert(std::endian::native == std::endian::little, "NBT code assumes a little-endian host");

namespace bematica::nbt {
namespace {

constexpr int kMaxDepth = 512;

class Reader {
public:
  Reader(std::span<const uint8_t> data, Endian endian) : data_(data), swap_(endian == Endian::Big) {}

  template <typename T> T read() {
    need(sizeof(T));
    uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, data_.data() + pos_, sizeof(T));
    if (swap_) std::reverse(bytes, bytes + sizeof(T));
    T v;
    std::memcpy(&v, bytes, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }

  std::string readString() {
    auto len = read<uint16_t>();
    need(len);
    std::string s(reinterpret_cast<const char *>(data_.data() + pos_), len);
    pos_ += len;
    return s;
  }

  // Validates a length prefix against the bytes left, so a corrupt file can't trigger a huge allocation.
  size_t readLength(size_t minElemSize) {
    auto len = read<int32_t>();
    if (len < 0) throw std::runtime_error("negative length");
    if (minElemSize && static_cast<size_t>(len) > remaining() / minElemSize) throw std::runtime_error("length past end of file");
    return static_cast<size_t>(len);
  }

  void payload(Tag &tag, Type type, int depth) {
    if (depth > kMaxDepth) throw std::runtime_error("nesting too deep");
    tag.type = type;
    switch (type) {
    case Type::Byte: tag.i = read<int8_t>(); break;
    case Type::Short: tag.i = read<int16_t>(); break;
    case Type::Int: tag.i = read<int32_t>(); break;
    case Type::Long: tag.i = read<int64_t>(); break;
    case Type::Float: tag.f = read<float>(); break;
    case Type::Double: tag.f = read<double>(); break;
    case Type::String: tag.s = readString(); break;
    case Type::ByteArray: readInts<int8_t>(tag.ints, readLength(1)); break;
    case Type::IntArray: readInts<int32_t>(tag.ints, readLength(4)); break;
    case Type::LongArray: readInts<int64_t>(tag.ints, readLength(8)); break;
    case Type::List: list(tag, depth); break;
    case Type::Compound: compound(tag, depth); break;
    default: throw std::runtime_error("unknown tag type " + std::to_string(static_cast<int>(type)));
    }
  }

  size_t remaining() const { return data_.size() - pos_; }

private:
  void need(size_t n) {
    if (n > remaining()) throw std::runtime_error("unexpected end of file");
  }

  template <typename T> void readInts(std::vector<int64_t> &out, size_t n) {
    out.reserve(n);
    for (size_t k = 0; k < n; ++k) out.push_back(read<T>());
  }

  void list(Tag &tag, int depth) {
    tag.listType = static_cast<Type>(read<uint8_t>());
    switch (tag.listType) {
    case Type::Byte: readInts<int8_t>(tag.ints, readLength(1)); return;
    case Type::Short: readInts<int16_t>(tag.ints, readLength(2)); return;
    case Type::Int: readInts<int32_t>(tag.ints, readLength(4)); return;
    case Type::Long: readInts<int64_t>(tag.ints, readLength(8)); return;
    default: break;
    }
    // Every other element type takes at least one byte, except End (only valid for empty lists).
    size_t n = readLength(tag.listType == Type::End ? 0 : 1);
    if (tag.listType == Type::End) {
      if (n != 0) throw std::runtime_error("non-empty list of End tags");
      return;
    }
    tag.items.resize(n);
    for (auto &item : tag.items) payload(item, tag.listType, depth + 1);
  }

  void compound(Tag &tag, int depth) {
    for (;;) {
      auto type = static_cast<Type>(read<uint8_t>());
      if (type == Type::End) return;
      tag.keys.push_back(readString());
      tag.items.emplace_back();
      payload(tag.items.back(), type, depth + 1);
    }
  }

  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  bool swap_;
};

}  // namespace

const Tag *Tag::get(std::string_view key) const {
  if (type != Type::Compound) return nullptr;
  for (size_t k = 0; k < keys.size(); ++k)
    if (keys[k] == key) return &items[k];
  return nullptr;
}

const Tag *Tag::get(std::string_view key, Type t) const {
  const Tag *tag = get(key);
  return tag && tag->type == t ? tag : nullptr;
}

bool parse(std::span<const uint8_t> data, Tag &root, std::string &error, Endian endian) {
  try {
    Reader r(data, endian);
    if (static_cast<Type>(r.read<uint8_t>()) != Type::Compound) throw std::runtime_error("root is not a compound");
    r.readString();
    root = Tag{};
    r.payload(root, Type::Compound, 0);
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}

void Writer::string(const std::string &s) {
  put<uint16_t>(static_cast<uint16_t>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

void Writer::ints(const std::vector<int64_t> &v, Type t) {
  for (int64_t x : v) {
    switch (t) {
    case Type::Byte: put<int8_t>(static_cast<int8_t>(x)); break;
    case Type::Short: put<int16_t>(static_cast<int16_t>(x)); break;
    case Type::Int: put<int32_t>(static_cast<int32_t>(x)); break;
    default: put<int64_t>(x); break;
    }
  }
}

void Writer::payload(const Tag &tag) {
  switch (tag.type) {
  case Type::Byte: put<int8_t>(static_cast<int8_t>(tag.i)); break;
  case Type::Short: put<int16_t>(static_cast<int16_t>(tag.i)); break;
  case Type::Int: put<int32_t>(static_cast<int32_t>(tag.i)); break;
  case Type::Long: put<int64_t>(tag.i); break;
  case Type::Float: put<float>(static_cast<float>(tag.f)); break;
  case Type::Double: put<double>(tag.f); break;
  case Type::String: string(tag.s); break;
  case Type::ByteArray: put<int32_t>(static_cast<int32_t>(tag.ints.size())); ints(tag.ints, Type::Byte); break;
  case Type::IntArray: put<int32_t>(static_cast<int32_t>(tag.ints.size())); ints(tag.ints, Type::Int); break;
  case Type::LongArray: put<int32_t>(static_cast<int32_t>(tag.ints.size())); ints(tag.ints, Type::Long); break;
  case Type::List:
    put<uint8_t>(static_cast<uint8_t>(tag.listType));
    if (tag.items.empty()) {
      put<int32_t>(static_cast<int32_t>(tag.ints.size()));
      ints(tag.ints, tag.listType);
    } else {
      put<int32_t>(static_cast<int32_t>(tag.items.size()));
      for (const auto &item : tag.items) payload(item);
    }
    break;
  case Type::Compound:
    for (size_t k = 0; k < tag.items.size(); ++k) {
      put<uint8_t>(static_cast<uint8_t>(tag.items[k].type));
      string(tag.keys[k]);
      payload(tag.items[k]);
    }
    put<uint8_t>(0);
    break;
  case Type::End: break;
  }
}

std::vector<uint8_t> write(const Tag &root) {
  Writer w;
  w.put<uint8_t>(static_cast<uint8_t>(Type::Compound));
  w.string("");
  w.payload(root);
  return std::move(w.out);
}

Tag makeInt(Type type, int64_t v) {
  Tag t;
  t.type = type;
  t.i = v;
  return t;
}

Tag makeString(std::string s) {
  Tag t;
  t.type = Type::String;
  t.s = std::move(s);
  return t;
}

Tag makeCompound() {
  Tag t;
  t.type = Type::Compound;
  return t;
}

void put(Tag &compound, std::string key, Tag value) {
  compound.keys.push_back(std::move(key));
  compound.items.push_back(std::move(value));
}

}  // namespace bematica::nbt
