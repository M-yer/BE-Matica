#include "hologram.hpp"

#include "game.hpp"
#include "mesh.hpp"
#include "textured.hpp"

#include <pl/Logger.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#include <fmt/format.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <android/log.h>
#include <cctype>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
std::array<int, 2> gForward{0, 1};  // the player's facing along X / Z, snapped to the nearer axis (guarded by gMu)
std::atomic_bool gPlaceAtPlayer{false};
pl::log::Logger *gLog = nullptr;
std::atomic_bool gVisible{true};
std::atomic<float> gAlpha{0.45f};
std::atomic_bool gOutlines{true}, gFill{true};
std::unordered_set<uint64_t> gPlaced;  // world positions already built correctly (guarded by gMu)
std::vector<uint64_t> gPending;        // positions placed since the last check (guarded by gMu)
bool gFullCheck = true;                 // check every shown block once (guarded by gMu)
size_t gTotal = 0;                      // shown structure blocks, counted by the full check (guarded by gMu)
std::function<void(size_t, size_t)> gProgress;  // guarded by gMu
std::function<void(std::array<int, 3>)> gOriginListener;  // guarded by gMu
std::array<int, 3> gReportedOrigin{};                     // last origin given to it (guarded by gMu)
bool gOriginReported = false;                             // guarded by gMu
GetBlockFn gGetBlock = nullptr;
BlockRegistryFn gRegistry = nullptr;
BlockLookupFn gLookup = nullptr;

// What the look-at card last showed (guarded by gMu). A new listener forgets it so the card refreshes.
struct LookKey {
  bool valid = false;
  uintptr_t type = 0;  // untagged BlockType under the crosshair
  std::shared_ptr<const Structure> structure;
  int32_t palette = -1;  // the structure's palette index for that cell, -1 when it wants nothing there
  bool ghost = false;    // the crosshair is on an unplaced ghost block at `cell`; `type` is then the world's block there
  std::array<int, 3> cell{};
  bool operator==(const LookKey &o) const = default;
};
LookKey gLookKey;
std::function<void(const LookInfo &)> gLookListener;  // guarded by gMu

bool finite(const Vec3f &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void rebuildLocked() {
  gMesh = gStructure ? std::make_shared<const HologramMesh>(buildMesh(*gStructure, gOrigin, gView, kMaxQuads, &gPlaced))
                     : nullptr;
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

void drawLines(void *sc, void *tess, void *mat, const std::vector<Line> &lines, const Vec3f &cam) {
  for (size_t from = 0; from < lines.size(); from += kLineBatch) {
    const size_t to = std::min(lines.size(), from + kLineBatch);
    gBegin(tess, nullptr, LineList, static_cast<int>((to - from) * 2), 0);
    gColor(tess, 0.3f, 0.75f, 1.0f, 1.0f);
    for (size_t i = from; i < to; ++i) {
      const Line &l = lines[i];
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

// Copies memory that may not be mapped: a bad address fails instead of crashing.
bool safeRead(const void *src, void *dst, size_t n) {
  iovec local{dst, n}, remote{reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(src) & 0x00FFFFFFFFFFFFFFULL), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

// Class name of a polymorphic game object from its RTTI ("11ClientLevel" -> "ClientLevel"), or "".
std::string className(const void *obj) {
  const void *vtable = nullptr, *typeInfo = nullptr, *name = nullptr;
  if (!obj || !safeRead(obj, &vtable, 8) || !vtable) return {};
  if (!safeRead(static_cast<const char *>(vtable) - 8, &typeInfo, 8) || !typeInfo) return {};
  if (!safeRead(static_cast<const char *>(typeInfo) + 8, &name, 8) || !name) return {};
  char buf[33]{};
  if (!safeRead(name, buf, 32) && !safeRead(name, buf, 16)) return {};
  std::string_view n(buf);
  while (!n.empty() && n.front() >= '0' && n.front() <= '9') n.remove_prefix(1);
  return std::string(n);
}

// The player's aim ray, from the level's current hit result, plus what it hit (type 0 is a block, at hitPos).
// The level is found in the level renderer by its class name, and the hit result getter is only called after
// checking it is the expected one-line field getter.
bool aimRay(const char *levelRenderer, Vec3f &eye, Vec3f &dir, int32_t &hitType, BlockPos &hitPos) {
  static int levelField = -2;
  if (levelField == -2) {
    levelField = -1;
    for (unsigned off = 0; off < 0x1000 && levelField < 0; off += 8) {
      const void *p = nullptr;
      if (!safeRead(levelRenderer + off, &p, 8) || !p) continue;
      const std::string name = className(p);
      if (name.size() >= 5 && name.compare(name.size() - 5, 5, "Level") == 0) levelField = static_cast<int>(off);
    }
  }
  if (levelField < 0) return false;
  void *level = nullptr;
  void **vtable = nullptr;
  void *getter = nullptr;
  if (!safeRead(levelRenderer + levelField, &level, 8) || !level || !safeRead(level, &vtable, 8) || !vtable ||
      !safeRead(vtable + off::Level_vtGetHitResult / sizeof(void *), &getter, 8) || !getter)
    return false;
  uint32_t code[2]{};
  if (!safeRead(getter, code, sizeof(code)) || (code[0] & 0xFFC003FF) != 0xF9400000 || code[1] != 0xD65F03C0) return false;
  const char *hr = static_cast<const char *>(reinterpret_cast<GetHitResultFn>(getter)(level));
  Vec3f d{};
  if (!hr || !safeRead(hr + off::HitResult_rayStart, &eye, sizeof(eye)) || !safeRead(hr + off::HitResult_rayDir, &d, sizeof(d)) ||
      !safeRead(hr + off::HitResult_type, &hitType, sizeof(hitType)) || !safeRead(hr + off::HitResult_pos, &hitPos, sizeof(hitPos)))
    return false;
  const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!(len > 1e-4f) || !finite(eye)) return false;
  dir = {d.x / len, d.y / len, d.z / len};
  return true;
}

// The world's BlockSource, found by its class name in the level renderer or its player (logged the first time).
void *blockSource(const char *levelRenderer) {
  // The class is a BlockSource subclass, so match the name's ending; the renderer player is searched too.
  static const char *sourceOwner = nullptr;
  static int sourceField = -2;
  if (sourceField == -2) {
    sourceField = -1;
    const char *player = nullptr;
    safeRead(levelRenderer + off::LevelRenderer_levelRendererPlayer, &player, 8);
    for (const char *owner : {levelRenderer, player}) {
      for (unsigned off = 0; owner && off < 0x1000 && sourceField < 0; off += 8) {
        const void *p = nullptr;
        if (!safeRead(owner + off, &p, 8) || !p) continue;
        const std::string name = className(p);
        if (name.size() >= 11 && name.compare(name.size() - 11, 11, "BlockSource") == 0) {
          sourceField = static_cast<int>(off), sourceOwner = owner;
          if (gLog) gLog->info("Hologram: block source {} at {} + {:#x}", name, owner == levelRenderer ? "level renderer" : "renderer player", off);
        }
      }
      if (sourceField >= 0) break;
    }
    if (sourceField < 0 && gLog) gLog->warn("Hologram: no block source found, built blocks can't be hidden");
  }
  void *source = nullptr;
  if (sourceField < 0 || !safeRead(sourceOwner + sourceField, &source, 8)) return nullptr;
  return source;
}

// BlockType of a block name, untagged. Render thread only.
uintptr_t typeOf(const std::string &name) {
  static std::unordered_map<std::string, uintptr_t> types;
  auto it = types.find(name);
  if (it != types.end()) return it->second;
  const HashedString hs(name);
  const uintptr_t t = reinterpret_cast<uintptr_t>(gLookup(gRegistry(), &hs)) & 0x00FFFFFFFFFFFFFFULL;
  types.emplace(name, t);
  return t;
}

// Ghost blocks the world already has right (same block type) are dropped from the hologram; checked twice a second
// on the render thread through the level renderer's BlockSource.
void refreshPlaced(const char *levelRenderer, const Structure &s) {
  std::vector<uint64_t> pending;
  bool full;
  std::unordered_set<uint64_t> placed;
  std::array<int, 3> origin;
  View view;
  {
    std::lock_guard lock(gMu);
    full = gFullCheck;
    gFullCheck = false;
    pending.swap(gPending);
    placed = gPlaced, origin = gOrigin, view = gView;
  }
  if (!full && pending.empty()) return;  // nothing placed since the last check: no world reads at all
  void *source = blockSource(levelRenderer);
  if (!source) return;
  auto matches = [&](int x, int y, int z, int32_t palette) {
    const BlockPos pos{x, y, z};
    const void *block = gGetBlock(source, pos);
    const void *type = nullptr;
    const bool ok = block && safeRead(static_cast<const char *>(block) + off::Block_blockType, &type, 8) && type &&
                    (reinterpret_cast<uintptr_t>(type) & 0x00FFFFFFFFFFFFFFULL) == typeOf(s.palette[palette].name);
    if (!ok && !full && gLog)  // placements are rare, so each miss is logged to compare the two types
      gLog->info("Hologram: {} {} {} wants {} ({:#x}), world block {} type {}", x, y, z, s.palette[palette].name,
                 typeOf(s.palette[palette].name), block, type);
    return ok;
  };
  bool changed = false;
  size_t total = 0;
  if (full) {  // once after showing or moving: every cell, since the mesh only holds the visible shell
    const auto size = viewSize(s, view);
    for (int x = 0; x < size[0]; ++x)
      for (int y = 0; y < size[1]; ++y)
        for (int z = 0; z < size[2]; ++z) {
          const int wx = origin[0] + x, wy = origin[1] + y, wz = origin[2] + z;
          const int32_t want = expectedAt(s, origin, view, {wx, wy, wz});
          if (want >= 0) ++total;
          if (want >= 0 && matches(wx, wy, wz, want) && placed.insert(packWorld(wx, wy, wz)).second) changed = true;
        }
  }
  auto sx = [](uint64_t v) { return static_cast<int>(v & 0x100000 ? static_cast<int64_t>(v) - 0x200000 : static_cast<int64_t>(v)); };
  if (!full) {  // built cells the player broke since come back as ghosts
    for (auto it = placed.begin(); it != placed.end();) {
      const int x = sx(*it >> 42), y = sx((*it >> 21) & 0x1FFFFF), z = sx(*it & 0x1FFFFF);
      const int32_t want = expectedAt(s, origin, view, {x, y, z});
      if (want >= 0 && matches(x, y, z, want)) {
        ++it;
      } else {
        it = placed.erase(it);
        changed = true;
      }
    }
  }
  for (uint64_t key : pending) {  // just placed
    const int x = sx(key >> 42), y = sx((key >> 21) & 0x1FFFFF), z = sx(key & 0x1FFFFF);
    const int32_t want = expectedAt(s, origin, view, {x, y, z});
    if (want >= 0 && matches(x, y, z, want) && placed.insert(key).second) changed = true;
  }
  if (gLog)
    gLog->info("Hologram: {} check, {} placed, {} built cells", full ? "full" : "placement", pending.size(), placed.size());
  if (!changed && !full) return;
  std::function<void(size_t, size_t)> progress;
  size_t built;
  {
    std::lock_guard lock(gMu);
    if (gFullCheck) return;  // moved while checking: these positions are stale, the next frame redoes it
    if (full) gTotal = total;
    if (changed) {
      gPlaced = std::move(placed);
      rebuildLocked();
    }
    progress = gProgress, built = gPlaced.size(), total = gTotal;
  }
  if (progress) progress(built, total);
}

bool placedAt(int x, int y, int z) {
  std::lock_guard lock(gMu);
  return gPlaced.count(packWorld(x, y, z)) > 0;
}

// First ghost block along the ray within `reach` blocks (voxel walk).
bool hoveredCell(const Structure &s, const std::array<int, 3> &origin, const View &view, Vec3f eye, Vec3f dir,
                 std::array<int, 3> &out, float *entry = nullptr) {
  constexpr float reach = 8.0f;
  int c[3] = {static_cast<int>(std::floor(eye.x)), static_cast<int>(std::floor(eye.y)), static_cast<int>(std::floor(eye.z))};
  const float p[3] = {eye.x, eye.y, eye.z}, d[3] = {dir.x, dir.y, dir.z};
  int step[3];
  float tMax[3], tDelta[3];
  for (int k = 0; k < 3; ++k) {
    step[k] = d[k] > 0 ? 1 : -1;
    tDelta[k] = d[k] != 0 ? std::fabs(1.0f / d[k]) : 1e30f;
    const float edge = d[k] > 0 ? c[k] + 1 - p[k] : p[k] - c[k];
    tMax[k] = d[k] != 0 ? edge * tDelta[k] : 1e30f;
  }
  for (float t = 0; t <= reach;) {
    if (expectedAt(s, origin, view, {c[0], c[1], c[2]}) >= 0 && !placedAt(c[0], c[1], c[2])) {
      out = {c[0], c[1], c[2]};
      if (entry) *entry = t;
      return true;
    }
    const int k = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
    t = tMax[k];
    tMax[k] += tDelta[k];
    c[k] += step[k];
  }
  return false;
}

// Entry distance where the aim ray enters a world block's unit cube.
bool rayBlockEntry(Vec3f eye, Vec3f dir, const BlockPos &pos, float &entry) {
  float near = 0.0f, far = 1e30f;
  const float min[3] = {static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z)};
  const float max[3] = {min[0] + 1.0f, min[1] + 1.0f, min[2] + 1.0f};
  const float origin[3] = {eye.x, eye.y, eye.z}, direction[3] = {dir.x, dir.y, dir.z};
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(direction[k]) < 1e-6f) {
      if (origin[k] < min[k] || origin[k] > max[k]) return false;
      continue;
    }
    float a = (min[k] - origin[k]) / direction[k], b = (max[k] - origin[k]) / direction[k];
    if (a > b) std::swap(a, b);
    near = std::max(near, a), far = std::min(far, b);
    if (near > far) return false;
  }
  entry = near;
  return far >= 0.0f;
}

// Corner brackets around the hovered ghost block, gently breathing outward. Thin flat strips (both sides) on each face.
void drawHover(void *sc, void *tess, void *mat, const std::array<int, 3> &cell, const Vec3f &cam) {
  static const auto start = std::chrono::steady_clock::now();
  const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
  const float pulse = 0.5f + 0.5f * std::sin(secs * 6.2832f);
  const float gap = 0.02f + 0.05f * pulse, size = 1 + 2 * gap;
  const float len = (0.22f + 0.05f * pulse) / size, width = 0.06f / size;
  static constexpr float kFace[6][4][3] = {  // origin, U, V, outward normal (unit cube)
      {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, -1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
      {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, -1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
      {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}, {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0}}};
  const float base[3] = {cell[0] - gap - cam.x, cell[1] - gap - cam.y, cell[2] - gap - cam.z};
  auto at = [&](const float (*f)[3], float a, float b) {
    return Vec3f{base[0] + size * (f[0][0] + a * f[1][0] + b * f[2][0]), base[1] + size * (f[0][1] + a * f[1][1] + b * f[2][1]),
                 base[2] + size * (f[0][2] + a * f[1][2] + b * f[2][2])};
  };
  gBegin(tess, nullptr, QuadList, 6 * 4 * 2 * 2 * 4, 0);
  gColor(tess, 0.38f, 0.44f, 0.91f, 0.95f);
  auto rect = [&](const float (*f)[3], float a0, float b0, float a1, float b1) {
    const Vec3f q[4] = {at(f, a0, b0), at(f, a1, b0), at(f, a1, b1), at(f, a0, b1)};
    for (int k = 0; k < 4; ++k) gVertex(tess, q[k].x, q[k].y, q[k].z);
    for (int k = 3; k >= 0; --k) gVertex(tess, q[k].x, q[k].y, q[k].z);
  };
  for (const auto &f : kFace)
    for (int cu = 0; cu < 2; ++cu)
      for (int cv = 0; cv < 2; ++cv) {
        const float a = cu, b = cv, da = cu ? -1.0f : 1.0f, db = cv ? -1.0f : 1.0f;
        rect(f, std::min(a, a + da * len), std::min(b, b + db * width), std::max(a, a + da * len), std::max(b, b + db * width));
        rect(f, std::min(a, a + da * width), std::min(b, b + db * len), std::max(a, a + da * width), std::max(b, b + db * len));
      }
  submit(sc, tess, mat);
}

// A libc++ std::string at `at` that spells a "minecraft:" block name, or "". Both layouts are tried (short strings
// inline, long ones behind a pointer), so this doesn't depend on which bit marks the layout in this build.
std::string nameAt(const char *at) {
  unsigned char hdr[24];
  if (!safeRead(at, hdr, sizeof(hdr))) return {};
  uint64_t size = 0, data = 0;
  std::memcpy(&size, hdr + 8, 8), std::memcpy(&data, hdr + 16, 8);
  struct Form {
    const char *text;
    size_t size;
  };
  const Form forms[] = {{at + 1, static_cast<size_t>(hdr[0] >> 1)}, {at + 1, static_cast<size_t>(hdr[0] & 0x7F)},
                        {reinterpret_cast<const char *>(data & 0x00FFFFFFFFFFFFFFULL), static_cast<size_t>(size)}};
  for (const Form &f : forms) {
    if (f.size < 10 || f.size > 128) continue;
    std::string s(f.size, '\0');
    if (!safeRead(f.text, s.data(), f.size) || s.compare(0, 10, "minecraft:") != 0) continue;
    if (std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7F; })) return s;
  }
  return {};
}

// The block name stored in a BlockType, cached per type: found by scanning the first 0x200 bytes once, then read at
// the same offset. "" if the type has no such name.
std::string blockName(uintptr_t type) {
  static std::unordered_map<uintptr_t, std::string> names;  // render thread only
  static int nameOffset = -1;
  if (auto it = names.find(type); it != names.end()) return it->second;
  const char *obj = reinterpret_cast<const char *>(type);
  std::string name = nameOffset >= 0 ? nameAt(obj + nameOffset) : std::string();
  for (int off = 0; nameOffset < 0 && name.empty() && off < 0x200; off += 8) {
    name = nameAt(obj + off);
    if (!name.empty()) {
      nameOffset = off;
      if (gLog) gLog->info("BE-Matica: WAILA name field at +{:#x}", off);
    }
  }
  names.emplace(type, name);
  return name;
}

// "minecraft:oak_planks" -> "Oak Planks".
std::string displayName(const std::string &id) {
  std::string out = id.substr(id.find(':') + 1);  // no ':' gives npos + 1 = 0, keeping the whole name
  bool wordStart = true;
  for (char &c : out) {
    if (c == '_') c = ' ', wordStart = true;
    else if (wordStart) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))), wordStart = false;
  }
  return out;
}

// The world's BlockType at a cell, untagged. 0 when it can't be read.
uintptr_t worldType(void *source, const std::array<int, 3> &cell) {
  if (!source) return 0;
  const void *block = gGetBlock(source, BlockPos{cell[0], cell[1], cell[2]});
  const void *type = nullptr;
  if (block && safeRead(static_cast<const char *>(block) + off::Block_blockType, &type, 8) && type)
    return reinterpret_cast<uintptr_t>(type) & 0x00FFFFFFFFFFFFFFULL;
  return 0;
}

// The ghost cell under the crosshair, unless the world block the game hits is nearer along the ray (a real block
// in front of a ghost hides it).
bool hoveredGhost(bool aimed, Vec3f eye, Vec3f dir, int32_t hitType, const BlockPos &hitPos, std::array<int, 3> &cell) {
  if (!aimed) return false;
  std::shared_ptr<const Structure> structure;
  std::array<int, 3> origin;
  View view;
  {
    std::lock_guard lock(gMu);
    structure = gStructure, origin = gOrigin, view = gView;
  }
  float ghostEntry = 0.0f;
  if (!structure || !hoveredCell(*structure, origin, view, eye, dir, cell, &ghostEntry)) return false;
  if (hitType != 0) return true;
  float worldEntry = 0.0f;
  return rayBlockEntry(eye, dir, hitPos, worldEntry) && ghostEntry <= worldEntry + 1e-4f;
}

// Looks at the block under the crosshair and publishes what it is when that or the structure's wish for the cell
// changes. The world read happens every frame, the name and the listener call only on a change.
void updateLook(const char *levelRenderer, bool aimed, int32_t hitType, const BlockPos &pos, bool hovering,
                const std::array<int, 3> &ghost) {
  std::shared_ptr<const Structure> structure;
  std::array<int, 3> origin;
  View view;
  std::function<void(const LookInfo &)> listener;
  {
    std::lock_guard lock(gMu);
    if (!gLookListener) return;
    structure = gStructure, origin = gOrigin, view = gView, listener = gLookListener;
  }
  LookKey key;
  key.structure = structure;
  void *source = blockSource(levelRenderer);
  if (hovering && structure) {
    key.valid = true, key.ghost = true, key.cell = ghost;
    key.type = worldType(source, ghost);
    key.palette = expectedAt(*structure, origin, view, ghost);
  } else if (aimed && hitType == 0 && source) {
    const void *block = gGetBlock(source, pos);
    const void *type = nullptr;
    if (block && safeRead(static_cast<const char *>(block) + off::Block_blockType, &type, 8) && type) {
      key.valid = true;
      key.type = reinterpret_cast<uintptr_t>(type) & 0x00FFFFFFFFFFFFFFULL;
      if (structure) key.palette = expectedAt(*structure, origin, view, {pos.x, pos.y, pos.z});
    }
  }
  {
    std::lock_guard lock(gMu);
    if (key == gLookKey) return;
    gLookKey = key;
  }
  LookInfo info;
  if (key.ghost) {
    if (key.palette >= 0) {
      const std::string &want = structure->palette[key.palette].name;
      const std::string world = key.type ? blockName(key.type) : std::string();
      const uintptr_t air = typeOf("minecraft:air");
      const bool isAir = world == "minecraft:air" || (air != 0 && key.type == air);
      const bool wrong = key.type != 0 && !isAir && typeOf(want) != key.type;
      info.valid = true, info.inside = true, info.name = displayName(want);
      info.note = wrong ? "Wrong block: " + displayName(world) : "Not placed";
      if (gLog) gLog->info("BE-Matica: WAILA ghost '{}' -> {}", want, info.note);
    }
  } else if (key.valid) {
    std::string id = blockName(key.type);
    if (id.empty() && structure)  // no name in the game's block: match the structure's names instead
      for (const auto &p : structure->palette)
        if (typeOf(p.name) == key.type) {
          id = p.name;
          break;
        }
    info.valid = true;
    info.name = id.empty() ? "Unknown block" : displayName(id);
    if (key.palette >= 0) {
      const std::string &want = structure->palette[key.palette].name;
      info.inside = true, info.match = typeOf(want) == key.type, info.want = displayName(want);
    }
  }
  listener(info);
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
  std::function<void(std::array<int, 3>)> originListener;
  {
    std::lock_guard lock(gMu);
    gCamera = cam;
    gHaveCamera = true;
    if (gPlaceAtPlayer.exchange(false)) {
      // The camera sits at eye height (1.62 blocks above the feet).
      gOrigin = {static_cast<int>(std::floor(cam.x)), static_cast<int>(std::floor(cam.y - 1.62f)),
                 static_cast<int>(std::floor(cam.z))};
      gPlaced.clear();
      gFullCheck = true;
      rebuildLocked();
    }
    mesh = gMesh;
    if (gStructure && gOriginListener && (!gOriginReported || gOrigin != gReportedOrigin)) {
      gReportedOrigin = gOrigin, gOriginReported = true;
      originListener = gOriginListener;
    }
  }
  if (originListener) originListener(gReportedOrigin);
  Vec3f eye, dir;
  int32_t hitType = -1;
  BlockPos hitPos{};
  const bool aimed = aimRay(static_cast<const char *>(self), eye, dir, hitType, hitPos);
  std::array<int, 3> ghost{};
  const bool hovering = hoveredGhost(aimed, eye, dir, hitType, hitPos, ghost);
  updateLook(static_cast<const char *>(self), aimed, hitType, hitPos, hovering, ghost);
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

  bool drewTextured = false;
  if (textured::enabled()) {
    std::shared_ptr<const Structure> structure;
    {
      std::lock_guard lock(gMu);
      structure = gStructure;
    }
    if (tintColor) tintColor[3] = gAlpha.load(std::memory_order_relaxed);  // see-through real blocks
    if (structure) drewTextured = textured::draw(screenContext, tess, static_cast<const char *>(self), lrp, mesh->blocks, *structure, cam);
    if (tintColor) tintColor[3] = 1.0f;
  }
  if (!drewTextured && haveFace && gFill.load(std::memory_order_relaxed)) drawQuads(screenContext, tess, faceMat, *mesh, cam, gAlpha.load(std::memory_order_relaxed));
  if (haveLine) {
    drawLines(screenContext, tess, lineMat, mesh->lines, cam);
    if (gOutlines.load(std::memory_order_relaxed)) drawLines(screenContext, tess, lineMat, mesh->blockLines, cam);
  }
  {
    std::shared_ptr<const Structure> structure;
    {
      std::lock_guard lock(gMu);
      structure = gStructure;
    }
    if (structure) refreshPlaced(static_cast<const char *>(self), *structure);
  }
  if (haveFace) {
    if (aimed && (dir.x != 0 || dir.z != 0)) {
      std::lock_guard lock(gMu);
      gForward = std::fabs(dir.x) > std::fabs(dir.z) ? std::array<int, 2>{dir.x > 0 ? 1 : -1, 0}
                                                     : std::array<int, 2>{0, dir.z > 0 ? 1 : -1};
    }
    if (hovering) drawHover(screenContext, tess, faceMat, ghost, cam);
  }

  std::memcpy(tessOffset, savedOffset, sizeof(savedOffset));
  if (tintColor) std::memcpy(tintColor, savedTint, sizeof(savedTint));
}

}  // namespace

namespace {
bool install(pl::log::Logger &log, std::string &error) {
  gLog = &log;
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
      {"BlockSourceGetBlock", sig::BlockSourceGetBlock, reinterpret_cast<void **>(&gGetBlock)},
      {"BlockRegistry", sig::BlockRegistry, reinterpret_cast<void **>(&gRegistry)},
      {"BlockLookupByName", sig::BlockLookupByName, reinterpret_cast<void **>(&gLookup)},
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
  textured::install(log);  // optional: the box hologram works without it
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
  textured::uninstall();
  if (gRenderLevelTarget) pl::memory::unhook(gRenderLevelTarget, reinterpret_cast<void *>(&renderLevelDetour));
  gRenderLevelTarget = nullptr;
}

void show(std::shared_ptr<const Structure> s, bool atPlayer) {
  std::lock_guard lock(gMu);
  gPlaced.clear();
  gFullCheck = true;
  gStructure = std::move(s);
  rebuildLocked();
  if (atPlayer) gPlaceAtPlayer = true;
}

void moveToPlayer() { gPlaceAtPlayer = true; }

void setOrigin(std::array<int, 3> origin) {
  std::lock_guard lock(gMu);
  gPlaceAtPlayer = false;
  gPlaced.clear();
  gFullCheck = true;
  gOrigin = origin;
  rebuildLocked();
}

void setOriginListener(std::function<void(std::array<int, 3>)> listener) {
  std::lock_guard lock(gMu);
  gOriginListener = std::move(listener);
  gOriginReported = false;
}

Snapshot snapshot() {
  std::lock_guard lock(gMu);
  if (!gVisible) return {};
  return {gStructure, gOrigin, gView, gCamera, gHaveCamera};
}

void nudge(int dx, int dy, int dz) {
  std::lock_guard lock(gMu);
  gPlaced.clear();
  gFullCheck = true;
  gOrigin[0] += dx, gOrigin[1] += dy, gOrigin[2] += dz;
  rebuildLocked();
}

void nudgeView(int forward, int right) {
  std::lock_guard lock(gMu);
  const auto [fx, fz] = gForward;
  // Right of the facing direction: facing south (+Z) the right hand points west (-X).
  const int dx = forward * fx - right * fz, dz = forward * fz + right * fx;
  gPlaced.clear();
  gFullCheck = true;
  gOrigin[0] += dx, gOrigin[2] += dz;
  rebuildLocked();
}

void setView(const View &view) {
  std::lock_guard lock(gMu);
  gPlaced.clear();
  gFullCheck = true;
  gView = view;
  rebuildLocked();
}
void setVisible(bool visible) { gVisible = visible; }
void setOpacity(float alpha) { gAlpha = std::clamp(alpha, 0.05f, 1.0f); }
void setOutlines(bool on) { gOutlines = on; }
void setFill(bool on) { gFill = on; }
void setProgressListener(std::function<void(size_t, size_t)> listener) {
  std::lock_guard lock(gMu);
  gProgress = std::move(listener);
}
void setLookListener(std::function<void(const LookInfo &)> listener) {
  std::lock_guard lock(gMu);
  gLookListener = std::move(listener);
  gLookKey = {};
}
void checkPlaced(int x, int y, int z) {
  std::lock_guard lock(gMu);
  gPending.push_back(packWorld(x, y, z));
}

}  // namespace bematica::hologram
