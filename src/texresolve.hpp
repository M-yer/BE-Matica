#pragma once

#include "atlas.hpp"
#include "mcstructure.hpp"

namespace bematica {

// Where a face samples the atlas, and how the picture is turned on that face.
struct FaceTexture {
  Tile tile;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // the tile in 0..1 atlas coordinates; v grows downwards like the image rows
  int turn[2][2]{};                      // centred face coordinates -> centred texture coordinates (quarter turns / flips)

  // (a, b): point on the world face, a towards the viewer's right and b up, both 0..1 (see faceCoords).
  void uv(float a, float b, float &u, float &v) const {
    const float ca = a - 0.5f, cb = b - 0.5f;
    const float ta = turn[0][0] * ca + turn[0][1] * cb, tb = turn[1][0] * ca + turn[1][1] * cb;
    u = u0 + (ta + 0.5f) * (u1 - u0);
    v = v0 + (0.5f - tb) * (v1 - v0);
  }
};

// The world face a structure face ends up on when the hologram is mirrored along X (first) and rotated by `rotation`
// quarter turns clockwise seen from above (same convention as View in mesh.hpp).
Face worldFace(Face f, int rotation, bool mirror);

// Position of block-local point (px, py, pz) on face `f`, seen from outside: a to the right, b up (0..1).
// Faces looking up / down have north / south at the top.
void faceCoords(Face f, float px, float py, float pz, float &a, float &b);

class TextureResolver {
public:
  // preferCarried: use the held-item textures where a block has them (grass, leaves... already tinted).
  explicit TextureResolver(const TextureAtlas &atlas, bool preferCarried = false)
      : atlas_(atlas), preferCarried_(preferCarried) {}

  // Texture of face `face` (a world face) of a block that the structure holds as `block`, shown with the hologram
  // rotated / mirrored: the block's direction states are transformed first (orient.hpp), as a placed block would be.
  // False when the block or its texture is unknown; the caller then keeps the plain tint.
  bool resolve(const BlockState &block, Face face, int rotation, bool mirror, FaceTexture &out) const;

private:
  const TextureAtlas &atlas_;
  bool preferCarried_;
};

}  // namespace bematica
