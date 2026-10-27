#include "easyplace.hpp"

#include "game.hpp"
#include "hologram.hpp"
#include "mesh.hpp"

#include <pl/Logger.hpp>
#include <fmt/format.h>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bematica::easyplace {
namespace {

using namespace game;
using Cell = std::array<int, 3>;

std::atomic<BuildBlockFn> gBuildOrig{nullptr};
std::atomic<UseItemFn> gUseOrig{nullptr};
void *gBuildTarget = nullptr, *gUseTarget = nullptr;
constexpr float kReach = 6.0f;  // blocks; how far a tap into the air can reach a ghost block
GetBlockFn gGetBlock = nullptr;
ItemBlockTypeFn gItemBlockType = nullptr;
HandItemFn gHandItem = nullptr;
BlockRegistryFn gRegistry = nullptr;
BlockLookupFn gLookup = nullptr;

std::atomic_bool gEnabled{false};
std::function<void(const std::string &)> gMessage;
std::function<void()> gLayerDone;
pl::log::Logger *gLog = nullptr;

// One line per tap, so a tap that "does nothing" can be explained from the log.
// Holding the button repeats placements many times a second, so identical consecutive lines are skipped.
template <typename... Args> void trace(fmt::format_string<Args...> f, Args &&...args) {
  static thread_local std::string last;
  std::string line = fmt::format(f, std::forward<Args>(args)...);
  if (!gLog || line == last) return;
  gLog->info("{}", line);
  last = std::move(line);
}
thread_local bool tInside = false;  // our own calls into buildBlock pass straight through

// Bedrock block faces: down, up, north (-z), south (+z), west (-x), east (+x).
constexpr int kFaceDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

// Pointers can carry a hardware tag in the top byte; compare without it.
uintptr_t untag(const void *p) { return reinterpret_cast<uintptr_t>(p) & 0x00FFFFFFFFFFFFFFULL; }

template <typename T> T at(const void *base, unsigned offset) {
  T v;
  std::memcpy(&v, static_cast<const char *>(base) + offset, sizeof(T));
  return v;
}

char *hitResultOf(void *player) {
  void *level = at<void *>(player, off::Actor_level);
  void **vtable = level ? at<void **>(level, 0) : nullptr;
  auto getHit = vtable ? reinterpret_cast<GetHitResultFn>(vtable[off::Level_vtGetHitResult / sizeof(void *)]) : nullptr;
  return getHit ? static_cast<char *>(getHit(level)) : nullptr;
}

// BlockType for a block name. Only touched on the game thread.
const void *blockTypeOf(const std::string &name) {
  static std::unordered_map<std::string, const void *> cache;
  if (auto it = cache.find(name); it != cache.end()) return it->second;
  const HashedString hs(name);
  void *block = gLookup(gRegistry(), &hs);
  // The registry lookup hands back the BlockType itself, not a Block.
  const void *type = block;
  cache.emplace(name, type);
  return type;
}

struct World {
  void *blockSource;
  const void *airType;
  bool isAir(const Cell &c) const {
    const BlockPos p{c[0], c[1], c[2]};
    void *block = gGetBlock(blockSource, p);
    return block && untag(at<void *>(block, off::Block_blockType)) == untag(airType);
  }
};

// The first structure block along the view ray that is still empty in the world, stopping at the block
// that was actually clicked or, with nothing clicked, at reach. Filled cells are skipped: the ray passes
// through plants like grass and flowers, which aren't air.
bool findTarget(const hologram::Snapshot &h, const World &w, const Vec3f &eye, const Vec3f &dir, const Cell *clicked,
                Cell &out) {
  Cell c{static_cast<int>(std::floor(eye.x)), static_cast<int>(std::floor(eye.y)), static_cast<int>(std::floor(eye.z))};
  const float d[3] = {dir.x, dir.y, dir.z}, e[3] = {eye.x, eye.y, eye.z};
  int step[3];
  float tMax[3], tDelta[3];
  for (int a = 0; a < 3; ++a) {
    step[a] = d[a] > 0 ? 1 : -1;
    tDelta[a] = d[a] != 0 ? std::fabs(1.0f / d[a]) : INFINITY;
    const float edge = d[a] > 0 ? c[a] + 1.0f : static_cast<float>(c[a]);
    tMax[a] = d[a] != 0 ? (edge - e[a]) / d[a] : INFINITY;
  }
  const Cell head = c, feet{c[0], c[1] - 1, c[2]};  // the player can't place into their own body
  float t = 0;  // distance travelled along the ray
  for (int i = 0; i < 64 && t <= kReach + 1; ++i) {
    if (clicked && c == *clicked) return false;
    if (c != head && c != feet && expectedAt(*h.structure, h.origin, h.view, c) >= 0 && w.isAir(c)) {
      out = c;
      return true;
    }
    const int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
    t = tMax[a];
    c[a] += step[a];
    tMax[a] += tDelta[a];
  }
  return false;
}

std::string shortName(const std::string &name) {
  return name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
}

void say(const std::string &text) {
  if (gMessage) gMessage(text);
}

// Places the held block into `target` by clicking the face of a solid neighbour. The game checks the
// placement against its current hit result, so that is pointed at the same face for the call.
bool placeAt(void *gm, void *player, const World &w, const Cell &target, uint8_t hand, bool flag) {
  for (uint8_t face = 0; face < 6; ++face) {
    const Cell n{target[0] - kFaceDir[face][0], target[1] - kFaceDir[face][1], target[2] - kFaceDir[face][2]};
    if (w.isAir(n)) continue;
    const BlockPos pos{n[0], n[1], n[2]};
    const float hit[3] = {n[0] + 0.5f + 0.5f * kFaceDir[face][0], n[1] + 0.5f + 0.5f * kFaceDir[face][1],
                          n[2] + 0.5f + 0.5f * kFaceDir[face][2]};

    char *hr = hitResultOf(player);

    uint8_t savedHr[0x20];
    float savedAlt[3];
    const bool alt = at<uint8_t>(gm, off::GameMode_hasAltHit) != 0;
    if (hr) {
      std::memcpy(savedHr, hr + off::HitResult_type, sizeof(savedHr));
      const int type = 0, f = face;  // 0 = hit a block
      std::memcpy(hr + off::HitResult_type, &type, 4);
      std::memcpy(hr + off::HitResult_face, &f, 4);
      std::memcpy(hr + off::HitResult_pos, &pos, 12);
      std::memcpy(hr + off::HitResult_hitPos, hit, 12);
    }
    if (alt) {
      std::memcpy(savedAlt, static_cast<char *>(gm) + off::GameMode_altHitPos, sizeof(savedAlt));
      std::memcpy(static_cast<char *>(gm) + off::GameMode_altHitPos, hit, sizeof(hit));
    }

    tInside = true;
    const bool placed = gBuildOrig.load()(gm, &pos, face, hand, flag);
    tInside = false;
    trace("EasyPlace: clicked support {} {} {} face {} -> {}", n[0], n[1], n[2], face, placed ? "placed" : "rejected");

    if (hr) std::memcpy(hr + off::HitResult_type, savedHr, sizeof(savedHr));
    if (alt && !placed) std::memcpy(static_cast<char *>(gm) + off::GameMode_altHitPos, savedAlt, sizeof(savedAlt));
    return placed;
  }
  say("Nothing to place it against");
  return false;
}

struct Context {
  hologram::Snapshot h;
  void *player = nullptr;
  World world{};
  Vec3f eye{}, dir{};
};


// Everything Easy Place needs about the player and world, or false to leave the tap to the game.
bool context(void *gm, Context &ctx) {
  ctx.h = hologram::snapshot();
  ctx.player = at<void *>(gm, off::GameMode_player);
  if (!ctx.h.structure || !ctx.player) return false;
  void *dimension = at<void *>(ctx.player, off::Actor_dimension);
  void *source = dimension ? at<void *>(dimension, off::Dimension_blockSource) : nullptr;
  const void *air = blockTypeOf("minecraft:air");
  if (!source || !air) return false;
  ctx.world = {source, air};
  // The game's own aim ray (what the crosshair is pointing along), from the current hit result.
  const char *hr = hitResultOf(ctx.player);
  if (!hr) return false;
  ctx.eye = at<Vec3f>(hr, off::HitResult_rayStart);
  const Vec3f d = at<Vec3f>(hr, off::HitResult_rayDir);
  const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!(len > 1e-4f) || !std::isfinite(ctx.eye.x)) return false;
  ctx.dir = {d.x / len, d.y / len, d.z / len};
  static bool checked = false;  // the player's head is always in air: proves the air test works
  if (!checked) {
    checked = true;
    const Cell head{static_cast<int>(std::floor(ctx.eye.x)), static_cast<int>(std::floor(ctx.eye.y)),
                    static_cast<int>(std::floor(ctx.eye.z))};
    trace("EasyPlace check: head cell {} {} {} air={}", head[0], head[1], head[2], ctx.world.isAir(head));
  }
  return true;
}

// The held block must be the one the structure wants at `target`.
bool holdingRight(const Context &ctx, const Cell &target, uint8_t hand) {
  const std::string &want = ctx.h.structure->palette[expectedAt(*ctx.h.structure, ctx.h.origin, ctx.h.view, target)].name;
  const void *stack = gHandItem(ctx.player, hand);
  const void *held = stack ? gItemBlockType(stack).blockType : nullptr;
  const void *expected = blockTypeOf(want);
  if (!expected) {
    trace("EasyPlace unknown block: '{}'", want);
    say("Unknown block: " + shortName(want));
    return false;
  }
  if (!held || untag(held) != untag(expected)) {
    say("Hold " + shortName(want));
    return false;
  }
  return true;
}

// In single-layer view, tell the UI once every block of the layer exists in the world.
bool afterPlace(const Context &ctx, bool placed) {
  if (!placed || ctx.h.view.layers != View::Layers::Single || !gLayerDone) return placed;
  const auto size = viewSize(*ctx.h.structure, ctx.h.view);
  const int y = ctx.h.origin[1] + ctx.h.view.layer;
  for (int x = 0; x < size[0]; ++x)
    for (int z = 0; z < size[2]; ++z) {
      const Cell c{ctx.h.origin[0] + x, y, ctx.h.origin[2] + z};
      if (expectedAt(*ctx.h.structure, ctx.h.origin, ctx.h.view, c) >= 0 && ctx.world.isAir(c)) return placed;
    }
  gLayerDone();
  return placed;
}

bool buildBlockDetour(void *gm, const BlockPos *pos, uint8_t face, uint8_t hand, bool flag) {
  BuildBlockFn orig;
  while (!(orig = gBuildOrig.load(std::memory_order_acquire))) std::this_thread::yield();
  if (tInside) return orig(gm, pos, face, hand, flag);
  Context ctx;
  if (!gEnabled.load(std::memory_order_relaxed) || !gm || !pos || face > 5 || !context(gm, ctx)) {
    trace("EasyPlace build: pass (enabled={} context={})", gEnabled.load(), gm && pos && face <= 5);
    return orig(gm, pos, face, hand, flag);
  }
  trace("EasyPlace build: clicked {} {} {} face {} cam {:.1f} {:.1f} {:.1f} dir {:.2f} {:.2f} {:.2f}", pos->x, pos->y,
        pos->z, face, ctx.eye.x, ctx.eye.y, ctx.eye.z, ctx.dir.x, ctx.dir.y, ctx.dir.z);

  const Cell clicked{pos->x, pos->y, pos->z};
  const Cell normal{clicked[0] + kFaceDir[face][0], clicked[1] + kFaceDir[face][1], clicked[2] + kFaceDir[face][2]};
  Cell target;
  if (!findTarget(ctx.h, ctx.world, ctx.eye, ctx.dir, &clicked, target)) {
    if (expectedAt(*ctx.h.structure, ctx.h.origin, ctx.h.view, normal) < 0) {
      trace("EasyPlace build: no ghost block on the view ray, vanilla placement");
      return orig(gm, pos, face, hand, flag);
    }
    target = normal;  // aimed straight at a ghost block's spot
  }
  // Only the fallback (the spot next to the clicked face) can be filled here, e.g. by grass the game will
  // replace; the game decides whether it can place there.
  if (!holdingRight(ctx, target, hand)) {
    trace("EasyPlace build: wrong block in hand for {} {} {}", target[0], target[1], target[2]);
    return false;
  }
  trace("EasyPlace build: placing at {} {} {}", target[0], target[1], target[2]);

  if (target == normal) {
    tInside = true;
    const bool placed = orig(gm, pos, face, hand, flag);
    tInside = false;
    return afterPlace(ctx, placed);
  }
  return afterPlace(ctx, placeAt(gm, ctx.player, ctx.world, target, hand, flag));
}

// Tapping while aiming at air: the game never calls buildBlock, so a ghost block in reach is placed here.
bool useItemDetour(void *gm, void *stack, uint32_t flag) {
  UseItemFn orig;
  while (!(orig = gUseOrig.load(std::memory_order_acquire))) std::this_thread::yield();
  if (tInside) return orig(gm, stack, flag);
  Context ctx;
  if (!gEnabled.load(std::memory_order_relaxed) || !gm || !context(gm, ctx)) {
    trace("EasyPlace air: pass (enabled={})", gEnabled.load());
    return orig(gm, stack, flag);
  }
  Cell target;
  if (!findTarget(ctx.h, ctx.world, ctx.eye, ctx.dir, nullptr, target)) {
    trace("EasyPlace air: no ghost block within reach (cam {:.1f} {:.1f} {:.1f} dir {:.2f} {:.2f} {:.2f})",
          ctx.eye.x, ctx.eye.y, ctx.eye.z, ctx.dir.x, ctx.dir.y, ctx.dir.z);
    return orig(gm, stack, flag);
  }
  if (!ctx.world.isAir(target)) return orig(gm, stack, flag);
  if (!holdingRight(ctx, target, 0)) {
    trace("EasyPlace air: wrong block in hand for {} {} {}", target[0], target[1], target[2]);
    return false;
  }
  trace("EasyPlace air: placing at {} {} {}", target[0], target[1], target[2]);
  return afterPlace(ctx, placeAt(gm, ctx.player, ctx.world, target, 0, false));
}

bool install(pl::log::Logger &log, std::string &error) {
  struct Want {
    const char *name, *sig;
    void **out;
  };
  void *build = nullptr, *use = nullptr;
  const Want wants[] = {
      {"BuildBlock", sig::BuildBlock, &build},
      {"BaseUseItem", sig::BaseUseItem, &use},
      {"BlockSourceGetBlock", sig::BlockSourceGetBlock, reinterpret_cast<void **>(&gGetBlock)},
      {"ItemBlockType", sig::ItemBlockType, reinterpret_cast<void **>(&gItemBlockType)},
      {"HandItem", sig::HandItem, reinterpret_cast<void **>(&gHandItem)},
      {"BlockRegistry", sig::BlockRegistry, reinterpret_cast<void **>(&gRegistry)},
      {"BlockLookupByName", sig::BlockLookupByName, reinterpret_cast<void **>(&gLookup)},
  };
  std::vector<std::string> sigs;
  for (const auto &w : wants) sigs.emplace_back(w.sig);
  const auto found = pl::memory::resolveSignatures(sigs, "libminecraftpe.so");
  for (const auto &w : wants) {
    const auto it = found.find(w.sig);
    if (it == found.end() || !it->second) {
      error = std::string("game function not found: ") + w.name;
      return false;
    }
    *w.out = reinterpret_cast<void *>(it->second);
    log.info("BE-Matica: {} at {:#x}", w.name, it->second);
  }
  void *origSlot = nullptr;
  if (pl::memory::hook(build, reinterpret_cast<void *>(&buildBlockDetour), &origSlot) != 0 || !origSlot) {
    error = "could not hook block placement";
    return false;
  }
  gBuildOrig.store(reinterpret_cast<BuildBlockFn>(origSlot), std::memory_order_release);
  gBuildTarget = build;
  origSlot = nullptr;
  if (pl::memory::hook(use, reinterpret_cast<void *>(&useItemDetour), &origSlot) != 0 || !origSlot) {
    error = "could not hook item use";
    return false;
  }
  gUseOrig.store(reinterpret_cast<UseItemFn>(origSlot), std::memory_order_release);
  gUseTarget = use;
  return true;
}

}  // namespace

void installAsync(pl::log::Logger &log, std::function<void(const std::string &)> message,
                  std::function<void()> layerDone, std::function<void(bool, const std::string &)> done) {
  gMessage = std::move(message);
  gLayerDone = std::move(layerDone);
  gLog = &log;
  std::thread([&log, done = std::move(done)] {
    std::string error;
    const bool ok = install(log, error);
    done(ok, error);
  }).detach();
}

void uninstall() {
  if (gBuildTarget) pl::memory::unhook(gBuildTarget, reinterpret_cast<void *>(&buildBlockDetour));
  gBuildTarget = nullptr;
  if (gUseTarget) pl::memory::unhook(gUseTarget, reinterpret_cast<void *>(&useItemDetour));
  gUseTarget = nullptr;
}

void setEnabled(bool enabled) { gEnabled = enabled; }

}  // namespace bematica::easyplace
