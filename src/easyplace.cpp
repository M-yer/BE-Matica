#include "easyplace.hpp"

#include "game.hpp"
#include "hologram.hpp"
#include "mesh.hpp"

#include <pl/Logger.hpp>
#include <fmt/format.h>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <array>
#include <sys/uio.h>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
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
SelectSlotFn gSelectSlot = nullptr;
ItemStackHasItemFn gItemStackHasItem = nullptr;
PickBlockFn gPickBlock = nullptr;
PlayerGameTypeFn gGameType = nullptr;
ItemStackCopyFn gItemStackCopy = nullptr;
ItemStackFromBlockFn gItemStackFromBlock = nullptr;
std::atomic<ServerTickFn> gServerTickOrig{nullptr};
void *gServerTickTarget = nullptr;
BlockRegistryFn gRegistry = nullptr;
BlockLookupFn gLookup = nullptr;
SetBlockFn gSetBlock = nullptr;  // BlockSource::setBlock; only ever called through the vtable slot that points at it
BlockChangeContextFn gMakeCtx = nullptr;

std::atomic_bool gEnabled{false};
std::atomic_bool gAirPlace{true};
std::atomic_bool gAutoSelect{true};
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

// Copies memory that may not be mapped: a bad address fails instead of crashing.
bool safeRead(const void *src, void *dst, size_t n) {
  iovec local{dst, n}, remote{reinterpret_cast<void *>(untag(src)), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

// Class name of a polymorphic game object from its RTTI ("5Level" -> "Level"), or "" if it is not one.
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

// Game updates move fields around, so the player's level and dimension (and the dimension's block source)
// are found by the class of what they point at. Offsets of the first match, scanned once.
int findField(const void *obj, unsigned from, unsigned to, std::string_view suffix) {
  for (unsigned off = from; off < to; off += 8) {
    const void *p = nullptr;
    if (!safeRead(static_cast<const char *>(obj) + off, &p, 8) || !p) continue;
    const std::string name = className(p);
    if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
      return static_cast<int>(off);
  }
  return -1;
}

struct Fields {
  int level = -1, dimension = -1, blockSource = -1, inventory = -1;
};

const Fields &fields(void *player) {
  static Fields f;
  static bool found = false;
  if (found || !player) return f;
  f.level = findField(player, 0x100, 0x400, "Level");
  f.dimension = findField(player, 0x100, 0x400, "Dimension");
  void *dimension = f.dimension >= 0 ? at<void *>(player, f.dimension) : nullptr;
  f.blockSource = dimension ? findField(dimension, 0, 0x400, "BlockSource") : -1;
  f.inventory = findField(player, 0x100, 0x800, "PlayerInventory");  // optional: only auto-select needs it
  found = f.level >= 0 && f.dimension >= 0 && f.blockSource >= 0;
  if (gLog)
    gLog->info("EasyPlace fields: player is {}, level @{:#x}, dimension @{:#x}, block source @{:#x}, inventory @{:#x}",
               className(player), f.level, f.dimension, f.blockSource, f.inventory);
  return f;
}

char *hitResultOf(void *player) {
  const int field = fields(player).level;
  void *level = field >= 0 ? at<void *>(player, field) : nullptr;
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
  // The last pass is air place: click the empty target cell itself, which the game fills since air is
  // replaceable.
  for (uint8_t face = 0; face < 7; ++face) {
    const bool air = face == 6;
    if (air && !gAirPlace.load(std::memory_order_relaxed)) break;
    const uint8_t clickFace = air ? 1 : face;
    const Cell n = air ? target
                       : Cell{target[0] - kFaceDir[face][0], target[1] - kFaceDir[face][1],
                              target[2] - kFaceDir[face][2]};
    if (!air && w.isAir(n)) continue;
    const BlockPos pos{n[0], n[1], n[2]};
    const float hit[3] = {n[0] + 0.5f + 0.5f * kFaceDir[clickFace][0], n[1] + 0.5f + 0.5f * kFaceDir[clickFace][1],
                          n[2] + 0.5f + 0.5f * kFaceDir[clickFace][2]};

    char *hr = hitResultOf(player);

    uint8_t savedHr[0x20];
    float savedAlt[3];
    const bool alt = at<uint8_t>(gm, off::GameMode_hasAltHit) != 0;
    if (hr) {
      std::memcpy(savedHr, hr + off::HitResult_type, sizeof(savedHr));
      const int type = 0, f = clickFace;  // 0 = hit a block
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
    const bool placed = gBuildOrig.load()(gm, &pos, clickFace, hand, flag);
    tInside = false;
    trace("EasyPlace: clicked {} {} {} {} face {} -> {}", air ? "air" : "support", n[0], n[1], n[2], clickFace, placed ? "placed" : "rejected");

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
bool contextFail(const char *step) {
  static std::atomic_bool logged{false};
  if (!logged.exchange(true) && gLog) gLog->info("EasyPlace context failed at '{}'", step);
  return false;
}

bool context(void *gm, Context &ctx) {
  ctx.h = hologram::snapshot();
  ctx.player = at<void *>(gm, off::GameMode_player);
  if (!ctx.h.structure) return false;
  if (!ctx.player) return contextFail("player");
  const Fields &f = fields(ctx.player);
  void *dimension = f.dimension >= 0 ? at<void *>(ctx.player, f.dimension) : nullptr;
  void *source = dimension && f.blockSource >= 0 ? at<void *>(dimension, f.blockSource) : nullptr;
  if (!source) return contextFail(dimension ? "block source" : "dimension");
  const void *air = blockTypeOf("minecraft:air");
  if (!air) return contextFail("air block");
  ctx.world = {source, air};
  // The game's own aim ray (what the crosshair is pointing along), from the current hit result.
  const char *hr = hitResultOf(ctx.player);
  if (!hr) return contextFail("hit result");
  ctx.eye = at<Vec3f>(hr, off::HitResult_rayStart);
  const Vec3f d = at<Vec3f>(hr, off::HitResult_rayDir);
  const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!(len > 1e-4f) || !std::isfinite(ctx.eye.x)) return contextFail("aim ray");
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

// Water and lava come from buckets, which aren't block items: Easy Place leaves those spots to the game.
bool wantsLiquid(const Context &ctx, const Cell &target) {
  const std::string &want = ctx.h.structure->palette[expectedAt(*ctx.h.structure, ctx.h.origin, ctx.h.view, target)].name;
  return want.find("water") != std::string::npos || want.find("lava") != std::string::npos;
}

struct InventoryView {
  void *inventory = nullptr;
  void *container = nullptr;
  using GetItemFn = const void *(*)(void *, int);
  GetItemFn getItem = nullptr;
};

bool inventoryView(void *player, InventoryView &out) {
  const int field = fields(player).inventory;
  if (field < 0 || !safeRead(static_cast<char *>(player) + field, &out.inventory, 8) || !out.inventory ||
      className(out.inventory) != "PlayerInventory")
    return false;
  void *vtable = nullptr, *getItemPtr = nullptr;
  if (!safeRead(static_cast<char *>(out.inventory) + off::PlayerInventory_container, &out.container, 8) ||
      !out.container || className(out.container).empty() || !safeRead(out.container, &vtable, 8) || !vtable ||
      !safeRead(static_cast<char *>(vtable) + off::Container_vtGetItem, &getItemPtr, 8) || !getItemPtr)
    return false;
  out.getItem = reinterpret_cast<InventoryView::GetItemFn>(getItemPtr);
  return true;
}

const void *itemAt(const InventoryView &view, int slot) { return view.getItem(view.container, slot); }

bool stackHasItem(const void *stack) { return stack && gItemStackHasItem && gItemStackHasItem(stack); }

bool stackHasType(const void *stack, const void *expected) {
  const void *type = stack ? gItemBlockType(stack).blockType : nullptr;
  return type && untag(type) == untag(expected);
}

// The hotbar slot (0-8) holding `expected`, or -1. Reads go through the inventory's own container.
int hotbarSlotWith(void *player, const void *expected) {
  InventoryView view;
  if (!inventoryView(player, view)) return -1;
  for (int slot = 0; slot < 9; ++slot)
    if (stackHasType(itemAt(view, slot), expected)) return slot;
  return -1;
}

// Switches the held slot to one that holds `expected`, as a tap on the hotbar would (the game tells the server).
bool selectFromHotbar(void *player, const void *expected) {
  if (!gSelectSlot) return false;
  const int slot = hotbarSlotWith(player, expected);
  if (slot < 0) return false;
  InventoryView view;
  if (!inventoryView(player, view)) return false;
  void *inventory = view.inventory;
  const bool ok = gSelectSlot(inventory, slot, 0);
  trace("EasyPlace: auto-select hotbar slot {} -> {}", slot, ok ? "ok" : "rejected");
  return ok;
}

// Moving a stack between inventory slots has to happen on the in-process server's copy of the inventory, or the server
// rejects the placement. The tap thread posts a job; the server thread runs it inside the ServerPlayer's own tick.
struct MoveJob {
  std::atomic_int state{0};  // 0 idle, 1 posted
  uintptr_t expected = 0;
  std::array<uintptr_t, 36> snapshot{};  // block type per slot of the local inventory, to find this player's ServerPlayer
  std::string want;
  bool inject = false;          // creative-inject: conjure `block` into the hotbar instead of moving a stack
  const void *block = nullptr;  // the wanted type's default Block
};
MoveJob gMove;
std::atomic<uintptr_t> gMoveFailedFor{0};  // type the last move could not do: later taps fall through to pick-block

uintptr_t slotType(const InventoryView &view, int slot) {
  const void *stack = itemAt(view, slot);
  if (!stackHasItem(stack)) return 0;
  return untag(gItemBlockType(stack).blockType);
}

// The Inventory's setItem is a known thunk; refuse to call anything else.
SetItemFn setItemOf(const InventoryView &view) {
  static constexpr uint8_t thunk[16] = {0x08, 0x00, 0x40, 0xF9, 0xE3, 0x03, 0x1F, 0x2A,
                                        0x04, 0x39, 0x40, 0xF9, 0x80, 0x00, 0x1F, 0xD6};
  void *vtable = nullptr, *fn = nullptr;
  uint8_t code[16];
  if (!safeRead(view.container, &vtable, 8) || !vtable ||
      !safeRead(static_cast<char *>(vtable) + off::Container_vtSetItem, &fn, 8) || !fn || !safeRead(fn, code, 16) ||
      std::memcmp(code, thunk, 16) != 0)
    return nullptr;
  return reinterpret_cast<SetItemFn>(fn);
}

void runMoveJob(void *serverPlayer) {
  const auto finish = [&](bool ok) {
    if (!ok) gMoveFailedFor.store(gMove.expected, std::memory_order_relaxed);
    else gMoveFailedFor.store(0, std::memory_order_relaxed);
    trace("EasyPlace: auto-pick {} via {} -> {}", shortName(gMove.want), gMove.inject ? "creative-inject" : "inventory-move",
          ok ? "ok" : "failed");
    gMove.state.store(0, std::memory_order_release);
  };
  InventoryView sv;
  if (className(serverPlayer) != "ServerPlayer" || !inventoryView(serverPlayer, sv)) return;  // not ours: wait for the next tick
  for (int slot = 0; slot < 36; ++slot)
    if (slotType(sv, slot) != gMove.snapshot[slot]) return;  // another player's inventory
  int source = -1, target = -1;
  if (!gMove.inject)
    for (int slot = 9; slot < 36 && source < 0; ++slot)
    if (slotType(sv, slot) == gMove.expected) source = slot;
  for (int slot = 0; slot < 9 && target < 0; ++slot)
    if (!stackHasItem(itemAt(sv, slot))) target = slot;
  if (target < 0 && (!safeRead(static_cast<char *>(sv.inventory) + off::PlayerInventory_selectedSlot, &target, sizeof(target)) ||
                     target < 0 || target >= 9))
    target = -1;
  const SetItemFn setItem = setItemOf(sv);
  alignas(16) unsigned char tmp[off::ItemStack_size + 16];
  void *vtable = nullptr, *dtor = nullptr;
  if (gMove.inject) {
    if (target < 0 || !setItem || !gItemStackFromBlock || !gMove.block) return finish(false);
    gItemStackFromBlock(tmp, gMove.block, 1, nullptr);
    if (!safeRead(tmp, &vtable, 8) || !vtable || !safeRead(vtable, &dtor, 8) || !dtor) return finish(false);  // tmp leaks
    if (stackHasItem(tmp)) setItem(sv.container, target, tmp);
    reinterpret_cast<void (*)(void *)>(dtor)(tmp);
    return finish(slotType(sv, target) == gMove.expected);
  }
  if (source < 0 || target < 0 || !setItem || !gItemStackCopy) return finish(false);
  gItemStackCopy(tmp, itemAt(sv, target));
  if (!safeRead(tmp, &vtable, 8) || !vtable || !safeRead(vtable, &dtor, 8) || !dtor) return finish(false);  // tmp leaks
  setItem(sv.container, target, itemAt(sv, source));
  setItem(sv.container, source, tmp);
  reinterpret_cast<void (*)(void *)>(dtor)(tmp);
  finish(slotType(sv, target) == gMove.expected);
}

// Vanilla placement orients a block by where the player looks. The structure's own states win: once the server has the
// block, the tap thread's job swaps it for the permutation with those states, on the server thread (ServerPlayer tick).
struct OrientJob {
  BlockPos pos;
  uintptr_t type;
  std::string name, desc;                      // desc: the wanted states, for the log
  std::vector<std::pair<std::string, int>> want;  // integer and bool states
  std::string unsupported;                     // string states, which this build cannot map to a value yet
  int ticks = 0;
};
std::mutex gOrientMu;
std::vector<OrientJob> gOrient;
std::atomic_int gOrientCount{0};
constexpr int kOrientTimeoutTicks = 200;

struct StateDesc {  // value of BlockType's state map: how one state is packed into the permutation index
  uint32_t count, bits, end, mask;
};

bool readPtr(const void *at, uintptr_t &out) {
  if (!safeRead(at, &out, 8)) return false;
  out = untag(reinterpret_cast<void *>(out));
  return true;
}

// BlockType+0x1B0 is the root of a std::map keyed by the state's id (the hash of its name).
bool findState(uintptr_t type, uint64_t key, StateDesc &out) {
  uintptr_t node = 0;
  if (!readPtr(reinterpret_cast<void *>(type + 0x1B0), node)) return false;
  for (int i = 0; node && i < 64; ++i) {
    uint64_t k = 0;
    if (!safeRead(reinterpret_cast<void *>(node + 0x20), &k, 8)) return false;
    if (k == key) {
      if (!safeRead(reinterpret_cast<void *>(node + 0x28), &out, sizeof(out))) return false;
      const uint32_t shift = out.end + 1 - out.bits;
      return out.count > 0 && out.bits > 0 && out.bits <= 16 && out.end < 16 && out.end + 1 >= out.bits &&
             out.mask == (((1u << out.bits) - 1) << shift);
    }
    if (!readPtr(reinterpret_cast<void *>(node + (k < key ? 8 : 0)), node)) return false;
  }
  return false;
}

// In-order dump of the state ids a type knows, for the log when a lookup fails.
void dumpStateKeys(uintptr_t node, std::string &out, int &budget) {
  if (!node || budget <= 0) return;
  uintptr_t left = 0, right = 0;
  uint64_t k = 0;
  StateDesc d{};
  if (!readPtr(reinterpret_cast<void *>(node), left) || !readPtr(reinterpret_cast<void *>(node + 8), right) ||
      !safeRead(reinterpret_cast<void *>(node + 0x20), &k, 8) || !safeRead(reinterpret_cast<void *>(node + 0x28), &d, sizeof(d)))
    return;
  dumpStateKeys(left, out, --budget);
  out += fmt::format(" [{:#x}: n={} bits={} end={} mask={:#x}]", k, d.count, d.bits, d.end, d.mask);
  dumpStateKeys(right, out, --budget);
}

// 1 done (ok or failed, logged), 0 the server does not have the block yet.
int runOrient(void *serverPlayer, OrientJob &j) {
  const Fields &f = fields(serverPlayer);
  void *dimension = f.dimension >= 0 ? at<void *>(serverPlayer, f.dimension) : nullptr;
  void *source = dimension && f.blockSource >= 0 ? at<void *>(dimension, f.blockSource) : nullptr;
  if (!source || !gGetBlock) return 0;
  void *block = gGetBlock(source, j.pos);
  uintptr_t blockType = 0;
  if (!block || !readPtr(static_cast<char *>(block) + off::Block_blockType, blockType) || blockType != j.type) return 0;
  const auto done = [&](const char *status, const std::string &note) {
    trace("EasyPlace: orient {} {} -> {}{}", shortName(j.name), j.desc, status, note.empty() ? "" : " (" + note + ")");
    return 1;
  };
  uint16_t data = 0;
  uintptr_t begin = 0, end = 0;
  if (!safeRead(static_cast<char *>(block) + 0x128, &data, 2) ||
      !readPtr(reinterpret_cast<void *>(j.type + off::BlockType_permutations), begin) ||
      !readPtr(reinterpret_cast<void *>(j.type + off::BlockType_permutations + 8), end) || end <= begin ||
      end - begin > 0x10000 * 8)
    return done("failed", "cannot read the permutation table");
  const size_t count = (end - begin) / 8;
  uintptr_t current = 0;
  if (data >= count || !readPtr(reinterpret_cast<void *>(begin + data * 8), current) || current != untag(block))
    return done("failed", fmt::format("permutation table does not hold the block: data {} of {}", data, count));

  uint32_t index = data;
  std::string missed = j.unsupported.empty() ? "" : "string states not supported: " + j.unsupported;
  for (const auto &[key, value] : j.want) {
    StateDesc d{};
    if (!findState(j.type, HashedString(key).hash, d)) {
      std::string keys;
      int budget = 24;
      uintptr_t root = 0;
      if (readPtr(reinterpret_cast<void *>(j.type + 0x1B0), root)) dumpStateKeys(root, keys, budget);
      trace("EasyPlace: orient {} state '{}' (id {:#x}) not found; known:{}", shortName(j.name), key, HashedString(key).hash,
            keys.empty() ? " none" : keys);
      missed += (missed.empty() ? "" : ", ") + key + " unknown";
      continue;
    }
    if (value < 0 || static_cast<uint32_t>(value) >= d.count) return done("failed", fmt::format("{}={} out of range {}", key, value, d.count));
    index = (index & ~d.mask) | (static_cast<uint32_t>(value) << (d.end + 1 - d.bits));
  }
  index &= 0xFFFF;
  uintptr_t wanted = 0, wantedType = 0;
  if (index >= count || !readPtr(reinterpret_cast<void *>(begin + index * 8), wanted) || !wanted ||
      !readPtr(reinterpret_cast<void *>(wanted + off::Block_blockType), wantedType) || wantedType != j.type)
    return done("failed", fmt::format("no permutation {} of {}", index, count));
  if (index == data) return done("ok", "already matches" + (missed.empty() ? "" : "; " + missed));

  uintptr_t vtable = 0, slot = 0;
  if (!gSetBlock || !gMakeCtx || !readPtr(source, vtable) || !vtable ||
      !readPtr(reinterpret_cast<void *>(vtable + off::BlockSource_vtSetBlock), slot) ||
      slot != untag(reinterpret_cast<void *>(gSetBlock)))
    return done("failed", "BlockSource::setBlock is not where expected");
  const BlockChangeContext ctx = gMakeCtx(0);
  const bool set = reinterpret_cast<SetBlockFn>(slot)(source, &j.pos, reinterpret_cast<void *>(wanted), 3, nullptr, &ctx);
  const bool now = untag(gGetBlock(source, j.pos)) == wanted;
  return done(set && now ? "ok" : "failed",
              fmt::format("data {} -> {}{}", data, index, missed.empty() ? "" : "; " + missed));
}

void runOrientJobs(void *serverPlayer) {
  static thread_local bool isServerPlayer = false;
  if (!isServerPlayer) {
    if (className(serverPlayer) != "ServerPlayer") return;
    isServerPlayer = true;
  }
  std::lock_guard lock(gOrientMu);
  for (size_t i = 0; i < gOrient.size();) {
    OrientJob &j = gOrient[i];
    const bool finished = runOrient(serverPlayer, j) == 1;
    if (!finished && ++j.ticks > kOrientTimeoutTicks)
      trace("EasyPlace: orient {} {} -> failed (the block never appeared)", shortName(j.name), j.desc);
    if (finished || j.ticks > kOrientTimeoutTicks) gOrient.erase(gOrient.begin() + i);
    else ++i;
  }
  gOrientCount.store(static_cast<int>(gOrient.size()), std::memory_order_release);
}

uint64_t serverTickDetour(void *self, void *a1, void *a2, void *a3) {
  const uint64_t result = gServerTickOrig.load(std::memory_order_acquire)(self, a1, a2, a3);
  if (gMove.state.load(std::memory_order_acquire) == 1) runMoveJob(self);
  if (gOrientCount.load(std::memory_order_acquire) > 0) runOrientJobs(self);
  return result;
}

// Posts a move of `expected` from the main inventory to the hotbar. True while the move is in flight.
bool queueMove(const InventoryView &view, const void *expected, const std::string &want) {
  if (!gServerTickOrig.load(std::memory_order_acquire) || !gItemStackCopy || !gItemStackHasItem) return false;
  if (gMoveFailedFor.load(std::memory_order_relaxed) == untag(expected)) return false;
  bool inMain = false;
  for (int slot = 9; slot < 36 && !inMain; ++slot) inMain = slotType(view, slot) == untag(expected);
  if (!inMain) return false;
  if (gMove.state.load(std::memory_order_acquire) == 1) return true;
  gMove.expected = untag(expected);
  gMove.want = want;
  gMove.inject = false;
  gMove.block = nullptr;
  for (int slot = 0; slot < 36; ++slot) gMove.snapshot[slot] = slotType(view, slot);
  gMove.state.store(1, std::memory_order_release);
  return true;
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

// Creative only: posts a job that builds a stack of the wanted block straight into the hotbar. True while in flight.
bool queueInject(const InventoryView &view, const void *expected, const std::string &want) {
  if (!gServerTickOrig.load(std::memory_order_acquire) || !gItemStackFromBlock || !gItemStackHasItem) return false;
  if (gMoveFailedFor.load(std::memory_order_relaxed) == untag(expected)) return false;
  for (int slot = 0; slot < 36; ++slot)
    if (slotType(view, slot) == untag(expected)) return false;  // already carried: nothing to conjure
  if (gMove.state.load(std::memory_order_acquire) == 1) return true;
  const void *block = defaultBlock(expected);
  if (!block) {
    gMoveFailedFor.store(untag(expected), std::memory_order_relaxed);
    trace("EasyPlace: auto-pick {} via creative-inject -> failed", shortName(want));
    return false;
  }
  gMove.expected = untag(expected);
  gMove.want = want;
  gMove.inject = true;
  gMove.block = block;
  for (int slot = 0; slot < 36; ++slot) gMove.snapshot[slot] = slotType(view, slot);
  gMove.state.store(1, std::memory_order_release);
  return true;
}

bool pickSource(const World &world, const Vec3f &eye, const void *expected, BlockPos &out) {
  if (!gGetBlock) return false;
  const Cell center{static_cast<int>(std::floor(eye.x)), static_cast<int>(std::floor(eye.y)),
                    static_cast<int>(std::floor(eye.z))};
  constexpr int radius = 6;
  for (int y = -radius; y <= radius; ++y)
    for (int z = -radius; z <= radius; ++z)
      for (int x = -radius; x <= radius; ++x) {
        if (x * x + y * y + z * z > radius * radius) continue;
        const BlockPos pos{center[0] + x, center[1] + y, center[2] + z};
        void *block = gGetBlock(world.blockSource, pos);
        void *type = nullptr;
        if (!block || !safeRead(static_cast<char *>(block) + off::Block_blockType, &type, 8) ||
            !type || untag(type) != untag(expected))
          continue;
        out = pos;
        return true;
      }
  return false;
}

// Player::getPlayerGameType; anything but creative (including unknown/default) keeps the survival-safe paths.
bool creativeMode(void *player) {
  static std::atomic_int last{-2};
  const int type = gGameType && player ? gGameType(player) : -1;
  if (last.exchange(type) != type)
    trace("EasyPlace: game mode {}", type == 1 ? "creative" : type == 0 ? "survival" : type == 2 ? "adventure" : "unknown");
  return type == 1;
}

bool autoPick(const Context &ctx, const void *expected, const std::string &want) {
  constexpr std::string_view method = "BlockPickRequest";
  const auto failed = [&] {
    trace("EasyPlace: auto-pick {} via {} -> failed", shortName(want), method);
    return false;
  };
  if (!gPickBlock || !gSelectSlot || !gItemStackHasItem) return failed();
  InventoryView view;
  if (!inventoryView(ctx.player, view)) return failed();
  if (queueMove(view, expected, want)) return false;  // the server thread does it; the next tap finds the block in the hotbar
  if (!creativeMode(ctx.player)) return false;  // survival: no conjuring, "Hold X"
  if (queueInject(view, expected, want)) return false;  // the next tap finds the block in the hotbar

  int empty = -1;
  for (int slot = 0; slot < 9; ++slot)
    if (!stackHasItem(itemAt(view, slot))) {
      empty = slot;
      break;
    }
  int selected = -1;
  if (empty < 0) {
    if (!safeRead(static_cast<char *>(view.inventory) + off::PlayerInventory_selectedSlot, &selected, sizeof(selected)) ||
        selected < 0 || selected >= 9)
      return failed();
    bool inMain = false;
    for (int slot = 9; slot < 36; ++slot)
      if (stackHasType(itemAt(view, slot), expected)) {
        inMain = true;
        break;
      }
    // A survival item in the main inventory must be moved only into an empty hotbar slot.
    // With no such slot, the only safe native path is creative pick-block into the selected slot.
    if (inMain) return failed();
  } else {
    selected = empty;
  }

  BlockPos source{};
  if (!pickSource(ctx.world, ctx.eye, expected, source)) return failed();
  if (empty >= 0 && !gSelectSlot(view.inventory, empty, 0)) return failed();
  struct PickHit {
    Vec3f rayStart;
    Vec3f rayDir;
    int type;
    int face;
    BlockPos pos;
    Vec3f hitPos;
  } hit{ctx.eye, ctx.dir, 0, 1, source, {static_cast<float>(source.x), static_cast<float>(source.y + 1),
                                          static_cast<float>(source.z)}};
  gPickBlock(ctx.player, &hit, false);
  const bool ok = stackHasType(itemAt(view, selected), expected);
  trace("EasyPlace: auto-pick {} via {} -> {}", shortName(want), method, ok ? "ok" : "failed");
  return ok;
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
  if ((!held || untag(held) != untag(expected)) && hand == 0 && gAutoSelect.load(std::memory_order_relaxed) &&
      (selectFromHotbar(ctx.player, expected) || autoPick(ctx, expected, want))) {
    stack = gHandItem(ctx.player, hand);
    held = stack ? gItemBlockType(stack).blockType : nullptr;
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

// Posts the structure's states for the block just placed at `cell`. Doors are two blocks: the cell above is posted too.
void queueOrient(const Context &ctx, const Cell &cell, bool upperDone = false) {
  if (!gSetBlock || !gMakeCtx || !gServerTickOrig.load(std::memory_order_acquire)) return;
  const Structure &st = *ctx.h.structure;
  const int32_t idx = expectedAt(st, ctx.h.origin, ctx.h.view, cell);
  if (idx < 0) return;
  const BlockState &bs = st.palette[idx];
  if (bs.states.items.empty()) return;
  OrientJob job{{cell[0], cell[1], cell[2]}, 0, bs.name, {}, {}, {}, 0};
  bool upper = false;
  for (size_t i = 0; i < bs.states.items.size() && i < bs.states.keys.size(); ++i) {
    const nbt::Tag &t = bs.states.items[i];
    const std::string &key = bs.states.keys[i];
    if (t.type == nbt::Type::Byte || t.type == nbt::Type::Short || t.type == nbt::Type::Int) {
      job.want.emplace_back(key, static_cast<int>(t.i));
      job.desc += fmt::format("{}{}={}", job.desc.empty() ? "" : ",", key, t.i);
      if (key == "upper_block_bit") upper = t.i != 0;
    } else {
      job.unsupported += (job.unsupported.empty() ? "" : ",") + key;
      job.desc += fmt::format("{}{}={}", job.desc.empty() ? "" : ",", key, t.type == nbt::Type::String ? t.s : "?");
    }
  }
  if (ctx.h.view.rotation != 0 || ctx.h.view.mirror) {
    trace("EasyPlace: orient {} {} -> skipped (rotated view)", shortName(bs.name), job.desc);
    return;
  }
  job.type = untag(blockTypeOf(bs.name));
  if (!job.type) return;
  {
    std::lock_guard lock(gOrientMu);
    if (gOrient.size() >= 64) gOrient.erase(gOrient.begin());
    gOrient.push_back(std::move(job));
    gOrientCount.store(static_cast<int>(gOrient.size()), std::memory_order_release);
  }
  if (!upperDone && !upper && bs.name.size() > 5 && bs.name.compare(bs.name.size() - 5, 5, "_door") == 0) {
    const Cell above{cell[0], cell[1] + 1, cell[2]};
    const int32_t a = expectedAt(st, ctx.h.origin, ctx.h.view, above);
    if (a >= 0 && st.palette[a].name == bs.name) queueOrient(ctx, above, true);
  }
}

bool buildBlockInner(void *gm, const BlockPos *pos, uint8_t face, uint8_t hand, bool flag);

// Every placement (vanilla or Easy Place) tells the hologram which spot to recheck, so it never polls the world.
bool buildBlockDetour(void *gm, const BlockPos *pos, uint8_t face, uint8_t hand, bool flag) {
  const bool placed = buildBlockInner(gm, pos, face, hand, flag);
  if (placed && pos && face <= 5) {
    hologram::checkPlaced(pos->x + kFaceDir[face][0], pos->y + kFaceDir[face][1], pos->z + kFaceDir[face][2]);
    hologram::checkPlaced(pos->x, pos->y, pos->z);  // replaceable blocks (grass) are placed into
  }
  return placed;
}

bool buildBlockInner(void *gm, const BlockPos *pos, uint8_t face, uint8_t hand, bool flag) {
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
  if (wantsLiquid(ctx, target)) {
    trace("EasyPlace build: liquid wanted at {} {} {}, vanilla placement", target[0], target[1], target[2]);
    return orig(gm, pos, face, hand, flag);
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
    if (placed) queueOrient(ctx, target);
    return afterPlace(ctx, placed);
  }
  const bool placed = placeAt(gm, ctx.player, ctx.world, target, hand, flag);
  if (placed) queueOrient(ctx, target);
  return afterPlace(ctx, placed);
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
  if (wantsLiquid(ctx, target)) {  // the bucket pours where the game aims it; then the ghost is rechecked
    const bool used = orig(gm, stack, flag);
    if (used) hologram::checkPlaced(target[0], target[1], target[2]);
    return used;
  }
  if (!holdingRight(ctx, target, 0)) {
    trace("EasyPlace air: wrong block in hand for {} {} {}", target[0], target[1], target[2]);
    return false;
  }
  trace("EasyPlace air: placing at {} {} {}", target[0], target[1], target[2]);
  const bool placed = placeAt(gm, ctx.player, ctx.world, target, 0, false);
  if (placed) {
    hologram::checkPlaced(target[0], target[1], target[2]);
    queueOrient(ctx, target);
  }
  return afterPlace(ctx, placed);
}

bool install(pl::log::Logger &log, std::string &error) {
  struct Want {
    const char *name, *sig;
    void **out;
  };
  void *build = nullptr, *use = nullptr, *serverTick = nullptr;
  const Want wants[] = {
      {"BuildBlock", sig::BuildBlock, &build},
      {"BaseUseItem", sig::BaseUseItem, &use},
      {"BlockSourceGetBlock", sig::BlockSourceGetBlock, reinterpret_cast<void **>(&gGetBlock)},
      {"ItemBlockType", sig::ItemBlockType, reinterpret_cast<void **>(&gItemBlockType)},
      {"SelectSlot", sig::SelectSlot, reinterpret_cast<void **>(&gSelectSlot)},
      {"ItemStackHasItem", sig::ItemStackHasItem, reinterpret_cast<void **>(&gItemStackHasItem)},
      {"PlayerGameType", sig::PlayerGameType, reinterpret_cast<void **>(&gGameType)},
      {"PickBlock", sig::PickBlock, reinterpret_cast<void **>(&gPickBlock)},
      {"ItemStackCopy", sig::ItemStackCopy, reinterpret_cast<void **>(&gItemStackCopy)},
      {"ItemStackFromBlock", sig::ItemStackFromBlock, reinterpret_cast<void **>(&gItemStackFromBlock)},
      {"ServerPlayerTick", sig::ServerPlayerTick, &serverTick},
      {"HandItem", sig::HandItem, reinterpret_cast<void **>(&gHandItem)},
      {"BlockRegistry", sig::BlockRegistry, reinterpret_cast<void **>(&gRegistry)},
      {"BlockLookupByName", sig::BlockLookupByName, reinterpret_cast<void **>(&gLookup)},
      {"BlockSourceSetBlock", sig::BlockSourceSetBlock, reinterpret_cast<void **>(&gSetBlock)},
      {"BlockChangeContext", sig::BlockChangeContext, reinterpret_cast<void **>(&gMakeCtx)},
  };
  std::vector<std::string> sigs;
  for (const auto &w : wants) sigs.emplace_back(w.sig);
  const auto found = pl::memory::resolveSignatures(sigs, "libminecraftpe.so");
  for (const auto &w : wants) {
    const auto it = found.find(w.sig);
    const bool optional = w.out == reinterpret_cast<void **>(&gSelectSlot) ||
                          w.out == reinterpret_cast<void **>(&gItemStackHasItem) ||
                          w.out == reinterpret_cast<void **>(&gPickBlock) ||
                          w.out == reinterpret_cast<void **>(&gGameType) ||
                          w.out == reinterpret_cast<void **>(&gItemStackCopy) ||
                          w.out == reinterpret_cast<void **>(&gItemStackFromBlock) || w.out == &serverTick ||
                          w.out == reinterpret_cast<void **>(&gSetBlock) || w.out == reinterpret_cast<void **>(&gMakeCtx);
    if ((it == found.end() || !it->second) && optional) {
      log.warn("BE-Matica: {} not found, {} disabled", w.name,
               w.out == reinterpret_cast<void **>(&gSelectSlot) ? "auto-select"
               : w.out == reinterpret_cast<void **>(&gSetBlock) || w.out == reinterpret_cast<void **>(&gMakeCtx)
                   ? "orientation"
                   : "auto-pick");
      continue;
    }
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
  if (serverTick) {
    origSlot = nullptr;
    if (pl::memory::hook(serverTick, reinterpret_cast<void *>(&serverTickDetour), &origSlot) == 0 && origSlot) {
      gServerTickOrig.store(reinterpret_cast<ServerTickFn>(origSlot), std::memory_order_release);
      gServerTickTarget = serverTick;
    } else {
      log.warn("BE-Matica: could not hook ServerPlayerTick, inventory-move disabled");
    }
  }
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
  if (gServerTickTarget) pl::memory::unhook(gServerTickTarget, reinterpret_cast<void *>(&serverTickDetour));
  gServerTickTarget = nullptr;
}

void setEnabled(bool enabled) { gEnabled = enabled; }
void setAirPlace(bool on) { gAirPlace = on; }
void setAutoSelect(bool on) { gAutoSelect = on; }

}  // namespace bematica::easyplace
