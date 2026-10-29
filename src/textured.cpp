#include "textured.hpp"

#include "game.hpp"

#include <pl/Logger.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <android/log.h>
#include <dlfcn.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace bematica::textured {
namespace {

using namespace game;

constexpr size_t kMaxBlocksPerFrame = 4096;  // the block tessellator runs every frame until meshes are cached
constexpr size_t kBatch = 256;

std::atomic_bool gEnabled{false};
std::atomic_bool gReady{false};
TessBeginFn gBegin = nullptr;
RenderMeshFn gRenderMesh = nullptr;
BlockTessCtorFn gCtor = nullptr;
AppendBlockFn gAppend = nullptr;
MaterialGetFn gMaterialGet = nullptr;
BlockRegistryFn gRegistry = nullptr;
BlockLookupFn gLookup = nullptr;
void *gMaterialGroup = nullptr;
uintptr_t gLibBase = 0;
std::atomic<MeshRenderFn> gMeshRenderOrig{nullptr};
void *gMeshRenderTarget = nullptr;

// The game's texture argument: std::variant<monostate, TexturePtr, ClientTexture, ServerTexture>.
struct TextureVariant {
  unsigned char storage[0x18];
  uint32_t index;
  uint32_t pad;
};
std::mutex gTextureMu;
TextureVariant gBlockTexture{};
bool gHaveTexture = false;

uintptr_t untag(const void *p) { return reinterpret_cast<uintptr_t>(p) & 0x00FFFFFFFFFFFFFFULL; }

// Copies memory that may not be mapped: a bad address fails instead of crashing.
bool safeRead(const void *src, void *dst, size_t n) {
  iovec local{dst, n}, remote{reinterpret_cast<void *>(untag(src)), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

void meshRenderDetour(void *mesh, void *context, void *material, uint64_t a3, uint64_t a4, void *textures, void *a6,
                      void *a7) {
  MeshRenderFn orig;
  while (!(orig = gMeshRenderOrig.load(std::memory_order_acquire))) std::this_thread::yield();
  uint32_t kind = 0;
  if (textures) safeRead(static_cast<char *>(textures) + 0x18, &kind, sizeof(kind));
  {
    // Diagnostic: every distinct caller once, with the texture kind and material it uses.
    const uintptr_t from = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - gLibBase;
    static std::mutex callersMu;
    static std::set<uintptr_t> callers;
    std::lock_guard lock(callersMu);
    if (callers.size() < 60 && callers.insert(from).second) {
      const void *mat = nullptr;
      if (material) safeRead(material, &mat, 8);
      __android_log_print(ANDROID_LOG_INFO, "BE-Matica", "Mesh draw from %#lx: textures %p kind %u, material %p",
                          static_cast<unsigned long>(from), textures, kind, mat);
    }
  }
  if (textures && kind >= 1 && kind <= 3) {
    const uintptr_t from = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - gLibBase;
    static std::mutex seenMu;
    static std::set<std::pair<uintptr_t, uint32_t>> seen;
    {
      std::lock_guard lock(seenMu);
      if (seen.size() < 40 && seen.insert({from, kind}).second) {
        uint64_t w[3]{};
        safeRead(textures, w, sizeof(w));
        __android_log_print(ANDROID_LOG_INFO, "BE-Matica", "Textured draw from %#lx: texture kind %u, %016llx %016llx %016llx",
                            static_cast<unsigned long>(from), kind, static_cast<unsigned long long>(w[0]),
                            static_cast<unsigned long long>(w[1]), static_cast<unsigned long long>(w[2]));
      }
    }
    // In 1.26.60.4 block item meshes (held blocks, inventory) are drawn from here, with the block atlas.
    if (from >= 0xab60000 && from < 0xab62000) {
      std::lock_guard lock(gTextureMu);
      std::memcpy(&gBlockTexture, textures, sizeof(gBlockTexture));
      gHaveTexture = true;
    }
  }
  orig(mesh, context, material, a3, a4, textures, a6, a7);
}

// The default state of a block type: the pointer field whose Block points back at the type.
const void *defaultBlock(const void *type) {
  for (unsigned off = 0; off < 0x400; off += 8) {
    const void *p = nullptr, *back = nullptr;
    if (!safeRead(static_cast<const char *>(type) + off, &p, 8) || !p) continue;
    if (!safeRead(static_cast<const char *>(p) + off::Block_blockType, &back, 8)) continue;
    if (untag(back) == untag(type)) {
      static std::atomic_bool logged{false};
      if (!logged.exchange(true))
        __android_log_print(ANDROID_LOG_INFO, "BE-Matica", "Textured: default block state at BlockType+%#x", off);
      return p;
    }
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
  cache.emplace(name, block);
  return block;
}

uintptr_t decodeAdrpAdd(uintptr_t at) {
  uint32_t adrp, add;
  std::memcpy(&adrp, reinterpret_cast<const void *>(at), 4);
  std::memcpy(&add, reinterpret_cast<const void *>(at + 4), 4);
  int64_t imm = (static_cast<int64_t>((adrp >> 5) & 0x7FFFF) << 2) | ((adrp >> 29) & 3);
  if (imm & (1 << 20)) imm -= 1 << 21;
  return ((at & ~uintptr_t{0xFFF}) + (imm << 12)) + ((add >> 10) & 0xFFF);
}

}  // namespace

void install(pl::log::Logger &log) {
  struct Want {
    const char *name, *sig;
    void **out;
  };
  void *groupRef = nullptr;
  const Want wants[] = {
      {"TessellatorBegin", sig::TessellatorBegin, reinterpret_cast<void **>(&gBegin)},
      {"RenderMeshImmediately", sig::RenderMeshImmediately, reinterpret_cast<void **>(&gRenderMesh)},
      {"BlockTessellatorCtor", sig::BlockTessellatorCtor, reinterpret_cast<void **>(&gCtor)},
      {"AppendTessellatedBlock", sig::AppendTessellatedBlock, reinterpret_cast<void **>(&gAppend)},
      {"MaterialGroupGet", sig::MaterialGroupGet, reinterpret_cast<void **>(&gMaterialGet)},
      {"MeshRender", sig::MeshRender, &gMeshRenderTarget},
      {"MaterialGroupRef", sig::MaterialGroupRef, &groupRef},
      {"BlockRegistry", sig::BlockRegistry, reinterpret_cast<void **>(&gRegistry)},
      {"BlockLookupByName", sig::BlockLookupByName, reinterpret_cast<void **>(&gLookup)},
  };
  std::vector<std::string> sigs;
  for (const auto &w : wants) sigs.emplace_back(w.sig);
  const auto found = pl::memory::resolveSignatures(sigs, "libminecraftpe.so");
  for (const auto &w : wants) {
    const auto it = found.find(w.sig);
    if (it == found.end() || !it->second) {
      log.info("BE-Matica: textured blocks unavailable (not found: {})", w.name);
      return;
    }
    *w.out = reinterpret_cast<void *>(it->second);
  }
  gMaterialGroup = reinterpret_cast<void *>(decodeAdrpAdd(reinterpret_cast<uintptr_t>(groupRef) + sig::MaterialGroupRefAdrp));
  Dl_info info{};
  if (dladdr(gMeshRenderTarget, &info) && info.dli_fbase) gLibBase = reinterpret_cast<uintptr_t>(info.dli_fbase);
  void *origSlot = nullptr;
  if (pl::memory::hook(gMeshRenderTarget, reinterpret_cast<void *>(&meshRenderDetour), &origSlot) != 0 || !origSlot) {
    log.info("BE-Matica: textured blocks unavailable (could not hook mesh rendering)");
    gMeshRenderTarget = nullptr;
    return;
  }
  gMeshRenderOrig.store(reinterpret_cast<MeshRenderFn>(origSlot), std::memory_order_release);
  gReady = true;
  log.info("BE-Matica: textured blocks ready (material group at lib+{:#x})",
           reinterpret_cast<uintptr_t>(gMaterialGroup) - gLibBase);
}

void uninstall() {
  if (gMeshRenderTarget) pl::memory::unhook(gMeshRenderTarget, reinterpret_cast<void *>(&meshRenderDetour));
  gMeshRenderTarget = nullptr;
  gReady = false;
}

void setEnabled(bool on) { gEnabled = on; }
bool enabled() { return gEnabled && gReady; }

// Which game texture holds the block atlas moves between versions and could not be pinned down statically, so every
// texture variant reachable from the level renderer and its player (pointer fields -> objects -> variants) is collected
// once (fault-safe), and a debug button cycles through them until the blocks show their textures.
struct Candidate {
  int base;  // 0 = level renderer, 1 = its player
  unsigned field, variant;
};
std::vector<Candidate> gCandidates;
std::atomic<int> gCandidate{0};
bool gCollected = false;

void collect(const char *bases[2]) {
  gCollected = true;
  std::set<uint64_t> seen;
  for (int b = 0; b < 2; ++b)
    for (unsigned lo = 0; lo <= 0x1400; lo += 8) {
      const char *holder = nullptr;
      if (!safeRead(bases[b] + lo, &holder, 8) || !holder) continue;
      for (unsigned vo = 0; vo <= 0x400; vo += 8) {
        uint32_t kind = 0;
        // Terrain is drawn with ServerTextures (kind 3), so only those can be the block atlas.
        if (!safeRead(holder + vo + 0x18, &kind, 4) || kind != 3) continue;
        uint64_t w[3]{}, probe = 0;
        safeRead(holder + vo, w, sizeof(w));
        if (!w[0] || (w[0] >> 56 != 0 && w[0] >> 56 != 0xb4) || !safeRead(reinterpret_cast<const void *>(w[0]), &probe, 8))
          continue;
        if (!seen.insert(w[0]).second) continue;  // the same texture reached another way
        gCandidates.push_back({b, lo, vo});
      }
    }
  __android_log_print(ANDROID_LOG_INFO, "BE-Matica", "Textured: %zu texture candidates", gCandidates.size());
}

std::atomic_bool gAutoCycle{false};

bool candidateTexture(const char *bases[2], TextureVariant &out) {
  if (!gCollected) collect(bases);
  if (gCandidates.empty()) return false;
  if (gAutoCycle) {  // debug: next candidate every 4 seconds, logged so screenshots can be matched
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(4)) {
      last = now;
      const int n = ++gCandidate;
      const Candidate &c = gCandidates[static_cast<size_t>(n) % gCandidates.size()];
      __android_log_print(ANDROID_LOG_INFO, "BE-Matica", "Textured: showing candidate %zu/%zu (%s+%#x -> +%#x)",
                          static_cast<size_t>(n) % gCandidates.size() + 1, gCandidates.size(), c.base ? "player" : "renderer",
                          c.field, c.variant);
    }
  }
  const Candidate &c = gCandidates[static_cast<size_t>(gCandidate.load()) % gCandidates.size()];
  const char *holder = nullptr;
  if (!safeRead(bases[c.base] + c.field, &holder, 8) || !holder || !safeRead(holder + c.variant, &out, sizeof(out)))
    return false;
  uint64_t probe = 0;
  return out.index >= 1 && out.index <= 3 && safeRead(*reinterpret_cast<void *const *>(out.storage), &probe, 8);
}

std::string nextTexture() {
  gAutoCycle = !gAutoCycle;  // the button now starts/stops automatic cycling
  if (!gAutoCycle) return "Stopped on texture candidate " + std::to_string(gCandidate.load() % std::max<size_t>(1, gCandidates.size()) + 1);
  const int n = gCandidate.load();
  if (gCandidates.empty()) return "No texture candidates yet (load a structure with real textures on)";
  const Candidate &c = gCandidates[static_cast<size_t>(n) % gCandidates.size()];
  const std::string text = "Texture candidate " + std::to_string(n % gCandidates.size() + 1) + "/" +
                           std::to_string(gCandidates.size()) + (c.base ? " (player+" : " (renderer+") +
                           std::to_string(c.field) + " -> +" + std::to_string(c.variant) + ")";
  __android_log_print(ANDROID_LOG_INFO, "BE-Matica", "Textured: %s", text.c_str());
  return text;
}

bool draw(void *screenContext, void *tess, const char *levelRenderer, const char *lrp, const std::vector<BlockCell> &blocks,
          const Structure &s, const Vec3f &cam) {
  if (!enabled()) return false;
  TextureVariant texture;
  const char *bases[2] = {levelRenderer, lrp};
  if (!candidateTexture(bases, texture)) return false;
  static void *blockTessellator = nullptr;
  if (!blockTessellator) {
    blockTessellator = std::aligned_alloc(16, kBlockTessellatorSize);
    std::memset(blockTessellator, 0, kBlockTessellatorSize);
    gCtor(blockTessellator, nullptr);  // no BlockSource: blocks are drawn on their own, like held items
  }
  static MaterialPtr material;
  static bool materialTried = false;
  if (!materialTried) {
    materialTried = true;
    material = gMaterialGet(gMaterialGroup, HashedString("entity_alphablend"));
    __android_log_print(ANDROID_LOG_INFO, "BE-Matica", "Textured: material entity_alphablend %p", material.ptr);
  }
  if (!material.ptr) return false;

  float *offset = reinterpret_cast<float *>(static_cast<char *>(tess) + off::Tessellator_postTransformOffset);
  float saved[3];
  std::memcpy(saved, offset, sizeof(saved));
  size_t inBatch = 0, drawn = 0;
  for (const auto &b : blocks) {
    if (drawn >= kMaxBlocksPerFrame) break;
    if (b.palette < 0 || static_cast<size_t>(b.palette) >= s.palette.size()) continue;
    const void *block = blockFor(s.palette[b.palette].name);
    if (!block) continue;
    if (inBatch == 0) gBegin(tess, nullptr, QuadList, static_cast<int>(kBatch * 24), 0);
    offset[0] = b.x - cam.x, offset[1] = b.y - cam.y, offset[2] = b.z - cam.z;
    gAppend(blockTessellator, tess, block);
    ++drawn;
    if (++inBatch == kBatch) {
      gRenderMesh(screenContext, tess, &material, reinterpret_cast<char *>(&texture));
      inBatch = 0;
    }
  }
  if (inBatch) gRenderMesh(screenContext, tess, &material, reinterpret_cast<char *>(&texture));
  std::memcpy(offset, saved, sizeof(saved));
  return drawn > 0;
}

}  // namespace bematica::textured
