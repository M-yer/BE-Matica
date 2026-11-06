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

#include <atomic>
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

// Positioning: no world MatrixStack was found, so a cached mesh cannot be placed. Instead the game's own block model
// is tessellated every frame with the camera-relative offset, but only for the nearest few blocks (bounded cost).
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
  // TEMPORARY device experiment: which held-block material shows the block models on 1.26.60. Each is tried for 5 s.
  static constexpr unsigned kMats[] = {off::ItemInHandRenderer_matAlphaColoredBlock, off::ItemInHandRenderer_matBlendBlock,
                                       off::ItemInHandRenderer_matOpaqueBlockColor, off::ItemInHandRenderer_matAlphaOneSidedColoredBlock};
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const unsigned matOff = kMats[(secs / 5) % std::size(kMats)];
  static unsigned lastMat = 0;
  if (matOff != lastMat) {
    lastMat = matOff;
    info("BE-Matica: Textured trying material ItemInHandRenderer+%#x", matOff);
  }
  void *material = itemInHand + matOff;
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
