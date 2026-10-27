#include "easyplace.hpp"

#include "game.hpp"
#include "hologram.hpp"
#include "mesh.hpp"

#include <pl/Logger.hpp>
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
void *gBuildTarget = nullptr;
GetBlockFn gGetBlock = nullptr;
ItemBlockTypeFn gItemBlockType = nullptr;
HandItemFn gHandItem = nullptr;
BlockRegistryFn gRegistry = nullptr;
BlockLookupFn gLookup = nullptr;

std::atomic_bool gEnabled{false};
std::function<void(const std::string &)> gMessage;
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

// BlockType of a block name's default state. Only touched on the game thread.
const void *blockTypeOf(const std::string &name) {
  static std::unordered_map<std::string, const void *> cache;
  if (auto it = cache.find(name); it != cache.end()) return it->second;
  const HashedString hs(name);
  void *block = gLookup(gRegistry(), &hs);
  const void *type = block ? at<void *>(block, off::Block_blockType) : nullptr;
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

// The first structure block along the view ray that is still air in the world, stopping before the
// block that was actually clicked (every cell before it is air, or the ray couldn't have reached it).
bool findTarget(const hologram::Snapshot &h, const Vec3f &eye, const Vec3f &dir, const Cell &clicked, Cell &out) {
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
  for (int i = 0; i < 64; ++i) {  // well past any reach distance
    if (c == clicked) return false;
    if (c != head && c != feet && expectedAt(*h.structure, h.origin, h.view, c) >= 0) {
      out = c;
      return true;
    }
    const int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
    c[a] += step[a];
    tMax[a] += tDelta[a];
  }
  return false;
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

    void *level = at<void *>(player, off::Actor_level);
    void **vtable = level ? at<void **>(level, 0) : nullptr;
    auto getHit = vtable ? reinterpret_cast<GetHitResultFn>(vtable[off::Level_vtGetHitResult / sizeof(void *)]) : nullptr;
    char *hr = getHit ? static_cast<char *>(getHit(level)) : nullptr;

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

    if (hr) std::memcpy(hr + off::HitResult_type, savedHr, sizeof(savedHr));
    if (alt && !placed) std::memcpy(static_cast<char *>(gm) + off::GameMode_altHitPos, savedAlt, sizeof(savedAlt));
    return placed;
  }
  say("Nothing to place it against");
  return false;
}

std::string shortName(const std::string &name) {
  return name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name;
}

bool buildBlockDetour(void *gm, const BlockPos *pos, uint8_t face, uint8_t hand, bool flag) {
  BuildBlockFn orig;
  while (!(orig = gBuildOrig.load(std::memory_order_acquire))) std::this_thread::yield();
  if (tInside || !gEnabled.load(std::memory_order_relaxed) || !gm || !pos || face > 5)
    return orig(gm, pos, face, hand, flag);

  const auto h = hologram::snapshot();
  void *player = at<void *>(gm, off::GameMode_player);
  if (!h.structure || !h.haveCamera || !player) return orig(gm, pos, face, hand, flag);
  void *dimension = at<void *>(player, off::Actor_dimension);
  void *source = dimension ? at<void *>(dimension, off::Dimension_blockSource) : nullptr;
  const void *air = blockTypeOf("minecraft:air");
  if (!source || !air) return orig(gm, pos, face, hand, flag);
  const World world{source, air};

  // View direction from the player's pitch and yaw (degrees; yaw 0 faces +z).
  const auto rot = at<std::array<float, 2>>(player, off::Actor_rotation);
  const float pitch = rot[0] * static_cast<float>(M_PI) / 180, yaw = rot[1] * static_cast<float>(M_PI) / 180;
  const Vec3f dir{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};

  const Cell clicked{pos->x, pos->y, pos->z};
  const Cell normal{clicked[0] + kFaceDir[face][0], clicked[1] + kFaceDir[face][1], clicked[2] + kFaceDir[face][2]};
  Cell target;
  if (!findTarget(h, h.camera, dir, clicked, target)) {
    if (expectedAt(*h.structure, h.origin, h.view, normal) < 0) return orig(gm, pos, face, hand, flag);
    target = normal;  // aimed straight at a ghost block's spot
  }
  if (!world.isAir(target)) return false;  // already built: don't stack blocks inside the structure

  const std::string &want = h.structure->palette[expectedAt(*h.structure, h.origin, h.view, target)].name;
  const void *stack = gHandItem(player, hand);
  const void *held = stack ? gItemBlockType(stack).blockType : nullptr;
  const void *expected = blockTypeOf(want);
  if (!expected) {
    say("Unknown block: " + shortName(want));
    return false;
  }
  if (!held || untag(held) != untag(expected)) {
    say("Hold " + shortName(want));
    return false;
  }

  if (target == normal) {
    tInside = true;
    const bool placed = orig(gm, pos, face, hand, flag);
    tInside = false;
    return placed;
  }
  return placeAt(gm, player, world, target, hand, flag);
}

bool install(pl::log::Logger &log, std::string &error) {
  struct Want {
    const char *name, *sig;
    void **out;
  };
  void *build = nullptr;
  const Want wants[] = {
      {"BuildBlock", sig::BuildBlock, &build},
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
  return true;
}

}  // namespace

void installAsync(pl::log::Logger &log, std::function<void(const std::string &)> message,
                  std::function<void(bool, const std::string &)> done) {
  gMessage = std::move(message);
  std::thread([&log, done = std::move(done)] {
    std::string error;
    const bool ok = install(log, error);
    done(ok, error);
  }).detach();
}

void uninstall() {
  if (gBuildTarget) pl::memory::unhook(gBuildTarget, reinterpret_cast<void *>(&buildBlockDetour));
  gBuildTarget = nullptr;
}

void setEnabled(bool enabled) { gEnabled = enabled; }

}  // namespace bematica::easyplace
