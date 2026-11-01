#include "glquads.hpp"

#include <GLES3/gl3.h>

#include <cmath>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace bematica::glquads {
namespace {

void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
#ifdef __ANDROID__
  __android_log_vprint(ANDROID_LOG_INFO, "BE-Matica", fmt, ap);
#else
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
#endif
  va_end(ap);
}

constexpr const char *kVertexShader = R"(#version 300 es
uniform mat4 uViewProj;
uniform vec3 uOffset;
in vec3 aPos;
in vec2 aUv;
in vec4 aColor;
out vec2 vUv;
out vec4 vColor;
void main() {
  vUv = aUv;
  vColor = aColor;
  gl_Position = uViewProj * vec4(aPos + uOffset, 1.0);
}
)";

// u < 0 marks a face without a texture: it keeps its tint.
constexpr const char *kFragmentShader = R"(#version 300 es
precision mediump float;
uniform sampler2D uAtlas;
uniform float uAlpha;
in vec2 vUv;
in vec4 vColor;
out vec4 outColor;
void main() {
  vec4 t = vUv.x < 0.0 ? vec4(1.0) : texture(uAtlas, vUv);
  if (t.a < 0.05) discard;
  outColor = vec4(t.rgb * vColor.rgb, t.a * uAlpha);
}
)";

struct Vertex {
  float x, y, z, u, v;
  uint8_t r, g, b, a;
};
static_assert(sizeof(Vertex) == 24);

struct Gl {
  GLuint program = 0, vao = 0, vbo = 0, ibo = 0, atlas = 0;
  GLint uViewProj = -1, uOffset = -1, uAlpha = -1, uAtlas = -1;
  uint64_t generation = 0;
  GLsizei indexCount = 0;
  bool failed = false;
  int errors = 0;
};
Gl gl;

// Everything draw() and uploadAtlas() change, put back in the destructor.
struct SavedState {
  GLint program, vao, arrayBuffer, elementBuffer, activeTexture, texture2d, sampler, pixelUnpackBuffer;
  GLint srcRgb, dstRgb, srcAlpha, dstAlpha, eqRgb, eqAlpha, depthFunc, unpackAlignment, unpackRowLength, unpackSkipRows, unpackSkipPixels;
  GLboolean depthMask, colorMask[4];
  GLfloat offsetFactor, offsetUnits;
  GLboolean blend, depthTest, cull, offsetFill, stencil;

  SavedState() {
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pixelUnpackBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2d);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glActiveTexture(static_cast<GLenum>(activeTexture));
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqAlpha);
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpackSkipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpackSkipPixels);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &offsetFactor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &offsetUnits);
    blend = glIsEnabled(GL_BLEND), depthTest = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
    offsetFill = glIsEnabled(GL_POLYGON_OFFSET_FILL), stencil = glIsEnabled(GL_STENCIL_TEST);
    // The element buffer binding belongs to the bound vertex array.
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBuffer);
  }

  ~SavedState() {
    auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
    glUseProgram(static_cast<GLuint>(program));
    glBindVertexArray(static_cast<GLuint>(vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(elementBuffer));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(pixelUnpackBuffer));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2d));
    glBindSampler(0, static_cast<GLuint>(sampler));
    glActiveTexture(static_cast<GLenum>(activeTexture));
    glBlendFuncSeparate(static_cast<GLenum>(srcRgb), static_cast<GLenum>(dstRgb), static_cast<GLenum>(srcAlpha), static_cast<GLenum>(dstAlpha));
    glBlendEquationSeparate(static_cast<GLenum>(eqRgb), static_cast<GLenum>(eqAlpha));
    glDepthFunc(static_cast<GLenum>(depthFunc));
    glDepthMask(depthMask);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glPolygonOffset(offsetFactor, offsetUnits);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, unpackSkipRows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpackSkipPixels);
    set(GL_BLEND, blend), set(GL_DEPTH_TEST, depthTest), set(GL_CULL_FACE, cull);
    set(GL_POLYGON_OFFSET_FILL, offsetFill), set(GL_STENCIL_TEST, stencil);
  }
};

GLuint compile(GLenum type, const char *src) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512]{};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    info("BE-Matica: Atlas GL shader failed: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

bool buildProgram() {
  const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader), fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return false;
  }
  gl.program = glCreateProgram();
  glAttachShader(gl.program, vs);
  glAttachShader(gl.program, fs);
  glBindAttribLocation(gl.program, 0, "aPos");
  glBindAttribLocation(gl.program, 1, "aUv");
  glBindAttribLocation(gl.program, 2, "aColor");
  glLinkProgram(gl.program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(gl.program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512]{};
    glGetProgramInfoLog(gl.program, sizeof(log) - 1, nullptr, log);
    info("BE-Matica: Atlas GL program failed: %s", log);
    glDeleteProgram(gl.program);
    gl.program = 0;
    return false;
  }
  gl.uViewProj = glGetUniformLocation(gl.program, "uViewProj");
  gl.uOffset = glGetUniformLocation(gl.program, "uOffset");
  gl.uAlpha = glGetUniformLocation(gl.program, "uAlpha");
  gl.uAtlas = glGetUniformLocation(gl.program, "uAtlas");
  return gl.uViewProj >= 0 && gl.uOffset >= 0 && gl.uAlpha >= 0 && gl.uAtlas >= 0;
}

void fail(const char *why) {
  if (!gl.failed) info("BE-Matica: Atlas GL path off (%s)", why);
  gl.failed = true;
}

// Interleaved vertices and indices for the whole mesh, uploaded once per built mesh.
bool uploadMesh(const HologramMesh &m, const Vec3f &base) {
  std::vector<Vertex> verts;
  std::vector<uint32_t> indices;
  verts.reserve(m.quads.size() * 4);
  indices.reserve(m.quads.size() * 6);
  for (size_t i = 0; i < m.quads.size(); ++i) {
    const Quad &q = m.quads[i];
    const QuadUv &uv = m.uvs[i];
    const bool tex = uv.uv[0][0] >= 0;
    const float r = tex ? 1.0f : q.r, g = tex ? 1.0f : q.g, b = tex ? 1.0f : q.b;
    const uint32_t first = static_cast<uint32_t>(verts.size());
    for (int k = 0; k < 4; ++k)
      verts.push_back({q.v[k].x - base.x, q.v[k].y - base.y, q.v[k].z - base.z, tex ? uv.uv[k][0] : -1.0f, tex ? uv.uv[k][1] : 0.0f,
                       static_cast<uint8_t>(r * uv.shade * 255.0f), static_cast<uint8_t>(g * uv.shade * 255.0f),
                       static_cast<uint8_t>(b * uv.shade * 255.0f), 255});
    for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(first + k);
  }
  if (!gl.vao) glGenVertexArrays(1, &gl.vao);
  if (!gl.vbo) glGenBuffers(1, &gl.vbo);
  if (!gl.ibo) glGenBuffers(1, &gl.ibo);
  glBindVertexArray(gl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Vertex)), verts.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)), indices.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, x)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, u)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, r)));
  gl.indexCount = static_cast<GLsizei>(indices.size());
  gl.generation = m.generation;
  return glGetError() == GL_NO_ERROR;
}

}  // namespace

bool ready() { return gl.atlas != 0 && gl.program != 0 && !gl.failed; }

bool captureViewProjection(float out[16]) {
  GLint program = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  if (!program) return false;
  static GLint cachedProgram = 0, cachedLocation = -1;
  static bool listed = false;
  if (static_cast<GLint>(program) != cachedProgram) {
    cachedProgram = program, cachedLocation = -1;
    GLint count = 0;
    glGetProgramiv(static_cast<GLuint>(program), GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; ++i) {
      char name[128]{};
      GLsizei len = 0;
      GLint size = 0;
      GLenum type = 0;
      glGetActiveUniform(static_cast<GLuint>(program), static_cast<GLuint>(i), sizeof(name) - 1, &len, &size, &type, name);
      if (!listed) info("BE-Matica: Atlas GL program %d uniform %d: %s (type %#x, size %d)", program, i, name, type, size);
      if (type == GL_FLOAT_MAT4 && strcasestr(name, "WORLDVIEWPROJ")) cachedLocation = glGetUniformLocation(static_cast<GLuint>(program), name);
    }
    listed = true;
  }
  if (cachedLocation < 0) return false;
  glGetUniformfv(static_cast<GLuint>(program), cachedLocation, out);
  bool any = false;
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(out[i])) return false;
    any = any || out[i] != 0.0f;
  }
  return any;
}

bool uploadAtlas(const TextureAtlas &atlas) {
  if (gl.failed) return false;
  if (atlas.rgba.size() != static_cast<size_t>(atlas.width) * atlas.height * 4 || atlas.rgba.empty()) return false;
  const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
  info("BE-Matica: Atlas GL context: %s", version ? version : "unknown");
  if (!version || std::strstr(version, "OpenGL ES 3") == nullptr) {  // the shaders and sampler binding need ES 3
    fail("needs an OpenGL ES 3 context");
    return false;
  }
  const SavedState saved;
  while (glGetError() != GL_NO_ERROR) {}
  if (!gl.program && !buildProgram()) {
    fail("shader");
    return false;
  }
  if (gl.atlas) glDeleteTextures(1, &gl.atlas);
  glGenTextures(1, &gl.atlas);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl.atlas);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas.width, atlas.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.rgba.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);  // the padding around tiles is one pixel: deeper levels would mix tiles
  glGenerateMipmap(GL_TEXTURE_2D);
  const GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    char why[48];
    std::snprintf(why, sizeof(why), "texture upload error %#x", err);
    fail(why);
    return false;
  }
  info("BE-Matica: Atlas uploaded to GL texture %u (%dx%d)", gl.atlas, atlas.width, atlas.height);
  return true;
}

bool draw(const HologramMesh &mesh, const Vec3f &base, const Vec3f &offset, const float viewProj[16], float alpha) {
  if (!ready() || !mesh.textured || mesh.uvs.size() != mesh.quads.size() || mesh.quads.empty()) return false;
  const SavedState saved;
  while (glGetError() != GL_NO_ERROR) {}
  if (gl.generation != mesh.generation && !uploadMesh(mesh, base)) {
    fail("mesh upload");
    return false;
  }

  // Depth: keep the game's own test (it may be reversed); a translucent ghost never writes depth.
  const bool reversed = saved.depthFunc == GL_GREATER || saved.depthFunc == GL_GEQUAL;
  glEnable(GL_DEPTH_TEST);
  if (!saved.depthTest) glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(reversed ? 1.0f : -1.0f, reversed ? 1.0f : -1.0f);  // ghost faces win over real blocks next to them

  glUseProgram(gl.program);
  glUniformMatrix4fv(gl.uViewProj, 1, GL_FALSE, viewProj);
  glUniform3f(gl.uOffset, offset.x, offset.y, offset.z);
  glUniform1f(gl.uAlpha, alpha);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl.atlas);
  glBindSampler(0, 0);
  glUniform1i(gl.uAtlas, 0);
  glBindVertexArray(gl.vao);
  glDrawElements(GL_TRIANGLES, gl.indexCount, GL_UNSIGNED_INT, nullptr);

  const GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    info("BE-Matica: Atlas GL draw error %#x", err);
    if (++gl.errors >= 3) fail("repeated GL errors");
    return false;
  }
  return true;
}

void shutdown() {
  if (gl.vbo) glDeleteBuffers(1, &gl.vbo);
  if (gl.ibo) glDeleteBuffers(1, &gl.ibo);
  if (gl.vao) glDeleteVertexArrays(1, &gl.vao);
  if (gl.atlas) glDeleteTextures(1, &gl.atlas);
  if (gl.program) glDeleteProgram(gl.program);
  gl = Gl{};
}

}  // namespace bematica::glquads
