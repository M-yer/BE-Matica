#pragma once

#include "mesh.hpp"

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
bool enabled();

// Draws `blocks` relative to the camera. Returns false when it could not draw (pieces missing, or no block
// texture has been seen yet), so the caller can fall back to boxes.
bool draw(void *screenContext, void *tess, const std::vector<BlockCell> &blocks, const Structure &s, const Vec3f &cam);

}  // namespace bematica::textured
