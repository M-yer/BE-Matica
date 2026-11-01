#pragma once

#include "atlas.hpp"
#include "mesh.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pl::log {
class Logger;
}

// Experimental: ghost blocks drawn with the game's own block models and textures (BlockTessellator),
// instead of tinted boxes. Optional: if any piece is missing the hologram keeps drawing boxes.
namespace bematica::textured {

// Resolves the extra game functions. Called from the hologram's install thread.
void install(pl::log::Logger &log);
void uninstall();
void setEnabled(bool on);

// Where the texture atlas made from the installed game is cached (keyed by the game's apk). The atlas is prepared on a
// background thread the first time the textured view is switched on; logs "Atlas ..." lines.
void setCacheDir(const std::filesystem::path &dir);
std::shared_ptr<const TextureAtlas> atlas();  // null until prepared

// Atlas path: faces textured from our own atlas with our own GL program (glquads.hpp), no game draw functions needed.
bool atlasWanted();  // setting on and the atlas is ready: build meshes with UVs and try drawCached()
// Render thread. Reads the view-projection matrix from the game's current shader program (call right after one of the
// game's own draws); false when it cannot be found.
void snapshotViewProjections();  // before the game's draw
bool captureViewProjection(float out[16]);
// Render thread. Draws the mesh's textured faces; false when it could not (the caller keeps the tinted boxes).
bool drawAtlas(const HologramMesh &mesh, const std::array<int, 3> &origin, const Vec3f &cam, float alpha, const float viewProj[16]);
bool enabled();

// Draws the blocks near the camera relative to it. `levelRenderer` holds the block atlas and the ClientInstance.
// Returns false when it could not draw (pieces missing, texture not found), so the caller can fall back to boxes.
bool draw(void *screenContext, void *tess, const char *levelRenderer, const char *lrp, const std::vector<BlockCell> &blocks,
          const Structure &s, const Vec3f &cam);

}  // namespace bematica::textured
