#include "textured.hpp"

#include "atlas.hpp"
#include "game.hpp"
#include "glquads.hpp"
#include "zipfile.hpp"

#include <pl/Logger.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// Positioning: retained section meshes placed by translating the world matrix (drawCached). If its helpers are not
// found, the game's own block model is tessellated every frame camera-relative for the nearest few blocks instead.
// Everything the game owns (ClientInstance, BlockTessellator, materials, atlas) is read through fault-safe reads;
// any failure makes draw() return false and the caller keeps drawing boxes.
namespace bematica::textured {
namespace {

using namespace game;

constexpr bool kAllowed = true;
constexpr float kRadius = 8.0f;           // blocks around the camera drawn with real textures
constexpr size_t kMaxBlocksPerFrame = 384;
constexpr size_t kBatch = 128;
constexpr const char *kTag = "BE-Matica";

std::atomic_bool gEnabled{false};
std::atomic_bool gReady{false};
TessBeginFn gBegin = nullptr;
RenderMeshFn gRenderMesh = nullptr;
// renderMeshImmediately(ScreenContext&, Tessellator&, MaterialPtr const&, <texture for slot 0>, <extra variant>)
// (26.60.4 0xb76a204): the overload whose texture reaches the material's texture slot 0 (via 0x117ae714). The plain
// one above passes an empty texture list, so block materials draw white.
using RenderMeshTexFn = void (*)(void *screenContext, void *tess, void *material, const void *texture0, void *extra);
RenderMeshTexFn gRenderMeshTex = nullptr;
AppendBlockFn gAppend = nullptr;
BlockRegistryFn gRegistry = nullptr;
BlockLookupFn gLookup = nullptr;
using TextureWrapFn = void (*)(void *holder, uint64_t a, uint64_t b, const void *variant);
TextureWrapFn gWrap = nullptr;

// Retained meshes (the LHolo approach, studied from its 26.60 Android build; own code): each 16^3 section of the
// hologram is tessellated once into a game mce::Mesh with section-relative positions. Each frame the top of the world
// matrix stack (ScreenContext+0x18) is translated to the section, the mesh drawn, and the matrix put back.
// The functions are the ones the slot-0 renderMeshImmediately (26.60.4 0xb76a204) itself calls, read from its BLs.
struct alignas(16) MeshBuf {
  unsigned char bytes[0x280];
};
using TessBusyFn = bool (*)(void *tess);
using TessEndFn = MeshBuf (*)(void *tess, int mode, const char *name, int nameLen, int flag);  // Mesh returned via x8
using MeshRenderFn = void (*)(void *mesh, void *screenContextPlus10, void *material, const void *texture0, int start, int count,
                              void *extra, void *unused);
using MeshDtorFn = void (*)(void *mesh);
using TessClearFn = void (*)(void *tess);
TessBusyFn gTessBusy = nullptr;
TessEndFn gTessEnd = nullptr;
MeshRenderFn gMeshRender = nullptr;
MeshDtorFn gMeshDtor = nullptr;
TessClearFn gTessClear = nullptr;

constexpr int kSection = 16;
constexpr float kCachedRadius = 96.0f;     // section centres farther than this are not drawn
constexpr size_t kBuildsPerFrame = 2;      // sections tessellated per frame (the rest wait for the next frames)
constexpr unsigned ScreenContext_worldMatrixStack = 0x18;  // MatrixStack*: std::deque<Matrix> map +0x50, start +0x68, size +0x70

uintptr_t untag(const void *p) { return reinterpret_cast<uintptr_t>(p) & 0x00FFFFFFFFFFFFFFULL; }

// Copies memory that may not be mapped: a bad address fails instead of crashing.
bool safeRead(const void *src, void *dst, size_t n) {
  iovec local{dst, n}, remote{reinterpret_cast<void *>(untag(src)), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

template <typename T> bool readPtr(const void *base, unsigned offset, T *&out) {
  void *p = nullptr;
  if (!safeRead(static_cast<const char *>(base) + offset, &p, sizeof(p)) || !p) return false;
  out = static_cast<T *>(p);
  return true;
}

void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, ap);
  va_end(ap);
}

// The default state of a block type: the pointer field whose Block points back at the type.
const void *defaultBlock(const void *type) {
  for (unsigned o = 0; o < 0x400; o += 8) {
    const void *p = nullptr, *back = nullptr;
    if (!safeRead(static_cast<const char *>(type) + o, &p, 8) || !p) continue;
    if (!safeRead(static_cast<const char *>(p) + off::Block_blockType, &back, 8)) continue;
    if (untag(back) == untag(type)) return p;
  }
  return nullptr;
}

// Only touched on the render thread.
const void *blockFor(const std::string &name) {
  static std::unordered_map<std::string, const void *> cache;
  if (auto it = cache.find(name); it != cache.end()) return it->second;
  const HashedString hs(name);
  const void *type = gLookup(gRegistry(), &hs);
  const void *block = type ? defaultBlock(type) : nullptr;
  info("BE-Matica: Textured block '%s' -> %s", name.c_str(), block ? "model found" : "not found (drawn as box)");
  cache.emplace(name, block);
  return block;
}

// Owned copy of the block atlas (LevelRenderer+0x1b8) made by the game's own variant copy, so the refcount is right.
// holder: {a, b, variant at +0x18, index at +0x30}; the texture argument is holder+0x18. Never destroyed on purpose.
struct Atlas {
  char *holder = nullptr;
  uint64_t source = 0;  // first word of the atlas object the copy was made from, to notice a resource reload
};
std::map<unsigned, Atlas> gAtlases;  // TEMPORARY: one owned copy per LevelRenderer texture slot tried

char *atlasTexture(const char *levelRenderer, unsigned slot = off::LevelRenderer_atlasTexture) {
  Atlas &gAtlas = gAtlases[slot];
  const char *atlasObj = levelRenderer + slot;
  uint64_t w[2]{};
  uint64_t probe = 0;
  if (!safeRead(atlasObj, w, sizeof(w)) || !w[0] || !w[1] || !safeRead(reinterpret_cast<void *>(w[0]), &probe, 8)) return nullptr;
  if (gAtlas.holder && gAtlas.source == w[0]) return gAtlas.holder + 0x18;
  alignas(16) unsigned char alias[0x20]{};  // non-owning TexturePtr alias (index 1); only read by the game's copy
  const uint64_t objAddr = reinterpret_cast<uint64_t>(atlasObj);
  std::memcpy(alias, &objAddr, 8);
  const uint32_t index = 1;
  std::memcpy(alias + 0x18, &index, 4);
  char *holder = static_cast<char *>(std::aligned_alloc(16, 0x40));
  if (!holder) return nullptr;
  std::memset(holder, 0, 0x40);
  gWrap(holder, 0, 0, alias);
  uint32_t got = 0;
  std::memcpy(&got, holder + 0x30, 4);
  if (got != 1) {  // the copy did not produce a TexturePtr variant: do not use it
    info("BE-Matica: Textured atlas copy failed (variant index %u)", got);
    return nullptr;
  }
  gAtlas = {holder, w[0]};
  info("BE-Matica: Textured atlas found at LevelRenderer+%#x (texture %#llx), owned copy %p", slot,
       static_cast<unsigned long long>(w[0]), static_cast<void *>(holder));
  return holder + 0x18;
}

std::mutex gAtlasMu;
std::shared_ptr<const TextureAtlas> gAtlasData;
std::filesystem::path gCacheDir;
std::atomic_bool gPrepareStarted{false};

// Loads the cached atlas, or builds it from the game's own apk (found through /proc/self/maps) and caches it.
void prepareAtlas(std::filesystem::path cacheDir) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto ms = [&] { return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()); };
  const std::vector<std::string> apks = gameArchiveCandidates();
  const std::string cacheFile = (cacheDir / "atlas.bin").string();
  std::map<std::string, std::vector<std::string>> byFolder;  // one install = one folder holding the apk and its splits
  for (const std::string &apk : apks) byFolder[std::filesystem::path(apk).parent_path().string()].push_back(apk);
  for (const auto &[folder, group] : byFolder) {
    const std::string stamp = archiveStamp(group);
    auto atlas = std::make_shared<TextureAtlas>();
    if (loadAtlas(cacheFile, stamp, *atlas)) {
      info("BE-Matica: Atlas loaded from cache %s (%dx%d, %zu textures, %zu blocks) in %lld ms", cacheFile.c_str(), atlas->width,
           atlas->height, atlas->tiles.size(), atlas->blocks.size(), ms());
      std::lock_guard lock(gAtlasMu);
      gAtlasData = std::move(atlas);
      return;
    }
    ResourcePack pack;
    std::string error;
    if (!pack.open(group, error) || !buildAtlas(pack, stamp, *atlas, error)) {
      info("BE-Matica: Atlas not built from %zu archive(s) in %s: %s", group.size(), folder.c_str(), error.c_str());
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    info("BE-Matica: Atlas built from %zu archive(s) in %s (%dx%d, %zu textures, %zu blocks) in %lld ms%s%s, cache %s", group.size(),
         folder.c_str(), atlas->width, atlas->height, atlas->tiles.size(), atlas->blocks.size(), ms(), error.empty() ? "" : "; note: ",
         error.c_str(), saveAtlas(*atlas, cacheFile) ? "written" : "NOT written");
    std::lock_guard lock(gAtlasMu);
    gAtlasData = std::move(atlas);
    return;
  }
  info("BE-Matica: Atlas unavailable: no readable game apk among %zu candidate(s)", apks.size());
}

struct Section {
  std::array<int, 3> origin{};
  std::vector<BlockCell> cells;
  uint64_t hash = 0;       // contents the mesh was built from
  uint64_t wantHash = 0;   // contents now
  MeshBuf *mesh = nullptr; // null: not built yet, or nothing drawable in it
  bool built = false;
};
std::map<std::array<int, 3>, Section> gSections;  // render thread only
const void *gSectionsFor = nullptr;                // blocks vector + structure the sections were grouped from
size_t gSectionsSize = 0;
const Structure *gSectionsStructure = nullptr;

void freeMesh(Section &sec) {
  if (sec.mesh) {
    gMeshDtor(sec.mesh);
    delete sec.mesh;
    sec.mesh = nullptr;
  }
}

void regroup(const std::vector<BlockCell> &blocks, const Structure &s) {
  if (gSectionsStructure != &s) {
    for (auto &[k, sec] : gSections) freeMesh(sec);
    gSections.clear();
  }
  gSectionsFor = blocks.data(), gSectionsSize = blocks.size(), gSectionsStructure = &s;
  auto floorDiv = [](int v) { return v >= 0 ? v / kSection : -((-v + kSection - 1) / kSection); };
  for (auto &[k, sec] : gSections) sec.cells.clear(), sec.wantHash = 0;
  for (const BlockCell &b : blocks) {
    const std::array<int, 3> key{floorDiv(b.x), floorDiv(b.y), floorDiv(b.z)};
    Section &sec = gSections[key];
    sec.origin = {key[0] * kSection, key[1] * kSection, key[2] * kSection};
    sec.cells.push_back(b);
    uint64_t h = sec.wantHash ^ 0xcbf29ce484222325ULL;
    for (int v : {b.x, b.y, b.z, static_cast<int>(b.palette)}) h = (h ^ static_cast<uint32_t>(v)) * 0x100000001b3ULL;
    sec.wantHash = h;
  }
  for (auto it = gSections.begin(); it != gSections.end();) {
    if (it->second.cells.empty()) {
      freeMesh(it->second);
      it = gSections.erase(it);
    } else {
      ++it;
    }
  }
}

// Tessellates one section into a retained mesh with positions relative to the section origin.
void buildSection(Section &sec, void *tess, char *blockTess, const Structure &s) {
  freeMesh(sec);
  sec.built = true;
  sec.hash = sec.wantHash;
  gBegin(tess, nullptr, QuadList, static_cast<int>(sec.cells.size() * 24), 0);
  float *offset = reinterpret_cast<float *>(static_cast<char *>(tess) + off::Tessellator_postTransformOffset);
  size_t appended = 0;
  for (const BlockCell &b : sec.cells) {
    if (b.palette < 0 || static_cast<size_t>(b.palette) >= s.palette.size()) continue;
    const void *block = blockFor(s.palette[b.palette].name);
    if (!block) continue;
    offset[0] = offset[1] = offset[2] = 0.0f;
    char *pb = nullptr, *pe = nullptr;
    std::memcpy(&pb, static_cast<char *>(tess) + 0x10, 8);
    std::memcpy(&pe, static_cast<char *>(tess) + 0x18, 8);
    const size_t before = pb && pe > pb ? static_cast<size_t>(pe - pb) / 12 : 0;
    gAppend(blockTess, tess, block);
    std::memcpy(&pb, static_cast<char *>(tess) + 0x10, 8);
    std::memcpy(&pe, static_cast<char *>(tess) + 0x18, 8);
    const size_t after = pb && pe > pb ? static_cast<size_t>(pe - pb) / 12 : 0;
    float *pos = reinterpret_cast<float *>(pb);
    // appendTessellatedBlock centres the model with setOffset(-0.5); corner it again, then place it in the section.
    const float tx = b.x - sec.origin[0] + 0.5f, ty = b.y - sec.origin[1] + 0.5f, tz = b.z - sec.origin[2] + 0.5f;
    for (size_t i = before; i < after; ++i) pos[i * 3] += tx, pos[i * 3 + 1] += ty, pos[i * 3 + 2] += tz;
    if (after > before) ++appended;
  }
  if (appended > 0) {
    static constexpr char kName[] = "BE-Matica";
    sec.mesh = new MeshBuf(gTessEnd(tess, 0, kName, static_cast<int>(sizeof(kName) - 1), 1));
  }
  gTessClear(tess);
}

// Top of the world matrix stack (column-major mat4), or null.
float *worldMatrix(void *screenContext) {
  const char *stack = nullptr, *map = nullptr, *block = nullptr;
  if (!readPtr(screenContext, ScreenContext_worldMatrixStack, stack)) return nullptr;
  uint64_t start = 0, size = 0;
  if (!readPtr(stack, 0x50, map) || !safeRead(stack + 0x68, &start, 8) || !safeRead(stack + 0x70, &size, 8)) return nullptr;
  if (size == 0 || size > 1024) return nullptr;
  const uint64_t top = start + size - 1;
  if (!readPtr(map, static_cast<unsigned>((top / 64) * 8), block)) return nullptr;
  float *m = reinterpret_cast<float *>(const_cast<char *>(block) + (top % 64) * 64);
  float probe[16];
  if (!safeRead(m, probe, sizeof(probe))) return nullptr;
  for (float f : probe)
    if (!std::isfinite(f)) return nullptr;
  return m;
}

// False when the cached path cannot run this frame (caller uses the near-camera path).
bool drawCached(void *screenContext, void *tess, char *blockTess, void *material, const void *slot0, void *extra,
                const std::vector<BlockCell> &blocks, const Structure &s, const Vec3f &cam, size_t &drawnSections) {
  drawnSections = 0;
  if (!gTessEnd || !gRenderMeshTex) return false;
  float *matrix = worldMatrix(screenContext);
  if (!matrix) return false;
  if (blocks.data() != gSectionsFor || blocks.size() != gSectionsSize || &s != gSectionsStructure) regroup(blocks, s);

  // Build the nearest stale sections first, a few per frame.
  if (!gTessBusy(tess)) {
    std::vector<std::pair<float, Section *>> stale;
    for (auto &[k, sec] : gSections) {
      if (sec.built && sec.hash == sec.wantHash) continue;
      const float dx = sec.origin[0] + 8 - cam.x, dy = sec.origin[1] + 8 - cam.y, dz = sec.origin[2] + 8 - cam.z;
      stale.emplace_back(dx * dx + dy * dy + dz * dz, &sec);
    }
    const size_t n = std::min(kBuildsPerFrame, stale.size());
    std::partial_sort(stale.begin(), stale.begin() + n, stale.end(), [](auto &a, auto &b) { return a.first < b.first; });
    float *offset = reinterpret_cast<float *>(static_cast<char *>(tess) + off::Tessellator_postTransformOffset);
    float savedOffset[3];
    std::memcpy(savedOffset, offset, sizeof(savedOffset));
    for (size_t i = 0; i < n; ++i) buildSection(*stale[i].second, tess, blockTess, s);
    std::memcpy(offset, savedOffset, sizeof(savedOffset));
  }

  float saved[16];
  std::memcpy(saved, matrix, sizeof(saved));
  for (auto &[k, sec] : gSections) {
    if (!sec.mesh) continue;
    const float dx = sec.origin[0] - cam.x, dy = sec.origin[1] - cam.y, dz = sec.origin[2] - cam.z;
    const float cx = dx + 8, cy = dy + 8, cz = dz + 8;
    if (cx * cx + cy * cy + cz * cz > kCachedRadius * kCachedRadius) continue;
    for (int r = 0; r < 4; ++r) matrix[12 + r] = saved[12 + r] + dx * saved[r] + dy * saved[4 + r] + dz * saved[8 + r];
    gMeshRender(sec.mesh, static_cast<char *>(screenContext) + 0x10, material, slot0, 0, 0, extra, nullptr);
    ++drawnSections;
  }
  std::memcpy(matrix, saved, sizeof(saved));
  return true;
}

}  // namespace

void setCacheDir(const std::filesystem::path &dir) { gCacheDir = dir; }

std::shared_ptr<const TextureAtlas> atlas() {
  std::lock_guard lock(gAtlasMu);
  return gAtlasData;
}

bool atlasWanted() { return gEnabled && atlas() != nullptr; }

void snapshotViewProjections() { glquads::snapshotViewProjections(); }

bool captureViewProjection(float out[16]) { return glquads::captureViewProjection(out); }

bool drawAtlas(const HologramMesh &mesh, const std::array<int, 3> &origin, const Vec3f &cam, float alpha, const float viewProj[16]) {
  static const TextureAtlas *uploaded = nullptr;
  static bool uploadOk = false;
  const auto data = atlas();
  if (!data) return false;
  if (uploaded != data.get()) {  // GL objects belong to the render thread, which is the only caller
    uploaded = data.get();
    uploadOk = glquads::uploadAtlas(*data);
  }
  if (!uploadOk) return false;
  const auto t0 = std::chrono::steady_clock::now();
  const Vec3f base{static_cast<float>(origin[0]), static_cast<float>(origin[1]), static_cast<float>(origin[2])};
  const Vec3f offset{static_cast<float>(origin[0] - static_cast<double>(cam.x)), static_cast<float>(origin[1] - static_cast<double>(cam.y)),
                     static_cast<float>(origin[2] - static_cast<double>(cam.z))};
  const bool ok = glquads::draw(mesh, base, offset, viewProj, alpha);
  static unsigned frames = 0;
  static bool firstLogged = false;
  if (ok && (!firstLogged || (frames++ % 600) == 0)) {
    firstLogged = true;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    info("BE-Matica: Atlas GL draw %zu faces in %lld us", mesh.quads.size(), static_cast<long long>(us));
  }
  return ok;
}

void install(pl::log::Logger &log) {
  if (!kAllowed) return;
  struct Want {
    const char *name, *sig;
    void **out;
  };
  const Want wants[] = {
      {"TessellatorBegin", sig::TessellatorBegin, reinterpret_cast<void **>(&gBegin)},
      {"RenderMeshImmediately", sig::RenderMeshImmediately, reinterpret_cast<void **>(&gRenderMesh)},
      {"AppendTessellatedBlock", sig::AppendTessellatedBlock, reinterpret_cast<void **>(&gAppend)},
      {"TextureVariantWrap", sig::TextureVariantWrap, reinterpret_cast<void **>(&gWrap)},
      {"BlockRegistry", sig::BlockRegistry, reinterpret_cast<void **>(&gRegistry)},
      {"BlockLookupByName", sig::BlockLookupByName, reinterpret_cast<void **>(&gLookup)},
  };
  std::vector<std::string> sigs;
  for (const auto &w : wants) sigs.emplace_back(w.sig);
  const auto found = pl::memory::resolveSignatures(sigs, "libminecraftpe.so");
  for (const auto &w : wants) {
    const auto it = found.find(w.sig);
    if (it == found.end() || !it->second) {
      log.info("BE-Matica: Textured unavailable (signature not found: {})", w.name);
      return;
    }
    *w.out = reinterpret_cast<void *>(it->second);
  }
  // No signature of its own yet: it sits 0x4e0 after the plain overload in 26.60.4; used only if its prologue matches.
  {
    static constexpr uint32_t kProlog[] = {0xa9bb7bfd, 0xf9000bfc, 0xa9025ff8, 0xa90357f6, 0xa9044ff4, 0x910003fd};
    const char *cand = reinterpret_cast<const char *>(gRenderMesh) + 0x4e0;
    uint32_t got[std::size(kProlog)];
    if (safeRead(cand, got, sizeof(got)) && std::memcmp(got, kProlog, sizeof(got)) == 0)
      gRenderMeshTex = reinterpret_cast<RenderMeshTexFn>(const_cast<char *>(cand));
    log.info("BE-Matica: Textured slot-0 renderMeshImmediately {}", gRenderMeshTex ? "found" : "NOT found (blocks draw untextured)");
  }
  if (gRenderMeshTex) {
    // BL targets inside it (26.60.4: busy check, Tessellator::end, mesh render with slot 0, ~Mesh, tessellator clear).
    auto blAt = [](unsigned offset) -> void * {
      const char *pc = reinterpret_cast<const char *>(gRenderMeshTex) + offset;
      uint32_t insn = 0;
      if (!safeRead(pc, &insn, 4) || (insn & 0xFC000000u) != 0x94000000u) return nullptr;
      const int64_t imm = static_cast<int64_t>(static_cast<int32_t>(insn << 6) >> 6) * 4;
      return const_cast<char *>(pc + imm);
    };
    gTessBusy = reinterpret_cast<TessBusyFn>(blAt(0x40));
    gTessEnd = reinterpret_cast<TessEndFn>(blAt(0x68));
    gMeshRender = reinterpret_cast<MeshRenderFn>(blAt(0x8c));
    gMeshDtor = reinterpret_cast<MeshDtorFn>(blAt(0x94));
    gTessClear = reinterpret_cast<TessClearFn>(blAt(0x9c));
    const bool all = gTessBusy && gTessEnd && gMeshRender && gMeshDtor && gTessClear;
    if (!all) gTessBusy = nullptr, gTessEnd = nullptr, gMeshRender = nullptr, gMeshDtor = nullptr, gTessClear = nullptr;
    log.info("BE-Matica: Textured cached meshes {}", all ? "available" : "unavailable (drawing near the camera only)");
  }
  gReady = true;
  log.info("BE-Matica: Textured ready (all signatures resolved)");
}

void uninstall() { gReady = false; }

void setEnabled(bool on) {
  gEnabled = on && kAllowed;
  info("BE-Matica: Textured %s", gEnabled ? "enabled" : "disabled");
  if (gEnabled && !gCacheDir.empty() && !gPrepareStarted.exchange(true)) std::thread(prepareAtlas, gCacheDir).detach();
}
bool enabled() { return gEnabled && gReady; }

bool draw(void *screenContext, void *tess, const char *levelRenderer, const char *lrp, const std::vector<BlockCell> &blocks,
          const Structure &s, const Vec3f &cam) {
  (void)lrp;
  if (!enabled()) return false;
  static bool failedLogged = false;
  auto fail = [&](const char *why) {
    if (!failedLogged) info("BE-Matica: Textured falling back to boxes (%s)", why);
    failedLogged = true;
    return false;
  };

  char *clientInstance = nullptr, *blockTess = nullptr, *itemInHand = nullptr;
  if (!readPtr(levelRenderer, off::LevelRenderer_clientInstance, clientInstance)) return fail("no ClientInstance");
  if (!readPtr(clientInstance, off::ClientInstance_blockTessellator, blockTess)) return fail("no BlockTessellator");
  if (!readPtr(clientInstance, off::ClientInstance_itemInHandRenderer, itemInHand)) return fail("no ItemInHandRenderer");
  // entity_alphablend: textured on 1.26.60 (device 2026-11-06) and honours the ghost alpha set in the shader colour.
  void *material = itemInHand + off::ItemInHandRenderer_matBlendBlock;
  void *materialPtr = nullptr;
  if (!safeRead(material, &materialPtr, 8) || !materialPtr) return fail("held-block material not set");
  char *texture = atlasTexture(levelRenderer);
  if (!texture) return fail("atlas not available");
  // Slot-0 texture: variant index 1 = the atlas object's two shared_ptrs (0x20 bytes) as they are. The game copies
  // them (taking references) into its own list and releases that copy; this stack alias is never destroyed.
  alignas(16) unsigned char slot0[0x28]{};
  alignas(16) unsigned char extra[0x58]{};  // no second texture, like the game's own outline draws
  if (!safeRead(levelRenderer + off::LevelRenderer_atlasTexture, slot0, 0x20)) return fail("atlas not readable");
  const uint32_t variantIndex = 1;
  std::memcpy(slot0 + 0x20, &variantIndex, 4);
  auto render = [&] {
    if (gRenderMeshTex) gRenderMeshTex(screenContext, tess, material, slot0, extra);
    else gRenderMesh(screenContext, tess, material, texture);
  };
  static bool stagesLogged = false;
  if (!stagesLogged) {
    stagesLogged = true;
    info("BE-Matica: Textured material entity_alphablend %p (ItemInHandRenderer+%#x), BlockTessellator %p", materialPtr,
         off::ItemInHandRenderer_matBlendBlock, static_cast<void *>(blockTess));
  }
  failedLogged = false;

  const auto t0 = std::chrono::steady_clock::now();
  size_t sections = 0;
  const bool cached = drawCached(screenContext, tess, blockTess, material, slot0, extra, blocks, s, cam, sections);
  static int lastCached = -1;
  if (static_cast<int>(cached) != lastCached) {
    lastCached = cached;
    info("BE-Matica: Textured path: %s", cached ? "cached section meshes" : "near-camera blocks every frame");
  }
  if (cached) {
    static unsigned frames = 0;
    if ((frames++ % 600) == 0) {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
      info("BE-Matica: Textured cached draw %zu of %zu sections within %.0f blocks in %lld us", sections, gSections.size(),
           kCachedRadius, static_cast<long long>(us));
    }
    return true;
  }
  float *offset = reinterpret_cast<float *>(static_cast<char *>(tess) + off::Tessellator_postTransformOffset);
  float saved[3];
  std::memcpy(saved, offset, sizeof(saved));
  // 1.26.60's appendTessellatedBlock sets the offset to -0.5 itself (0xaaada80 -> setOffset 0xac2c690), so blocks are
  // placed with the tessellator's vertex matrix instead, which Tessellator::vertex applies before the offset.
  char *matFlag = static_cast<char *>(tess) + off::Tessellator_useTransform;
  float *matrix = reinterpret_cast<float *>(static_cast<char *>(tess) + off::Tessellator_transform);
  char savedFlag = *matFlag;
  float savedMatrix[16];
  std::memcpy(savedMatrix, matrix, sizeof(savedMatrix));
  static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  (void)kIdentity;
  // Positions live in a std::vector<Vec3> at Tessellator+0x10 (Tessellator::vertex 0xac2ad74); whatever path the
  // block's model takes, its new positions are moved into place there after appending.
  auto positions = [&](float *&begin, size_t &count) {
    char *b = nullptr, *e = nullptr;
    std::memcpy(&b, static_cast<char *>(tess) + 0x10, 8);
    std::memcpy(&e, static_cast<char *>(tess) + 0x18, 8);
    begin = reinterpret_cast<float *>(b);
    count = b && e > b ? static_cast<size_t>(e - b) / 12 : 0;
  };
  size_t inBatch = 0, drawn = 0;
  uint64_t vertices = 0;
  for (const auto &b : blocks) {
    if (drawn >= kMaxBlocksPerFrame) break;
    const float dx = b.x + 0.5f - cam.x, dy = b.y + 0.5f - cam.y, dz = b.z + 0.5f - cam.z;
    if (dx * dx + dy * dy + dz * dz > kRadius * kRadius) continue;
    if (b.palette < 0 || static_cast<size_t>(b.palette) >= s.palette.size()) continue;
    const void *block = blockFor(s.palette[b.palette].name);
    if (!block) continue;
    if (inBatch == 0) gBegin(tess, nullptr, QuadList, static_cast<int>(kBatch * 24), 0);
    offset[0] = offset[1] = offset[2] = 0.0f;
    float *pos = nullptr;
    size_t before = 0, after = 0;
    positions(pos, before);
    gAppend(blockTess, tess, block);
    positions(pos, after);
    // The model is centred by the game's own setOffset(-0.5) on most paths; corner it again, then place it.
    const float tx = b.x - cam.x + 0.5f, ty = b.y - cam.y + 0.5f, tz = b.z - cam.z + 0.5f;
    for (size_t i = before; i < after; ++i) pos[i * 3] += tx, pos[i * 3 + 1] += ty, pos[i * 3 + 2] += tz;
    vertices += 24;
    ++drawn;
    if (++inBatch == kBatch) {
      render();
      inBatch = 0;
    }
  }
  if (inBatch) render();
  std::memcpy(offset, saved, sizeof(saved));
  std::memcpy(matrix, savedMatrix, sizeof(savedMatrix));
  *matFlag = savedFlag;

  static unsigned frames = 0;
  if (drawn > 0 && (frames++ % 600) == 0) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    info("BE-Matica: Textured draw %zu blocks (~%llu vertices) within %.0f blocks in %lld us", drawn,
         static_cast<unsigned long long>(vertices), kRadius, static_cast<long long>(us));
  }
  return drawn > 0;
}

}  // namespace bematica::textured
