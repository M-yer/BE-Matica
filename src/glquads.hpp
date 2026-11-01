#pragma once

#include "atlas.hpp"
#include "mesh.hpp"

// Textured hologram faces drawn with our own OpenGL ES program (the game's materials cannot be given our atlas without
// game functions that are not located yet). Everything runs on the render thread with the game's GL context current and
// puts every piece of GL state it touches back, so the game's own state tracking stays valid.
namespace bematica::glquads {

// Reads the view-projection matrix of the program the game used for its last draw (its WORLDVIEWPROJ uniform), column
// major, ready for glUniformMatrix4fv. False when that program has no such uniform; the active uniforms are logged once.
// The game may leave no program bound after a draw; then call snapshotViewProjections() before the game's draw and the
// program whose matrix changed (or the one found last time) is used.
void snapshotViewProjections();
bool captureViewProjection(float out[16]);

// Uploads the atlas as a GL texture (once per atlas). False when GL refuses; the path then stays off for good.
bool uploadAtlas(const TextureAtlas &atlas);

// Draws mesh.quads with mesh.uvs. Vertices are mesh world positions minus `base`; `offset` = base - camera.
// Returns false when nothing could be drawn (not ready, GL error), so the caller keeps the tinted boxes.
bool draw(const HologramMesh &mesh, const Vec3f &base, const Vec3f &offset, const float viewProj[16], float alpha);

bool ready();
void shutdown();  // releases the GL objects (needs the GL context); the host test calls it

}  // namespace bematica::glquads
