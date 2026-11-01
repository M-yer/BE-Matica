#pragma once

#include "zipfile.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bematica {

// Same order as kDir / kCorner in mesh.cpp.
enum class Face : int { Up = 0, Down, North, South, West, East };

struct Tile {
  uint16_t x = 0, y = 0, w = 0, h = 0;  // pixel rectangle in the atlas
};

// One block's entry of the resource pack's blocks.json: the terrain-texture key of every face of the unrotated model.
struct BlockModel {
  std::array<std::string, 6> faces;
  std::array<std::string, 6> carried;  // the "carried_textures" (held-item look), empty when the block has none
};

// Our own texture atlas, built at run time from the textures of the installed game (nothing from it is shipped).
struct TextureAtlas {
  std::string stamp;  // identifies the game build the atlas was made from
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;                             // width * height * 4
  std::unordered_map<std::string, std::vector<Tile>> tiles;  // terrain_texture.json key -> its variants, in order
  std::unordered_map<std::string, BlockModel> blocks;       // block name (no namespace) -> faces
};

// The vanilla resource pack inside a game apk (assets/resource_packs/vanilla/) or a plain resource_pack/ zip.
class ResourcePack {
public:
  bool open(const std::string &archivePath, std::string &error);
  bool read(const std::string &relPath, std::vector<uint8_t> &out, std::string &error) const;

private:
  ZipArchive zip_;
  std::string root_;
};

// Changes whenever the archive is replaced (game update): file name, size, modification time.
std::string archiveStamp(const std::string &archivePath);

// Reads blocks.json, terrain_texture.json and the textures they name, packs them into one atlas.
// Flipbook strips contribute their first frame. Grass, foliage and water get a fixed default tint baked in.
bool buildAtlas(const ResourcePack &pack, const std::string &stamp, TextureAtlas &out, std::string &error);

bool saveAtlas(const TextureAtlas &atlas, const std::string &path);
// False when the file is missing, damaged or was made for another `stamp`.
bool loadAtlas(const std::string &path, const std::string &stamp, TextureAtlas &out);

}  // namespace bematica
