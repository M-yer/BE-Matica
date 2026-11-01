#include "easyplace.hpp"

#include "game.hpp"
#include "hologram.hpp"
#include "itemmap.hpp"
#include "mesh.hpp"
#include "orient.hpp"

#include <pl/Logger.hpp>
#include <fmt/format.h>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

#include <array>
#include <sys/uio.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
ItemStackFromNameFn gItemStackFromName = nullptr;
std::atomic<ServerTickFn> gServerTickOrig{nullptr};
void *gServerTickTarget = nullptr;
BlockRegistryFn gRegistry = nullptr;
BlockLookupFn gLookup = nullptr;
SetBlockFn gSetBlock = nullptr;  // BlockSource::setBlock; only ever called through the vtable slot that points at it
BlockChangeContextFn gMakeCtx = nullptr;
BlockStateByNameFn gStateByName = nullptr;

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

// Blocks that open, toggle or press when clicked: placing against one would use it instead of placing. Block types are
// looked up by name once (the lists are Bedrock names), so this only runs on the game thread.
bool interactable(const World &w, const Cell &c) {
  static std::unordered_set<uintptr_t> types;
  static bool built = false;
  if (!built) {
    built = true;
    std::vector<std::string> names;
    for (const char *n : {"barrel", "chest", "trapped_chest", "ender_chest", "furnace", "lit_furnace", "blast_furnace",
                          "lit_blast_furnace", "smoker", "lit_smoker", "crafting_table", "hopper", "dropper", "dispenser",
                          "crafter", "brewing_stand", "anvil", "enchanting_table", "lectern", "loom", "cartography_table",
                          "smithing_table", "fletching_table", "grindstone", "stonecutter_block", "beacon", "bell",
                          "jukebox", "noteblock", "cake", "bed", "lever", "stone_button", "polished_blackstone_button",
                          "wooden_button", "wooden_door", "trapdoor", "fence_gate", "unpowered_repeater", "powered_repeater",
                          "unpowered_comparator", "powered_comparator", "daylight_detector", "command_block",
                          "chain_command_block", "repeating_command_block", "chiseled_bookshelf", "decorated_pot",
                          "undyed_shulker_box"})
      names.push_back(std::string("minecraft:") + n);
    for (const char *wood : {"spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "cherry", "pale_oak", "bamboo",
                             "crimson", "warped"})
      for (const char *kind : {"door", "trapdoor", "fence_gate", "button"})
        names.push_back(std::string("minecraft:") + wood + "_" + kind);
    for (const char *color : {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray",
                              "cyan", "purple", "blue", "brown", "green", "red", "black"})
      names.push_back(std::string("minecraft:") + color + "_shulker_box");
    for (const std::string &n : names)
      if (const void *t = blockTypeOf(n)) types.insert(untag(t));
  }
  const BlockPos p{c[0], c[1], c[2]};
  void *block = gGetBlock(w.blockSource, p);
  return block && types.count(untag(at<void *>(block, off::Block_blockType)));
}

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
// `preferFace` (-1: none) is tried first, for blocks whose facing the game takes from the clicked face.
bool placeAt(void *gm, void *player, const World &w, const Cell &target, uint8_t hand, bool flag, int preferFace = -1) {
  // Clicking an interactable block (barrel, door, lever...) uses it instead of placing, so those supports are left out
  // whenever another support or air place can take the click.
  bool otherSupport = gAirPlace.load(std::memory_order_relaxed);
  for (int f = 0; f < 6 && !otherSupport; ++f) {
    const Cell n{target[0] - kFaceDir[f][0], target[1] - kFaceDir[f][1], target[2] - kFaceDir[f][2]};
    otherSupport = !w.isAir(n) && !interactable(w, n);
  }
  // The last pass is air place: click the empty target cell itself, which the game fills since air is
  // replaceable.
  for (int step = 0; step < 8; ++step) {
    if (step == 0 && preferFace < 0) continue;
    const uint8_t face = static_cast<uint8_t>(step == 0 ? preferFace : step - 1);
    const bool air = face == 6;
    if (air && !gAirPlace.load(std::memory_order_relaxed)) break;
    const uint8_t clickFace = air ? 1 : face;
    const Cell n = air ? target
                       : Cell{target[0] - kFaceDir[face][0], target[1] - kFaceDir[face][1],
                              target[2] - kFaceDir[face][2]};
    if (!air && (w.isAir(n) || (otherSupport && interactable(w, n)))) continue;
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

// The ghost is water or lava from a bucket item: that bucket is auto-picked like a block before the game pours it.
bool wantsBucket(const Context &ctx, const Cell &target) {
  const std::string item =
      itemmap::itemFor(ctx.h.structure->palette[expectedAt(*ctx.h.structure, ctx.h.origin, ctx.h.view, target)].name);
  return item.size() > 7 && item.compare(item.size() - 7, 7, "_bucket") == 0;
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

// The game's own hasItem also demands that the item has a legacy block, which signs, doors, buckets and every other
// component-defined item lack, so a stack of those looked empty. Here a stack has an item when its holder names one.
bool stackHasItem(const void *stack) {
  const void *holder = nullptr, *item = nullptr;
  return stack && gItemStackHasItem && safeRead(static_cast<const char *>(stack) + off::ItemStack_item, &holder, 8) && holder &&
         safeRead(holder, &item, 8) && item;
}

// What places a block: normally its own block item (found by block type); doors, buckets, seeds and the like come from
// an item of another name (found by Item). `item` stays null when the game has no item of that name.
struct Wanted {
  std::string block, itemName;
  const void *type = nullptr, *item = nullptr;
  uintptr_t key() const { return item ? untag(item) : untag(type); }
  const std::string &label() const { return itemName.empty() ? block : itemName; }
};

// ItemStackBase keeps a pointer to a holder whose first word is the Item.
const void *itemOf(const void *stack) {
  const void *holder = nullptr, *item = nullptr;
  if (!stack || !safeRead(static_cast<const char *>(stack) + off::ItemStack_item, &holder, 8) || !holder ||
      !safeRead(holder, &item, 8))
    return nullptr;
  return item;
}

// The Item registered under a full name, found through the game's own name lookup (a throwaway stack is built and freed).
const void *itemNamed(const std::string &name) {
  if (!gItemStackFromName) return nullptr;
  alignas(16) unsigned char tmp[off::ItemStack_size + 16];
  gItemStackFromName(tmp, name.data(), name.size(), 1, 0, nullptr);
  void *vtable = nullptr, *dtor = nullptr;
  if (!safeRead(tmp, &vtable, 8) || !vtable || !safeRead(vtable, &dtor, 8) || !dtor) return nullptr;  // tmp leaks
  const void *item = stackHasItem(tmp) ? itemOf(tmp) : nullptr;
  reinterpret_cast<void (*)(void *)>(dtor)(tmp);
  return item;
}

Wanted wantedFor(const std::string &block) {
  Wanted w{block, itemmap::itemFor(block), blockTypeOf(block), nullptr};
  if (!w.itemName.empty()) w.item = itemNamed(w.itemName);
  return w;
}

bool stackMatches(const void *stack, const Wanted &w) {
  if (!stack) return false;
  // An item of the wanted name decides alone: a wall sign's own block item shares its block type but places the wrong thing.
  if (w.item) {
    const void *item = itemOf(stack);
    return item && untag(item) == untag(w.item);
  }
  const void *type = gItemBlockType(stack).blockType;
  return type && w.type && untag(type) == untag(w.type);
}

// The hotbar slot (0-8) holding `w`, or -1. Reads go through the inventory's own container.
int hotbarSlotWith(void *player, const Wanted &w) {
  InventoryView view;
  if (!inventoryView(player, view)) return -1;
  for (int slot = 0; slot < 9; ++slot)
    if (stackMatches(itemAt(view, slot), w)) return slot;
  return -1;
}

// Switches the held slot to one that holds `w`, as a tap on the hotbar would (the game tells the server).
bool selectFromHotbar(void *player, const Wanted &w) {
  if (!gSelectSlot) return false;
  const int slot = hotbarSlotWith(player, w);
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
  Wanted w;
  std::array<uintptr_t, 36> snapshot{};  // block type (else item) per slot of the local inventory, to find this player's ServerPlayer
  bool inject = false;          // creative-inject: conjure `block` into the hotbar instead of moving a stack
  const void *block = nullptr;  // the wanted type's default Block
  std::chrono::steady_clock::time_point posted{};
};
MoveJob gMove;
std::atomic<uintptr_t> gMoveFailedFor{0};  // type the last move could not do: later taps fall through to pick-block

// The client's and the server's copy of a slot must give the same value, so this is the Item itself: ItemBlockType on an
// item without a legacy block (swords, buckets...) is not stable between the two.
uintptr_t slotType(const InventoryView &view, int slot) {
  const void *stack = itemAt(view, slot);
  return stackHasItem(stack) ? untag(itemOf(stack)) : 0;
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
    if (!ok) gMoveFailedFor.store(gMove.w.key(), std::memory_order_relaxed);
    else gMoveFailedFor.store(0, std::memory_order_relaxed);
    trace("EasyPlace: auto-pick {} via {} -> {}", shortName(gMove.w.label()), gMove.inject ? "creative-inject" : "inventory-move",
          ok ? "ok" : "failed");
    gMove.state.store(0, std::memory_order_release);
  };
  InventoryView sv;
  if (className(serverPlayer) != "ServerPlayer" || !inventoryView(serverPlayer, sv)) {  // not ours: wait for the next tick
    if (std::chrono::steady_clock::now() - gMove.posted > std::chrono::seconds(2)) gMove.state.store(0, std::memory_order_release);
    return;
  }
  for (int slot = 0; slot < 36; ++slot)
    if (slotType(sv, slot) != gMove.snapshot[slot]) {
      // Another player's inventory, or ours changed since the tap: a job left posted would block every later auto-pick.
      if (std::chrono::steady_clock::now() - gMove.posted < std::chrono::seconds(1)) return;
      trace("EasyPlace: auto-pick {} -> dropped (inventory changed at slot {})", shortName(gMove.w.label()), slot);
      gMove.state.store(0, std::memory_order_release);
      return;
    }
  int source = -1, target = -1;
  if (!gMove.inject)
    for (int slot = 9; slot < 36 && source < 0; ++slot)
    if (stackMatches(itemAt(sv, slot), gMove.w)) source = slot;
  for (int slot = 0; slot < 9 && target < 0; ++slot)
    if (!stackHasItem(itemAt(sv, slot))) target = slot;
  if (target < 0 && (!safeRead(static_cast<char *>(sv.inventory) + off::PlayerInventory_selectedSlot, &target, sizeof(target)) ||
                     target < 0 || target >= 9))
    target = -1;
  const SetItemFn setItem = setItemOf(sv);
  alignas(16) unsigned char tmp[off::ItemStack_size + 16];
  void *vtable = nullptr, *dtor = nullptr;
  if (gMove.inject) {
    if (target < 0 || !setItem) return finish(false);
    // The item of the wanted name first (doors, buckets...); a block the game has no such item for falls back to its own block item.
    for (int pass = 0; pass < 2 && !stackMatches(itemAt(sv, target), gMove.w); ++pass) {
      const bool byName = pass == 0;
      if (byName ? gMove.w.itemName.empty() || !gItemStackFromName : !gItemStackFromBlock || !gMove.block) continue;
      if (byName) gItemStackFromName(tmp, gMove.w.itemName.data(), gMove.w.itemName.size(), 1, 0, nullptr);
      else gItemStackFromBlock(tmp, gMove.block, 1, nullptr);
      if (!safeRead(tmp, &vtable, 8) || !vtable || !safeRead(vtable, &dtor, 8) || !dtor) return finish(false);  // tmp leaks
      const bool built = stackHasItem(tmp);
      trace("EasyPlace: inject {} by {} -> {}", shortName(gMove.w.label()), byName ? "name" : "block",
            built ? "stack has an item" : "empty stack");
      if (built) setItem(sv.container, target, tmp);
      reinterpret_cast<void (*)(void *)>(dtor)(tmp);
    }
    return finish(stackMatches(itemAt(sv, target), gMove.w));
  }
  if (source < 0 || target < 0 || !setItem || !gItemStackCopy) return finish(false);
  gItemStackCopy(tmp, itemAt(sv, target));
  if (!safeRead(tmp, &vtable, 8) || !vtable || !safeRead(vtable, &dtor, 8) || !dtor) return finish(false);  // tmp leaks
  setItem(sv.container, target, itemAt(sv, source));
  setItem(sv.container, source, tmp);
  reinterpret_cast<void (*)(void *)>(dtor)(tmp);
  finish(stackMatches(itemAt(sv, target), gMove.w));
}

// Vanilla placement orients a block by where the player looks. The structure's own states win: once the server has the
// block, the tap thread's job swaps it for the permutation with those states, on the server thread (ServerPlayer tick).
struct OrientJob {
  BlockPos pos;
  uintptr_t type;
  std::string name, desc;                      // desc: the wanted states, for the log
  struct Want {
    std::string key, text;  // text: the value of a string state (isText)
    int value = 0;
    bool isText = false;
  };
  std::vector<Want> want;
  std::string unsupported;  // states of a type that cannot be resolved (lists, floats...)
  int ticks = 0;
  bool verifyOnly = false;  // block entity placed with a chosen face: only log whether the game's own facing matches
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

// BlockType+0x1B0 is the root of a std::map keyed by the BlockState's id (BlockState+8, a small registry index).
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

struct TagOut {  // non-trivial destructor: returned through x8 like the game's unique_ptr<Tag>
  void *p = nullptr;
  ~TagOut() {}
};
using StateToTagFn = TagOut (*)(void *state, int value);

// BlockState* by name from the game's registry; nullptr when unknown. Server thread only.
void *stateByName(const std::string &key) {
  static std::map<std::string, void *> cache;
  if (!gStateByName) return nullptr;
  if (const auto it = cache.find(key); it != cache.end()) return it->second;
  const HashedString name(key);
  uintptr_t st = untag(gStateByName(name));
  uint64_t gotHash = 0;
  if (!st || !safeRead(reinterpret_cast<void *>(st + 0x18), &gotHash, 8) || gotHash != name.hash) return nullptr;
  return cache[key] = reinterpret_cast<void *>(st);
}

// Ordinal of a string state's value: BlockState::toTag(ordinal) (vtable slot 0x10) yields a StringTag (id 8, string at +8).
bool stringOrdinal(void *state, uint32_t count, const std::string &text, int &out) {
  static std::map<std::pair<void *, std::string>, int> cache;
  if (const auto it = cache.find({state, text}); it != cache.end()) return (out = it->second) >= 0;
  uintptr_t vt = 0, fn = 0;
  if (!readPtr(state, vt) || !vt || !readPtr(reinterpret_cast<void *>(vt + 0x10), fn) || !fn) return false;
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    uintptr_t tag = untag(reinterpret_cast<StateToTagFn>(fn)(state, static_cast<int>(i)).p), tvt = 0, idFn = 0;
    if (!tag || !readPtr(reinterpret_cast<void *>(tag), tvt) || !readPtr(reinterpret_cast<void *>(tvt + 0x30), idFn) || !idFn ||
        reinterpret_cast<uint8_t (*)(void *)>(idFn)(reinterpret_cast<void *>(tag)) != 8)
      return false;
    if (*reinterpret_cast<const std::string *>(tag + 8) == text) return (cache[{state, text}] = out = static_cast<int>(i)) >= 0;
  }
  cache[{state, text}] = -1;
  return false;
}

// The permutation of `type` that carries the wanted states on top of the states of `block`.
struct Permutation {
  uintptr_t raw = 0;  // the pointer exactly as the type's table holds it, tag byte included
  uint32_t data = 0, index = 0;
  std::string error;  // empty when raw is valid
};

Permutation permutationFor(void *block, uintptr_t type, const std::vector<OrientJob::Want> &wants, const std::string &unsupported) {
  Permutation out;
  uint16_t data = 0;
  uintptr_t begin = 0, end = 0;
  if (!safeRead(static_cast<char *>(block) + 0x128, &data, 2) ||
      !readPtr(reinterpret_cast<void *>(type + off::BlockType_permutations), begin) ||
      !readPtr(reinterpret_cast<void *>(type + off::BlockType_permutations + 8), end) || end <= begin ||
      end - begin > 0x10000 * 8) {
    out.error = "cannot read the permutation table";
    return out;
  }
  const size_t count = (end - begin) / 8;
  uintptr_t current = 0;
  if (data >= count || !readPtr(reinterpret_cast<void *>(begin + data * 8), current) || current != untag(block)) {
    out.error = fmt::format("permutation table does not hold the block: data {} of {}", data, count);
    return out;
  }

  if (!unsupported.empty()) {
    out.error = "unknown state " + unsupported;
    return out;
  }
  uint32_t index = data;
  for (const auto &w : wants) {
    void *state = stateByName(w.key);
    uint64_t id = 0;
    StateDesc d{};
    if (!state || !safeRead(static_cast<char *>(state) + 8, &id, 8) || !findState(type, id, d)) {
      out.error = fmt::format("unknown state {}", w.key);
      return out;
    }
    int value = w.value;
    if (w.isText && !stringOrdinal(state, d.count, w.text, value)) {
      out.error = fmt::format("unknown value {}={}", w.key, w.text);
      return out;
    }
    if (value < 0 || static_cast<uint32_t>(value) >= d.count) {
      out.error = fmt::format("{}={} out of range {}", w.key, value, d.count);
      return out;
    }
    index = (index & ~d.mask) | (static_cast<uint32_t>(value) << (d.end + 1 - d.bits));
  }
  index &= 0xFFFF;
  uintptr_t wanted = 0, wantedType = 0;
  if (index >= count || !readPtr(reinterpret_cast<void *>(begin + index * 8), wanted) || !wanted ||
      !readPtr(reinterpret_cast<void *>(wanted + off::Block_blockType), wantedType) || wantedType != type ||
      !safeRead(reinterpret_cast<void *>(begin + index * 8), &out.raw, 8) || untag(reinterpret_cast<void *>(out.raw)) != wanted) {
    out.error = fmt::format("no permutation {} of {}", index, count);
    return out;
  }
  out.data = data;
  out.index = index;
  return out;
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
    if (j.verifyOnly) {
      trace("EasyPlace: native-facing {} {} via face -> {}{}", shortName(j.name), j.desc,
            std::string_view(status) == "ok" ? "ok" : "unchanged", note.empty() ? "" : " (" + note + ")");
      return 1;
    }
    trace("EasyPlace: orient {} {} -> {}{}", shortName(j.name), j.desc, status, note.empty() ? "" : " (" + note + ")");
    return 1;
  };
  const Permutation perm = permutationFor(block, j.type, j.want, j.unsupported);
  if (!perm.error.empty()) return done("failed", perm.error);
  const uint32_t data = perm.data, index = perm.index;
  if (index == data) return done("ok", "already matches");
  if (j.verifyOnly) return done("unchanged", fmt::format("data {} wanted {}", data, index));
  // The chunk's palette stores Block pointers and finds entries by comparing them whole, tag byte included: setBlock gets
  // the pointer exactly as the game holds it, never the untagged copy used for our own comparisons.
  const uintptr_t wantedRaw = perm.raw;
  if ((wantedRaw >> 56) != (reinterpret_cast<uintptr_t>(block) >> 56))
    return done("failed", "permutation pointer does not carry the same tag as the placed block");

  uintptr_t vtable = 0, slot = 0;
  if (!gSetBlock || !gMakeCtx || !readPtr(source, vtable) || !vtable ||
      !readPtr(reinterpret_cast<void *>(vtable + off::BlockSource_vtSetBlock), slot) ||
      slot != untag(reinterpret_cast<void *>(gSetBlock)))
    return done("failed", "BlockSource::setBlock is not where expected");
  const BlockChangeContext ctx = gMakeCtx(0);
  const bool set = reinterpret_cast<SetBlockFn>(slot)(source, &j.pos, reinterpret_cast<void *>(wantedRaw), 3, nullptr, &ctx);
  const bool now = reinterpret_cast<uintptr_t>(gGetBlock(source, j.pos)) == wantedRaw;
  return done(set && now ? "ok" : "failed",
              fmt::format("data {} -> {}", data, index));
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

// Player-direction block entities (chest, furnace, barrel...) get their facing from the placing player's yaw, which the
// server reads from the ServerPlayer. The client posts what the structure wants for the spot; the game's own
// getPlacementBlock then returns the permutation with that facing, so the game creates the block entity for the final block.
struct FacingJob {
  BlockPos pos;
  uintptr_t type;
  std::string name, desc;
  std::vector<OrientJob::Want> want;
  std::chrono::steady_clock::time_point expires;
};
std::mutex gFacingMu;
std::vector<FacingJob> gFacing;
std::atomic_int gFacingCount{0}, gFacingHooks{0};  // gFacingHooks: orient::placementHook bits that are installed
constexpr auto kFacingLife = std::chrono::seconds(1);
std::array<std::atomic<PlacementFn>, 3> gPlacementOrig{};
std::array<void *, 3> gPlacementTarget{};

void facingLog(const FacingJob &j, const std::string &status) {
  trace("EasyPlace: native-facing {} {} via server-placement -> {}", shortName(j.name), j.desc, status);
}

// Callers hold gFacingMu.
void expireFacing() {
  const auto now = std::chrono::steady_clock::now();
  for (size_t i = 0; i < gFacing.size();) {
    if (gFacing[i].expires > now) {
      ++i;
      continue;
    }
    facingLog(gFacing[i], "failed(the placement never reached the server)");
    gFacing.erase(gFacing.begin() + i);
  }
  gFacingCount.store(static_cast<int>(gFacing.size()), std::memory_order_release);
}

// Runs on the server thread inside getPlacementBlock; `result` is the block the game picked for the player.
const void *applyFacing(const void *result, void *actor, const BlockPos *pos) {
  if (!result || !pos) return result;
  std::lock_guard lock(gFacingMu);
  expireFacing();
  uintptr_t type = 0;
  if (!readPtr(reinterpret_cast<const char *>(untag(result)) + off::Block_blockType, type)) return result;
  for (size_t i = 0; i < gFacing.size(); ++i) {
    FacingJob &j = gFacing[i];
    if (j.pos.x != pos->x || j.pos.y != pos->y || j.pos.z != pos->z || j.type != type) continue;
    // The client's own prediction runs the same code for the LocalPlayer: leave the job for the server's placement.
    if (className(actor) != "ServerPlayer") return result;
    const FacingJob job = std::move(j);
    gFacing.erase(gFacing.begin() + i);
    gFacingCount.store(static_cast<int>(gFacing.size()), std::memory_order_release);
    const Permutation perm = permutationFor(const_cast<void *>(result), job.type, job.want, {});
    if (!perm.error.empty()) {
      facingLog(job, "failed(" + perm.error + ")");
      return result;
    }
    if (perm.index == perm.data) {
      facingLog(job, "passthrough");
      return result;
    }
    if ((perm.raw >> 56) != (reinterpret_cast<uintptr_t>(result) >> 56)) {
      facingLog(job, "failed(permutation pointer does not carry the same tag as the game's block)");
      return result;
    }
    facingLog(job, "ok");
    return reinterpret_cast<const void *>(perm.raw);
  }
  return result;
}

template <size_t N>
const void *placementDetour(void *blockType, void *actor, const BlockPos *pos, uint8_t face, const void *click, int itemValue) {
  PlacementFn orig;
  while (!(orig = gPlacementOrig[N].load(std::memory_order_acquire))) std::this_thread::yield();
  const void *result = orig(blockType, actor, pos, face, click, itemValue);
  return gFacingCount.load(std::memory_order_acquire) > 0 ? applyFacing(result, actor, pos) : result;
}

uint64_t serverTickDetour(void *self, void *a1, void *a2, void *a3) {
  const uint64_t result = gServerTickOrig.load(std::memory_order_acquire)(self, a1, a2, a3);
  if (gMove.state.load(std::memory_order_acquire) == 1) runMoveJob(self);
  if (gOrientCount.load(std::memory_order_acquire) > 0) runOrientJobs(self);
  return result;
}

// Posts a move of `expected` from the main inventory to the hotbar. True while the move is in flight.
bool queueMove(const InventoryView &view, const Wanted &w) {
  if (!gServerTickOrig.load(std::memory_order_acquire) || !gItemStackCopy || !gItemStackHasItem) return false;
  if (gMoveFailedFor.load(std::memory_order_relaxed) == w.key()) return false;
  bool inMain = false;
  for (int slot = 9; slot < 36 && !inMain; ++slot) inMain = stackMatches(itemAt(view, slot), w);
  if (!inMain) return false;
  if (gMove.state.load(std::memory_order_acquire) == 1) return true;
  gMove.w = w;
  gMove.inject = false;
  gMove.block = nullptr;
  for (int slot = 0; slot < 36; ++slot) gMove.snapshot[slot] = slotType(view, slot);
  gMove.posted = std::chrono::steady_clock::now();
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
bool queueInject(const InventoryView &view, const Wanted &w) {
  const bool byName = !w.itemName.empty() && gItemStackFromName;
  if (!gServerTickOrig.load(std::memory_order_acquire) || !(gItemStackFromBlock || byName) || !gItemStackHasItem) return false;
  if (gMoveFailedFor.load(std::memory_order_relaxed) == w.key()) return false;
  for (int slot = 0; slot < 36; ++slot)
    if (stackMatches(itemAt(view, slot), w)) return false;  // already carried: nothing to conjure
  if (gMove.state.load(std::memory_order_acquire) == 1) return true;
  const void *block = w.type ? defaultBlock(w.type) : nullptr;
  if (!block && !byName) {
    gMoveFailedFor.store(w.key(), std::memory_order_relaxed);
    trace("EasyPlace: auto-pick {} via creative-inject -> failed", shortName(w.label()));
    return false;
  }
  gMove.w = w;
  gMove.inject = true;
  gMove.block = block;
  for (int slot = 0; slot < 36; ++slot) gMove.snapshot[slot] = slotType(view, slot);
  gMove.posted = std::chrono::steady_clock::now();
  gMove.state.store(1, std::memory_order_release);
  return true;
}

bool pickSource(const World &world, const Vec3f &eye, const Wanted &w, BlockPos &out) {
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
            !type || !w.type || untag(type) != untag(w.type))
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

bool autoPick(const Context &ctx, const Wanted &w) {
  constexpr std::string_view method = "BlockPickRequest";
  const auto failed = [&] {
    trace("EasyPlace: auto-pick {} via {} -> failed", shortName(w.label()), method);
    return false;
  };
  if (!gPickBlock || !gSelectSlot || !gItemStackHasItem) return failed();
  InventoryView view;
  if (!inventoryView(ctx.player, view)) return failed();
  if (queueMove(view, w)) return false;  // the server thread does it; the next tap finds the block in the hotbar
  if (!creativeMode(ctx.player)) return false;  // survival: no conjuring, "Hold X"
  if (queueInject(view, w)) return false;  // the next tap finds the block in the hotbar

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
      if (stackMatches(itemAt(view, slot), w)) {
        inMain = true;
        break;
      }
    // A survival item in the main inventory must be moved only into an empty hotbar slot.
    // With no such slot, the only safe native path is creative pick-block into the selected slot.
    if (inMain) return failed();
  } else {
    selected = empty;
  }

  if (w.itemName.size() > 7 && w.itemName.compare(w.itemName.size() - 7, 7, "_bucket") == 0) return failed();  // no pick-block for liquids
  BlockPos source{};
  if (!pickSource(ctx.world, ctx.eye, w, source)) return failed();
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
  const bool ok = stackMatches(itemAt(view, selected), w);
  trace("EasyPlace: auto-pick {} via {} -> {}", shortName(w.label()), method, ok ? "ok" : "failed");
  return ok;
}

// The held block must be the one the structure wants at `target`.
bool holdingRight(const Context &ctx, const Cell &target, uint8_t hand) {
  const std::string &want = ctx.h.structure->palette[expectedAt(*ctx.h.structure, ctx.h.origin, ctx.h.view, target)].name;
  const Wanted w = wantedFor(want);
  if (!w.type) {
    trace("EasyPlace unknown block: '{}'", want);
    say("Unknown block: " + shortName(want));
    return false;
  }
  const void *stack = gHandItem(ctx.player, hand);
  if (!stackMatches(stack, w) && hand == 0 && gAutoSelect.load(std::memory_order_relaxed) &&
      (selectFromHotbar(ctx.player, w) || autoPick(ctx, w)))
    stack = gHandItem(ctx.player, hand);
  if (!stackMatches(stack, w)) {
    say("Hold " + shortName(w.label()));
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

// Swapping the permutation of a block-entity block (barrel...) was the only setBlock the device ever ran before the server
// thread crashed saving that chunk (device log 19:46, see the handoff notes), so those blocks keep their vanilla state until
// the cause is confirmed on a device. Flip kOrientBlockEntities to test them.
constexpr bool kOrientBlockEntities = false;
bool hasBlockEntity(const std::string &n) {
  if (kOrientBlockEntities) return false;
  static constexpr std::string_view exact[] = {
      "barrel", "chest", "trapped_chest", "ender_chest", "furnace", "lit_furnace", "blast_furnace", "lit_blast_furnace",
      "smoker", "lit_smoker", "hopper", "dropper", "dispenser", "crafter", "brewing_stand", "lectern", "chiseled_bookshelf",
      "jukebox", "beacon", "enchanting_table", "bed", "command_block", "chain_command_block", "repeating_command_block",
      "decorated_pot", "daylight_detector", "conduit", "bell", "campfire", "soul_campfire", "end_portal", "end_gateway",
      "mob_spawner", "trial_spawner", "vault", "structure_block", "jigsaw", "sculk_sensor", "calibrated_sculk_sensor",
      "sculk_shrieker", "sculk_catalyst", "skull", "comparator", "unpowered_comparator", "powered_comparator", "cauldron"};
  for (const auto e : exact)
    if (n == e) return true;
  for (const std::string_view suffix : {"_sign", "_banner", "shulker_box", "_bed", "_head", "_skull", "_hanging_sign"})
    if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
  return false;
}

// The face to click at `target` so the game itself gives the block the structure's facing, or -1 (see orient::clickFace).
int nativeFace(const Context &ctx, const Cell &target) {
  const Structure &st = *ctx.h.structure;
  const int32_t idx = expectedAt(st, ctx.h.origin, ctx.h.view, target);
  if (idx < 0) return -1;
  const BlockState &bs = st.palette[idx];
  for (size_t i = 0; i < bs.states.items.size() && i < bs.states.keys.size(); ++i) {
    const nbt::Tag &t = bs.states.items[i];
    if (bs.states.keys[i] != "facing_direction" ||
        (t.type != nbt::Type::Byte && t.type != nbt::Type::Short && t.type != nbt::Type::Int))
      continue;
    orient::StateValue v{static_cast<int>(t.i), {}, false};
    orient::transformState(bs.name, bs.states.keys[i], v, ctx.h.view.rotation, ctx.h.view.mirror);
    return orient::clickFace(bs.name, bs.states.keys[i], v);
  }
  return -1;
}

// The placement-time facing the structure wants for `cell`, or false when the block is not one the installed placement hooks cover.
bool buildFacing(const Context &ctx, const Cell &cell, FacingJob &out) {
  const Structure &st = *ctx.h.structure;
  const int32_t idx = expectedAt(st, ctx.h.origin, ctx.h.view, cell);
  if (idx < 0) return false;
  const BlockState &bs = st.palette[idx];
  const int hook = orient::placementHook(bs.name);
  if (!hook || !(gFacingHooks.load(std::memory_order_relaxed) & hook) || nativeFace(ctx, cell) >= 0) return false;
  out = {{cell[0], cell[1], cell[2]}, untag(blockTypeOf(bs.name)), bs.name, {}, {}, {}};
  for (size_t i = 0; i < bs.states.items.size() && i < bs.states.keys.size(); ++i) {
    const nbt::Tag &t = bs.states.items[i];
    const std::string &key = bs.states.keys[i];
    if (!orient::isDirectionKey(key)) continue;
    orient::StateValue v{static_cast<int>(t.i), t.s, t.type == nbt::Type::String};
    if (!v.isText && t.type != nbt::Type::Byte && t.type != nbt::Type::Short && t.type != nbt::Type::Int) continue;
    orient::transformState(bs.name, key, v, ctx.h.view.rotation, ctx.h.view.mirror);
    out.want.push_back({key, v.s, v.i, v.isText});
    out.desc += fmt::format("{}{}={}", out.desc.empty() ? "" : ",", key, v.isText ? v.s : std::to_string(v.i));
  }
  return out.type && !out.want.empty();
}

// Called before the placement is sent, so the server finds the job when it places the block. True if one was posted.
bool postFacing(const Context &ctx, const Cell &cell) {
  FacingJob job;
  if (!buildFacing(ctx, cell, job)) return false;
  job.expires = std::chrono::steady_clock::now() + kFacingLife;
  std::lock_guard lock(gFacingMu);
  expireFacing();
  if (gFacing.size() >= 64) gFacing.erase(gFacing.begin());
  gFacing.push_back(std::move(job));
  gFacingCount.store(static_cast<int>(gFacing.size()), std::memory_order_release);
  return true;
}

// The game refused the placement: the job must not linger and hit a later block at the same spot.
void dropFacing(const Cell &cell) {
  std::lock_guard lock(gFacingMu);
  std::erase_if(gFacing, [&](const FacingJob &j) { return j.pos.x == cell[0] && j.pos.y == cell[1] && j.pos.z == cell[2]; });
  gFacingCount.store(static_cast<int>(gFacing.size()), std::memory_order_release);
}

// Posts the structure's states for the block just placed at `cell`. Doors are two blocks: the cell above is posted too.
void queueOrient(const Context &ctx, const Cell &cell, bool upperDone = false) {
  if (!gSetBlock || !gMakeCtx || !gStateByName || !gServerTickOrig.load(std::memory_order_acquire)) return;
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
      orient::StateValue v{static_cast<int>(t.i), {}, false};
      orient::transformState(bs.name, key, v, ctx.h.view.rotation, ctx.h.view.mirror);
      job.want.push_back({key, {}, v.i, false});
      job.desc += fmt::format("{}{}={}", job.desc.empty() ? "" : ",", key, v.i);
      if (key == "upper_block_bit") upper = v.i != 0;
    } else if (t.type == nbt::Type::String) {
      orient::StateValue v{0, t.s, true};
      orient::transformState(bs.name, key, v, ctx.h.view.rotation, ctx.h.view.mirror);
      job.want.push_back({key, v.s, 0, true});
      job.desc += fmt::format("{}{}={}", job.desc.empty() ? "" : ",", key, v.s);
    } else {
      job.unsupported += (job.unsupported.empty() ? "" : ",") + key;
      job.desc += fmt::format("{}{}=?", job.desc.empty() ? "" : ",", key);
    }
  }
  if (hasBlockEntity(shortName(bs.name))) {
    if (nativeFace(ctx, cell) < 0) {
      FacingJob facing;
      if (buildFacing(ctx, cell, facing)) return;  // chosen at placement time; the placement hook logs the result
      trace("EasyPlace: orient {} {} -> skipped (block entity)", shortName(bs.name), job.desc);
      return;
    }
    // The placement clicked the face that makes the game pick this facing; only check, never setBlock.
    job.verifyOnly = true;
    std::erase_if(job.want, [](const OrientJob::Want &w) { return w.key != "facing_direction"; });
    job.desc = fmt::format("facing_direction={}", job.want.empty() ? 0 : job.want[0].value);
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
    if (wantsBucket(ctx, target) && !holdingRight(ctx, target, hand)) {
      trace("EasyPlace build: no bucket in hand for {} {} {}", target[0], target[1], target[2]);
      return false;
    }
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

  const int nativeClick = nativeFace(ctx, target);
  const bool facing = postFacing(ctx, target);
  if (target == normal && (nativeClick < 0 || nativeClick == face)) {
    tInside = true;
    const bool placed = orig(gm, pos, face, hand, flag);
    tInside = false;
    if (placed) queueOrient(ctx, target);
    else if (facing) dropFacing(target);
    return afterPlace(ctx, placed);
  }
  const bool placed = placeAt(gm, ctx.player, ctx.world, target, hand, flag, nativeClick);
  if (placed) queueOrient(ctx, target);
  else if (facing) dropFacing(target);
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
    if (wantsBucket(ctx, target) && !holdingRight(ctx, target, 0)) {
      trace("EasyPlace air: no bucket in hand for {} {} {}", target[0], target[1], target[2]);
      return false;
    }
    const void *held = gHandItem(ctx.player, 0);  // auto-select may have switched the slot since `stack` was taken
    const bool used = orig(gm, held ? const_cast<void *>(held) : stack, flag);
    if (used) hologram::checkPlaced(target[0], target[1], target[2]);
    return used;
  }
  if (!holdingRight(ctx, target, 0)) {
    trace("EasyPlace air: wrong block in hand for {} {} {}", target[0], target[1], target[2]);
    return false;
  }
  trace("EasyPlace air: placing at {} {} {}", target[0], target[1], target[2]);
  const bool facing = postFacing(ctx, target);
  const bool placed = placeAt(gm, ctx.player, ctx.world, target, 0, false, nativeFace(ctx, target));
  if (placed) {
    hologram::checkPlaced(target[0], target[1], target[2]);
    queueOrient(ctx, target);
  } else if (facing) {
    dropFacing(target);
  }
  return afterPlace(ctx, placed);
}

// Optional: without a hook, yaw-driven block entities keep the game's own facing (logged as skipped).
template <size_t N> void hookPlacement(pl::log::Logger &log, const char *name, const char *signature, uintptr_t offset, int bit) {
  const auto found = pl::memory::resolveSignatures(std::vector<std::string>{signature}, "libminecraftpe.so");
  const auto it = found.find(signature);
  if (it == found.end() || !it->second) {
    log.warn("BE-Matica: {} not found, native facing of that block family disabled", name);
    return;
  }
  void *target = reinterpret_cast<void *>(it->second + offset), *origSlot = nullptr;
  if (pl::memory::hook(target, reinterpret_cast<void *>(&placementDetour<N>), &origSlot) != 0 || !origSlot) {
    log.warn("BE-Matica: could not hook {}, native facing of that block family disabled", name);
    return;
  }
  gPlacementOrig[N].store(reinterpret_cast<PlacementFn>(origSlot), std::memory_order_release);
  gPlacementTarget[N] = target;
  gFacingHooks.fetch_or(bit);
  log.info("BE-Matica: {} at {:#x}", name, it->second + offset);
}

void installFacingHooks(pl::log::Logger &log) {
  if (!gStateByName) return;
  hookPlacement<0>(log, "PlacementBase", sig::PlacementBase, 0, 1);
  hookPlacement<1>(log, "PlacementBarrel", sig::PlacementBarrel, sig::PlacementBarrelOffset, 2);
  hookPlacement<2>(log, "PlacementDispenser", sig::PlacementDispenser, 0, 4);
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
      {"ItemStackFromName", sig::ItemStackFromName, reinterpret_cast<void **>(&gItemStackFromName)},
      {"ServerPlayerTick", sig::ServerPlayerTick, &serverTick},
      {"HandItem", sig::HandItem, reinterpret_cast<void **>(&gHandItem)},
      {"BlockRegistry", sig::BlockRegistry, reinterpret_cast<void **>(&gRegistry)},
      {"BlockLookupByName", sig::BlockLookupByName, reinterpret_cast<void **>(&gLookup)},
      {"BlockSourceSetBlock", sig::BlockSourceSetBlock, reinterpret_cast<void **>(&gSetBlock)},
      {"BlockChangeContext", sig::BlockChangeContext, reinterpret_cast<void **>(&gMakeCtx)},
      {"BlockStateByName", sig::BlockStateByName, reinterpret_cast<void **>(&gStateByName)},
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
                          w.out == reinterpret_cast<void **>(&gItemStackFromBlock) ||
                          w.out == reinterpret_cast<void **>(&gItemStackFromName) || w.out == &serverTick ||
                          w.out == reinterpret_cast<void **>(&gSetBlock) || w.out == reinterpret_cast<void **>(&gMakeCtx) ||
                          w.out == reinterpret_cast<void **>(&gStateByName);
    if ((it == found.end() || !it->second) && optional) {
      log.warn("BE-Matica: {} not found, {} disabled", w.name,
               w.out == reinterpret_cast<void **>(&gSelectSlot) ? "auto-select"
               : w.out == reinterpret_cast<void **>(&gSetBlock) || w.out == reinterpret_cast<void **>(&gMakeCtx) ||
                       w.out == reinterpret_cast<void **>(&gStateByName)
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
  installFacingHooks(log);
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
  const std::array<void *, 3> detours{reinterpret_cast<void *>(&placementDetour<0>), reinterpret_cast<void *>(&placementDetour<1>),
                                      reinterpret_cast<void *>(&placementDetour<2>)};
  for (size_t i = 0; i < gPlacementTarget.size(); ++i) {
    if (gPlacementTarget[i]) pl::memory::unhook(gPlacementTarget[i], detours[i]);
    gPlacementTarget[i] = nullptr;
  }
  gFacingHooks.store(0);
}

void setEnabled(bool enabled) { gEnabled = enabled; }
void setAirPlace(bool on) { gAirPlace = on; }
void setAutoSelect(bool on) { gAutoSelect = on; }

}  // namespace bematica::easyplace
