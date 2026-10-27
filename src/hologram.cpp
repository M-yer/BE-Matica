#include "hologram.hpp"

#include "game.hpp"
#include "mesh.hpp"

#include <pl/Logger.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace bematica::hologram {
namespace {

using namespace game;

constexpr size_t kMaxQuads = 200000;
constexpr size_t kQuadBatch = 512;   // vertices per Tessellator batch stay well under its limits
constexpr size_t kLineBatch = 1024;

RenderLevelFn gRenderLevelOrig = nullptr;
void *gRenderLevelTarget = nullptr;
TessBeginFn gBegin = nullptr;
TessColorFn gColor = nullptr;
TessVertexFn gVertex = nullptr;
RenderMeshFn gRenderMesh = nullptr;

// Written by the UI thread, read by the render thread.
std::mutex gMu;
std::shared_ptr<const Structure> gStructure;
std::shared_ptr<const HologramMesh> gMesh;
std::array<int, 3> gOrigin{};
std::atomic_bool gPlaceAtPlayer{false};
std::atomic_bool gVisible{true};
std::atomic<float> gAlpha{0.45f};

bool finite(const Vec3f &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void rebuildLocked() {
  gMesh = gStructure ? std::make_shared<const HologramMesh>(buildMesh(*gStructure, gOrigin, kMaxQuads)) : nullptr;
}

void submit(void *screenContext, void *tess, void *material) {
  char scratch[0x58]{};  // the game writes per-draw state here
  gRenderMesh(screenContext, tess, material, scratch);
}

void drawQuads(void *sc, void *tess, void *mat, const HologramMesh &m, const Vec3f &cam, float alpha) {
  for (size_t from = 0; from < m.quads.size(); from += kQuadBatch) {
    const size_t to = std::min(m.quads.size(), from + kQuadBatch);
    gBegin(tess, nullptr, QuadList, static_cast<int>((to - from) * 4), 0);
    for (size_t i = from; i < to; ++i) {
      const Quad &q = m.quads[i];
      gColor(tess, q.r, q.g, q.b, alpha);
      for (const auto &v : q.v) gVertex(tess, v.x - cam.x, v.y - cam.y, v.z - cam.z);
    }
    submit(sc, tess, mat);
  }
}

void drawLines(void *sc, void *tess, void *mat, const HologramMesh &m, const Vec3f &cam) {
  for (size_t from = 0; from < m.lines.size(); from += kLineBatch) {
    const size_t to = std::min(m.lines.size(), from + kLineBatch);
    gBegin(tess, nullptr, LineList, static_cast<int>((to - from) * 2), 0);
    gColor(tess, 0.3f, 0.75f, 1.0f, 1.0f);
    for (size_t i = from; i < to; ++i) {
      const Line &l = m.lines[i];
      gVertex(tess, l.a.x - cam.x, l.a.y - cam.y, l.a.z - cam.z);
      gVertex(tess, l.b.x - cam.x, l.b.y - cam.y, l.b.z - cam.z);
    }
    submit(sc, tess, mat);
  }
}

template <typename T> T at(void *base, unsigned offset) {
  T v;
  std::memcpy(&v, static_cast<char *>(base) + offset, sizeof(T));
  return v;
}

void renderLevelDetour(void *self, void *screenContext, void *frameRenderObject) {
  gRenderLevelOrig(self, screenContext, frameRenderObject);
  if (!gVisible.load(std::memory_order_relaxed) || !self || !screenContext) return;

  void *tess = at<void *>(screenContext, off::ScreenContext_tessellator);
  char *lrp = at<char *>(self, off::LevelRenderer_levelRendererPlayer);
  if (!tess || !lrp) return;
  const Vec3f cam = at<Vec3f>(lrp, off::LevelRendererPlayer_cameraPos);
  if (!finite(cam)) return;

  std::shared_ptr<const HologramMesh> mesh;
  {
    std::lock_guard lock(gMu);
    if (gPlaceAtPlayer.exchange(false)) {
      // The camera sits at eye height (1.62 blocks above the feet).
      gOrigin = {static_cast<int>(std::floor(cam.x)), static_cast<int>(std::floor(cam.y - 1.62f)),
                 static_cast<int>(std::floor(cam.z))};
      rebuildLocked();
    }
    mesh = gMesh;
  }
  if (!mesh) return;

  // Materials are owned by the game; a MaterialPtr is "set" when its first word is non-null.
  void *faceMat = lrp + off::LevelRendererPlayer_selectionOverlayMat;
  void *lineMat = lrp + off::LevelRendererPlayer_selectionBoxMat;
  const bool haveFace = at<void *>(faceMat, 0) != nullptr, haveLine = at<void *>(lineMat, 0) != nullptr;
  if (!haveFace && !haveLine) return;

  // Leave the game's tint and tessellator offset exactly as we found them.
  float *tintColor = at<float *>(screenContext, off::ScreenContext_colorHolder);
  float savedTint[4]{};
  if (tintColor) {
    std::memcpy(savedTint, tintColor, sizeof(savedTint));
    std::fill(tintColor, tintColor + 4, 1.0f);
  }
  float *tessOffset = reinterpret_cast<float *>(static_cast<char *>(tess) + off::Tessellator_postTransformOffset);
  float savedOffset[3];
  std::memcpy(savedOffset, tessOffset, sizeof(savedOffset));

  if (haveFace) drawQuads(screenContext, tess, faceMat, *mesh, cam, gAlpha.load(std::memory_order_relaxed));
  if (haveLine) drawLines(screenContext, tess, lineMat, *mesh, cam);

  std::memcpy(tessOffset, savedOffset, sizeof(savedOffset));
  if (tintColor) std::memcpy(tintColor, savedTint, sizeof(savedTint));
}

}  // namespace

bool install(pl::log::Logger &log, std::string &error) {
  struct Want {
    const char *name, *sig;
    void **out;
  };
  void *renderLevel = nullptr;
  const Want wants[] = {
      {"RenderLevel", sig::RenderLevel, &renderLevel},
      {"TessellatorBegin", sig::TessellatorBegin, reinterpret_cast<void **>(&gBegin)},
      {"TessellatorColor", sig::TessellatorColor, reinterpret_cast<void **>(&gColor)},
      {"TessellatorVertex", sig::TessellatorVertex, reinterpret_cast<void **>(&gVertex)},
      {"RenderMeshImmediately", sig::RenderMeshImmediately, reinterpret_cast<void **>(&gRenderMesh)},
  };
  for (const auto &w : wants) {
    const uintptr_t addr = pl::memory::resolveSignature(w.sig, "libminecraftpe.so");
    if (!addr) {
      error = std::string("game function not found: ") + w.name;
      return false;
    }
    *w.out = reinterpret_cast<void *>(addr);
    log.info("BE-Matica: {} at {:#x}", w.name, addr);
  }
  if (pl::memory::hook(renderLevel, reinterpret_cast<void *>(&renderLevelDetour),
                       reinterpret_cast<void **>(&gRenderLevelOrig)) != 0 ||
      !gRenderLevelOrig) {
    error = "could not hook world rendering";
    return false;
  }
  gRenderLevelTarget = renderLevel;
  return true;
}

void uninstall() {
  if (gRenderLevelTarget) pl::memory::unhook(gRenderLevelTarget, reinterpret_cast<void *>(&renderLevelDetour));
  gRenderLevelTarget = nullptr;
}

void show(std::shared_ptr<const Structure> s, bool atPlayer) {
  std::lock_guard lock(gMu);
  gStructure = std::move(s);
  rebuildLocked();
  if (atPlayer) gPlaceAtPlayer = true;
}

void moveToPlayer() { gPlaceAtPlayer = true; }
void setVisible(bool visible) { gVisible = visible; }
void setOpacity(float alpha) { gAlpha = std::clamp(alpha, 0.05f, 1.0f); }

}  // namespace bematica::hologram
