// Host-side test of the textured face renderer on a real GLES 3 context (Mesa, surfaceless EGL): shader, texture upload,
// V orientation, state restore, view-projection capture, and the whole chain block -> mesh UVs -> pixels.
//   glquads_test <vanilla resource pack zip>
#include "atlas.hpp"
#include "glquads.hpp"
#include "texresolve.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace bematica;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

constexpr int kSize = 64;

bool makeContext() {
  auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  EGLDisplay d = getDisplay ? getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major, minor;
  if (!eglInitialize(d, &major, &minor)) return false;
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!eglChooseConfig(d, cfgAttr, &cfg, 1, &n) || n < 1) return false;
  const EGLint pb[] = {EGL_WIDTH, kSize, EGL_HEIGHT, kSize, EGL_NONE};
  EGLSurface s = eglCreatePbufferSurface(d, cfg, pb);
  const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
  return s != EGL_NO_SURFACE && c != EGL_NO_CONTEXT && eglMakeCurrent(d, s, s, c);
}

struct Target {
  GLuint fbo = 0, color = 0, depth = 0;
  Target() {
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kSize, kSize);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kSize, kSize);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    glViewport(0, 0, kSize, kSize);
  }
  void clear() {
    glClearColor(0, 0, 0, 1);
    glClearDepthf(1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }
  // Colour of framebuffer pixel (x, y from the top).
  void pixel(int x, int y, uint8_t out[4]) const { glReadPixels(x, kSize - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out); }
};

const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

HologramMesh screenQuad(bool textured, float u0, float v0, float u1, float v1) {
  HologramMesh m;
  m.textured = true;
  static uint64_t next = 1000;
  m.generation = next++;
  Quad q{};
  q.v[0] = {-1, -1, 0.5f}, q.v[1] = {1, -1, 0.5f}, q.v[2] = {1, 1, 0.5f}, q.v[3] = {-1, 1, 0.5f};
  q.r = 0.2f, q.g = 0.4f, q.b = 0.8f;
  m.quads.push_back(q);
  QuadUv uv{};
  uv.shade = 1.0f;
  const float us[4] = {u0, u1, u1, u0}, vs[4] = {v1, v1, v0, v0};  // bottom corners take v1: v0 is the picture's top
  for (int k = 0; k < 4; ++k) uv.uv[k][0] = textured ? us[k] : -1.0f, uv.uv[k][1] = vs[k];
  m.uvs.push_back(uv);
  return m;
}

bool near(int a, int b, int tol = 2) { return std::abs(a - b) <= tol; }

void testPrimitives() {
  // 2x2 atlas: top row red, green; bottom row blue, white.
  TextureAtlas atlas;
  atlas.width = atlas.height = 2;
  atlas.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
  Target t;
  CHECK(glquads::uploadAtlas(atlas) && glquads::ready());

  t.clear();
  glDisable(GL_BLEND);
  CHECK(glquads::draw(screenQuad(true, 0, 0, 1, 1), {0, 0, 0}, {0, 0, 0}, kIdentity, 1.0f));
  uint8_t p[4];
  t.pixel(8, 8, p);  // top left of the screen shows the picture's top left: red
  CHECK(p[0] > 240 && p[1] < 15 && p[2] < 15);
  t.pixel(56, 8, p);
  CHECK(p[1] > 240 && p[0] < 15 && p[2] < 15);
  t.pixel(8, 56, p);
  CHECK(p[2] > 240 && p[0] < 15 && p[1] < 15);
  t.pixel(56, 56, p);
  CHECK(p[0] > 240 && p[1] > 240 && p[2] > 240);

  // Half opacity over black, a tinted face without texture, shade.
  t.clear();
  CHECK(glquads::draw(screenQuad(true, 0, 0, 0.5f, 0.5f), {0, 0, 0}, {0, 0, 0}, kIdentity, 0.5f));
  t.pixel(32, 32, p);
  CHECK(near(p[0], 128, 3) && p[1] < 3 && p[2] < 3);
  t.clear();
  HologramMesh tint = screenQuad(false, 0, 0, 0, 0);
  tint.uvs[0].shade = 0.5f;
  CHECK(glquads::draw(tint, {0, 0, 0}, {0, 0, 0}, kIdentity, 1.0f));
  t.pixel(32, 32, p);
  CHECK(near(p[0], 26, 2) && near(p[1], 51, 2) && near(p[2], 102, 2));
  // offset moves the geometry by whole screen halves: base + offset = -1 in x puts the quad off to the left
  t.clear();
  CHECK(glquads::draw(screenQuad(false, 0, 0, 0, 0), {0, 0, 0}, {2.0f, 0, 0}, kIdentity, 1.0f));
  t.pixel(32, 32, p);
  CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0);

  // Mesh without UVs or empty is refused.
  HologramMesh bad = screenQuad(true, 0, 0, 1, 1);
  bad.uvs.clear();
  CHECK(!glquads::draw(bad, {0, 0, 0}, {0, 0, 0}, kIdentity, 1.0f));
}

void testStateRestored() {
  Target t;
  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GREATER);
  glDepthMask(GL_TRUE);
  glEnable(GL_CULL_FACE);
  glBlendFunc(GL_ONE, GL_ZERO);
  glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
  glActiveTexture(GL_TEXTURE3);
  GLuint other;
  glGenTextures(1, &other);
  glBindTexture(GL_TEXTURE_2D, other);
  glPolygonOffset(3.0f, 4.0f);
  GLuint buf;
  glGenBuffers(1, &buf);
  glBindBuffer(GL_ARRAY_BUFFER, buf);
  glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_TRUE);
  glUseProgram(0);

  CHECK(glquads::draw(screenQuad(true, 0, 0, 1, 1), {0, 0, 0}, {0, 0, 0}, kIdentity, 1.0f));

  GLint v[4];
  CHECK(glIsEnabled(GL_BLEND) == GL_FALSE && glIsEnabled(GL_DEPTH_TEST) == GL_TRUE && glIsEnabled(GL_CULL_FACE) == GL_TRUE);
  CHECK(glIsEnabled(GL_POLYGON_OFFSET_FILL) == GL_FALSE && glIsEnabled(GL_STENCIL_TEST) == GL_FALSE);
  glGetIntegerv(GL_DEPTH_FUNC, v);
  CHECK(v[0] == GL_GREATER);
  GLboolean mask, cm[4];
  glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
  CHECK(mask == GL_TRUE);
  glGetBooleanv(GL_COLOR_WRITEMASK, cm);
  CHECK(cm[0] && !cm[1] && cm[2] && cm[3]);
  glGetIntegerv(GL_BLEND_SRC_RGB, v);
  CHECK(v[0] == GL_ONE);
  glGetIntegerv(GL_BLEND_DST_RGB, v);
  CHECK(v[0] == GL_ZERO);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, v);
  CHECK(v[0] == GL_FUNC_REVERSE_SUBTRACT);
  glGetIntegerv(GL_ACTIVE_TEXTURE, v);
  CHECK(v[0] == GL_TEXTURE3);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, v);
  CHECK(static_cast<GLuint>(v[0]) == other);
  GLfloat f[2];
  glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &f[0]);
  glGetFloatv(GL_POLYGON_OFFSET_UNITS, &f[1]);
  CHECK(f[0] == 3.0f && f[1] == 4.0f);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, v);
  CHECK(static_cast<GLuint>(v[0]) == buf);
  glGetIntegerv(GL_CURRENT_PROGRAM, v);
  CHECK(v[0] == 0);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, v);
  CHECK(v[0] == 0);
  CHECK(glGetError() == GL_NO_ERROR);

  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_CULL_FACE);
  glDepthFunc(GL_LESS);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ZERO);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void testViewProjectionCapture() {
  const char *vs = "#version 300 es\nuniform mat4 WORLDVIEWPROJ;\nin vec3 p;\nvoid main(){gl_Position=WORLDVIEWPROJ*vec4(p,1.0);}\n";
  const char *vsOther = "#version 300 es\nuniform mat4 other;\nin vec3 p;\nvoid main(){gl_Position=other*vec4(p,1.0);}\n";
  const char *fs = "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main(){o=vec4(1.0);}\n";
  auto program = [&](const char *v) {
    GLuint p = glCreateProgram();
    for (auto [type, src] : {std::pair<GLenum, const char *>{GL_VERTEX_SHADER, v}, {GL_FRAGMENT_SHADER, fs}}) {
      GLuint s = glCreateShader(type);
      glShaderSource(s, 1, &src, nullptr);
      glCompileShader(s);
      glAttachShader(p, s);
    }
    glLinkProgram(p);
    return p;
  };
  const GLuint good = program(vs), other = program(vsOther);
  float m[16];
  for (int i = 0; i < 16; ++i) m[i] = static_cast<float>(i + 1) * 0.5f;
  glUseProgram(good);
  glUniformMatrix4fv(glGetUniformLocation(good, "WORLDVIEWPROJ"), 1, GL_FALSE, m);
  float got[16] = {};
  CHECK(glquads::captureViewProjection(got) && std::memcmp(got, m, sizeof(m)) == 0);
  glUseProgram(other);
  glUniformMatrix4fv(glGetUniformLocation(other, "other"), 1, GL_FALSE, m);
  CHECK(!glquads::captureViewProjection(got));
  glUseProgram(0);
  CHECK(!glquads::captureViewProjection(got));
}

// One block of `block` seen from outside through its `face`, drawn by the real chain, compared with the picture.
bool faceMatchesTile(const TextureAtlas &atlas, const BlockState &block, const View &view, Face wf, const FaceTexture &want) {
  Structure s;
  s.size = {1, 1, 1};
  s.palette.push_back(block);
  s.layers[0] = {0};
  s.layers[1] = {-1};
  const TextureResolver resolver(atlas);
  HologramMesh full = buildMesh(s, {0, 0, 0}, view, 100, nullptr, &resolver);
  HologramMesh one;
  one.textured = true;
  one.generation = full.generation;
  // keep the quad lying on the wanted side of the cube, whichever face of the structure it came from
  for (size_t i = 0; i < full.quads.size(); ++i) {
    const Quad &q = full.quads[i];
    float c[3] = {0, 0, 0};
    for (const Vec3f &v : q.v) c[0] += v.x / 4, c[1] += v.y / 4, c[2] += v.z / 4;
    const float n[3] = {c[0] - 0.5f, c[1] - 0.5f, c[2] - 0.5f};
    const bool on = (wf == Face::Up && n[1] > 0.4f) || (wf == Face::Down && n[1] < -0.4f) || (wf == Face::North && n[2] < -0.4f) ||
                    (wf == Face::South && n[2] > 0.4f) || (wf == Face::West && n[0] < -0.4f) || (wf == Face::East && n[0] > 0.4f);
    if (on) one.quads.push_back(q), one.uvs.push_back(full.uvs[i]);
  }
  if (one.quads.size() != 1) return false;
  one.uvs[0].shade = 1.0f;

  // Orthographic view onto that face: screen x / y follow the face's right / up directions.
  float a0, b0, ax, bx, ay, by, az, bz;
  faceCoords(wf, 0, 0, 0, a0, b0);
  faceCoords(wf, 1, 0, 0, ax, bx);
  faceCoords(wf, 0, 1, 0, ay, by);
  faceCoords(wf, 0, 0, 1, az, bz);
  const float m[16] = {2 * (ax - a0), 2 * (bx - b0), 0, 0, 2 * (ay - a0), 2 * (by - b0), 0, 0,
                       2 * (az - a0), 2 * (bz - b0), 0, 0, 2 * a0 - 1,     2 * b0 - 1,      0.5f, 1};
  Target t;
  t.clear();
  glDisable(GL_BLEND);
  if (!glquads::draw(one, {0, 0, 0}, {0, 0, 0}, m, 1.0f)) return false;
  for (int y = 0; y < want.tile.h; ++y)
    for (int x = 0; x < want.tile.w; ++x) {
      uint8_t p[4];
      t.pixel((x * kSize) / want.tile.w + kSize / want.tile.w / 2, (y * kSize) / want.tile.h + kSize / want.tile.h / 2, p);
      const uint8_t *e = &atlas.rgba[(static_cast<size_t>(want.tile.y + y) * atlas.width + want.tile.x + x) * 4];
      if (e[3] != 255) continue;  // transparent texels show the clear colour
      if (!near(p[0], e[0], 3) || !near(p[1], e[1], 3) || !near(p[2], e[2], 3)) {
        std::printf("mismatch at %d,%d: got %d %d %d want %d %d %d\n", x, y, p[0], p[1], p[2], e[0], e[1], e[2]);
        return false;
      }
    }
  return true;
}

BlockState block(const char *name, std::initializer_list<std::pair<const char *, nbt::Tag>> states = {}) {
  BlockState b;
  b.name = name;
  b.states = nbt::makeCompound();
  for (const auto &[k, v] : states) nbt::put(b.states, k, v);
  return b;
}
nbt::Tag I(int v) { return nbt::makeInt(nbt::Type::Int, v); }
nbt::Tag S(const char *v) { return nbt::makeString(v); }

void testChain(const TextureAtlas &atlas) {
  CHECK(glquads::uploadAtlas(atlas));
  auto tile = [&](const char *key, size_t variant = 0) {
    FaceTexture t;
    t.tile = atlas.tiles.at(key).at(variant);
    return t;
  };
  const View plain;
  View turned;
  turned.rotation = 1;
  View mirrored;
  mirrored.mirror = true;

  const BlockState northFurnace = block("minecraft:furnace", {{"facing_direction", I(2)}});
  CHECK(faceMatchesTile(atlas, northFurnace, plain, Face::North, tile("furnace_front_off")));
  CHECK(faceMatchesTile(atlas, northFurnace, plain, Face::East, tile("furnace_side")));
  const BlockState southFurnace = block("minecraft:furnace", {{"facing_direction", I(3)}});
  CHECK(faceMatchesTile(atlas, southFurnace, plain, Face::Up, tile("furnace_top")));  // front not turned: top not turned
  // the mesh uses the rotated view: a furnace the structure faces south shows its front to the west
  CHECK(faceMatchesTile(atlas, southFurnace, turned, Face::West, tile("furnace_front_off")));
  const BlockState eastFurnace = block("minecraft:furnace", {{"facing_direction", I(5)}});
  CHECK(faceMatchesTile(atlas, eastFurnace, mirrored, Face::West, tile("furnace_front_off")));
  CHECK(faceMatchesTile(atlas, block("minecraft:grass_block"), plain, Face::North, tile("grass_side")));
  CHECK(faceMatchesTile(atlas, block("minecraft:grass_block"), turned, Face::Up, tile("grass_top")));

  const BlockState logY = block("minecraft:oak_log", {{"pillar_axis", S("y")}});
  CHECK(faceMatchesTile(atlas, logY, plain, Face::South, tile("oak_log_side")));
  const BlockState logX = block("minecraft:oak_log", {{"pillar_axis", S("x")}});
  CHECK(faceMatchesTile(atlas, logX, turned, Face::South, tile("oak_log_top")));  // turned a quarter: the axis is now z
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::printf("usage: glquads_test <vanilla resource pack zip>\n");
    return 2;
  }
  if (!makeContext()) {
    std::printf("SKIPPED glquads_test: no GLES 3 context (surfaceless EGL)\n");
    return 0;
  }
  std::printf("GL: %s\n", glGetString(GL_RENDERER));
  testPrimitives();
  testStateRestored();
  testViewProjectionCapture();

  ResourcePack pack;
  TextureAtlas atlas;
  std::string error;
  if (!pack.open(argv[1], error) || !buildAtlas(pack, archiveStamp(argv[1]), atlas, error)) {
    std::printf("FAIL atlas: %s\n", error.c_str());
    return 1;
  }
  testChain(atlas);
  glquads::shutdown();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("glquads_test: all passed\n");
  return 0;
}
