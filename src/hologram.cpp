#include "hologram.hpp"

#include "game.hpp"
#include "mesh.hpp"

#include <pl/Logger.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#include <fmt/format.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <android/log.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace bematica::hologram {
namespace {

using namespace game;

constexpr size_t kMaxQuads = 200000;
constexpr size_t kQuadBatch = 512;   // vertices per Tessellator batch stay well under its limits
constexpr size_t kLineBatch = 1024;

std::atomic<RenderLevelFn> gRenderLevelOrig{nullptr};
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
View gView;
Vec3f gCamera{};
bool gHaveCamera = false;
std::atomic_bool gPlaceAtPlayer{false};
std::atomic_bool gVisible{true};
std::atomic<float> gAlpha{0.45f};

bool finite(const Vec3f &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void rebuildLocked() {
  gMesh = gStructure ? std::make_shared<const HologramMesh>(buildMesh(*gStructure, gOrigin, gView, kMaxQuads)) : nullptr;
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

// Copies 4 floats from a pointer that may not be valid: a bad address fails instead of crashing.
bool readColor(const float *src, float out[4]) {
  iovec local{out, 4 * sizeof(float)}, remote{const_cast<float *>(src), 4 * sizeof(float)};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(4 * sizeof(float));
}

bool looksLikeColor(const float *c) {
  float v[4];
  if (!readColor(c, v)) return false;
  for (float x : v)
    if (!(x >= 0.0f && x <= 1.0f)) return false;
  return true;
}

// One line on the first drawn frame, to check this version's offsets against where the player stands.
void logFirstFrame(void *screenContext, char *lrp, const Vec3f &cam, void *faceMat, void *lineMat) {
  static std::atomic_bool logged{false};
  if (logged.exchange(true)) return;
  auto tint = [&](unsigned offset) {
    float c[4];
    if (!readColor(at<float *>(screenContext, offset), c)) return std::string("unreadable");
    return fmt::format("{:.2f} {:.2f} {:.2f} {:.2f}", c[0], c[1], c[2], c[3]);
  };
  __android_log_print(ANDROID_LOG_INFO, "BE-Matica",
                      "Hologram first frame: camera %.2f %.2f %.2f (vec@0x614 %.2f %.2f %.2f), face material %p %p, "
                      "line material %p %p, tint %s",
                      cam.x, cam.y, cam.z, at<float>(lrp, 0x614), at<float>(lrp, 0x618), at<float>(lrp, 0x61c),
                      at<void *>(faceMat, 0), at<void *>(faceMat, 8), at<void *>(lineMat, 0), at<void *>(lineMat, 8),
                      tint(0x30).c_str());
}

void renderLevelDetour(void *self, void *screenContext, void *frameRenderObject) {
  // The game can enter the detour before hook() has stored the original on the install thread.
  RenderLevelFn orig;
  while (!(orig = gRenderLevelOrig.load(std::memory_order_acquire))) std::this_thread::yield();
  orig(self, screenContext, frameRenderObject);
  if (!gVisible.load(std::memory_order_relaxed) || !self || !screenContext) return;

  void *tess = at<void *>(screenContext, off::ScreenContext_tessellator);
  char *lrp = at<char *>(self, off::LevelRenderer_levelRendererPlayer);
  if (!tess || !lrp) return;
  const Vec3f cam = at<Vec3f>(lrp, off::LevelRendererPlayer_cameraPos);
  if (!finite(cam)) return;

  std::shared_ptr<const HologramMesh> mesh;
  {
    std::lock_guard lock(gMu);
    gCamera = cam;
    gHaveCamera = true;
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
  logFirstFrame(screenContext, lrp, cam, faceMat, lineMat);
  if (!haveFace && !haveLine) return;

  // Leave the game's tint and tessellator offset exactly as we found them. The tint offset is unconfirmed
  // for this game version, so it is only touched when what it points at looks like a colour.
  float *tintColor = at<float *>(screenContext, off::ScreenContext_colorHolder);
  if (tintColor && !looksLikeColor(tintColor)) tintColor = nullptr;
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

namespace {
bool install(pl::log::Logger &log, std::string &error) {
  struct Want {
    const char *name, *sig;
    void **out;
  };
  void *renderLevel = nullptr;
  void *origSlot = nullptr;
  const Want wants[] = {
      {"RenderLevel", sig::RenderLevel, &renderLevel},
      {"TessellatorBegin", sig::TessellatorBegin, reinterpret_cast<void **>(&gBegin)},
      {"TessellatorColor", sig::TessellatorColor, reinterpret_cast<void **>(&gColor)},
      {"TessellatorVertex", sig::TessellatorVertex, reinterpret_cast<void **>(&gVertex)},
      {"RenderMeshImmediately", sig::RenderMeshImmediately, reinterpret_cast<void **>(&gRenderMesh)},
  };
  // One pass over the game for all signatures.
  std::vector<std::string> sigs;
  for (const auto &w : wants) sigs.emplace_back(w.sig);
  sigs.emplace_back(sig::RenderLevelTail);
  auto found = pl::memory::resolveSignatures(sigs, "libminecraftpe.so");
  if (!found[sig::RenderLevel] && found[sig::RenderLevelTail]) {
    found[sig::RenderLevel] = found[sig::RenderLevelTail] - sig::RenderLevelTailOffset;
    log.info("BE-Matica: RenderLevel was already hooked by another mod, hooking on top of it");
  }
  for (const auto &w : wants) {
    const auto it = found.find(w.sig);
    const uintptr_t addr = it == found.end() ? 0 : it->second;
    if (!addr) {
      error = std::string("game function not found: ") + w.name;
      return false;
    }
    *w.out = reinterpret_cast<void *>(addr);
    log.info("BE-Matica: {} at {:#x}", w.name, addr);
  }
  if (pl::memory::hook(renderLevel, reinterpret_cast<void *>(&renderLevelDetour), &origSlot) != 0 || !origSlot) {
    error = "could not hook world rendering";
    return false;
  }
  gRenderLevelOrig.store(reinterpret_cast<RenderLevelFn>(origSlot), std::memory_order_release);
  gRenderLevelTarget = renderLevel;
  return true;
}
}  // namespace

void installAsync(pl::log::Logger &log, std::function<void(bool, const std::string &)> done) {
  std::thread([&log, done = std::move(done)] {
    std::string error;
    const bool ok = install(log, error);
    done(ok, error);
  }).detach();
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

Snapshot snapshot() {
  std::lock_guard lock(gMu);
  if (!gVisible) return {};
  return {gStructure, gOrigin, gView, gCamera, gHaveCamera};
}

void nudge(int dx, int dy, int dz) {
  std::lock_guard lock(gMu);
  gOrigin[0] += dx, gOrigin[1] += dy, gOrigin[2] += dz;
  rebuildLocked();
}

void setView(const View &view) {
  std::lock_guard lock(gMu);
  gView = view;
  rebuildLocked();
}
void setVisible(bool visible) { gVisible = visible; }
void setOpacity(float alpha) { gAlpha = std::clamp(alpha, 0.05f, 1.0f); }

}  // namespace bematica::hologram
