#pragma once

#include <cstdint>
#include <string>

// Minecraft internals BE-Matica relies on. Found for Minecraft Bedrock 1.26.60.4 (arm64).
// Signatures and offsets break on game updates: re-check all of them together.
namespace bematica::game {

namespace sig {
// LevelRenderer::renderLevel(ScreenContext&, FrameRenderObject&)
constexpr const char *RenderLevel = "FF 83 07 D1 EE AB 00 FD ED 33 16 6D EB 2B 17 6D E9 23 18 6D FD 7B 19 A9";
// RenderLevel minus its first 16 bytes, which another mod's inline hook may have overwritten.
constexpr const char *RenderLevelTail =
    "E9 23 18 6D FD 7B 19 A9 FC 67 1A A9 F8 5F 1B A9 F6 57 1C A9 F4 4F 1D A9 FD 43 06 91 57 D0 3B D5 F3 03 00 AA";
constexpr uintptr_t RenderLevelTailOffset = 16;
// Tessellator::begin(name, primitive mode, vertex count, build face data)
constexpr const char *TessellatorBegin = "FD 7B BC A9 F8 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 08 60 4A 39";
// Tessellator::color(r, g, b, a)
constexpr const char *TessellatorColor = "E8 6F A8 52 0C 50 49 39 04 01 27 1E 00 08 24 1E 21 08 24 1E 42 08 24 1E";
// Tessellator::vertex(x, y, z)
constexpr const char *TessellatorVertex =
    "FF C3 02 D1 EA 1B 00 FD E9 23 04 6D FD 7B 05 A9 FC 6F 06 A9 FA 67 07 A9 F8 5F 08 A9 F6 57 09 A9 F4 4F 0A A9 FD 43 "
    "01 91 5B D0 3B D5 68 17 40 F9";
// MeshHelpers::renderMeshImmediately(ScreenContext&, Tessellator&, const MaterialPtr&, ...)
constexpr const char *RenderMeshImmediately =
    "FD 7B BC A9 FC 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 FF C3 09 D1 57 D0 3B D5 F6 03 00 AA";

// Easy Place
// GameMode::buildBlock(const BlockPos& clicked, uchar face, uchar hand, bool)
constexpr const char *BuildBlock =
    "FF C3 04 D1 FD 7B 0D A9 FC 73 00 F9 FA 67 0F A9 F8 5F 10 A9 F6 57 11 A9 F4 4F 12 A9 FD 43 03 91 5A D0 3B D5 F7 "
    "03 04 2A";
// GameMode::baseUseItem(ItemStack&): a tap that hits no block (aiming at air)
constexpr const char *BaseUseItem =
    "FD 7B BC A9 FC 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 FF 83 0C D1 57 D0 3B D5 F4 03 00 AA";
// BlockSource::getBlock(const BlockPos&)
constexpr const char *BlockSourceGetBlock =
    "FF 03 01 D1 FD 7B 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 43 00 91 56 D0 3B D5 C8 16 40 F9 E8 07 00 F9 28 04 40 B9 09 84 "
    "C0 79 1F 01 09 6B ? ? ? ? 09 80 C0 79 F3 03 00 AA";
// Returns the BlockType an ItemStack places (two-register struct, first is the BlockType)
constexpr const char *ItemBlockType =
    "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 F3 03 00 AA ? ? ? ? ? ? ? ? E0 03 13 AA ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? 08 FD "
    "DF 08";
// Player's item in a hand (0 = main hand)
constexpr const char *HandItem = "28 1C 00 12 1F 05 00 71 ? ? ? ? 08 00 40 F9 01 39 41 F9 20 00 1F D6";
// PlayerInventory::selectSlot(int slot, ContainerID): switches the held hotbar slot like a tap on the hotbar (0 = inventory)
constexpr const char *SelectSlot =
    "FD 7B BB A9 FC 67 01 A9 F8 5F 02 A9 F6 57 03 A9 F4 4F 04 A9 FD 03 00 91 FF C3 09 D1 56 D0 3B D5 C8 16 40 F9 5F 1C 00 "
    "72 A8 83 1F F8 60 00 00 54";
// ItemStack::hasItem: distinguishes an empty stack from a non-block item.
constexpr const char *ItemStackHasItem =
    "08 04 40 F9 48 01 00 B4 00 01 40 F9 E0 00 00 B4 FD 7B BF A9 FD 03 00 91 ? ? ? ? 1F 00 00 F1 E0 07 9F 1A "
    "FD 7B C1 A8 C0 03 5F D6";
// Player::pick(const HitResult&, bool): builds the native picked ItemStack and sends BlockPickRequest.
constexpr const char *PickBlock =
    "FF C3 02 D1 FD 7B 07 A9 F7 43 00 F9 F6 57 09 A9 F4 4F 0A A9 FD C3 01 91 57 D0 3B D5 E8 16 40 F9 A8 83 1F F8 "
    "28 18 40 B9 08 11 00 51 1F 0D 00 31 ? ? ? ? F4 03 02 2A F5 03 01 AA F6 03 00 AA ? ? ? ? A8 1A 40 B9 F3 03 00 AA 1F 05 00 71";
// ItemStack::ItemStack(const ItemStack&): copy constructor (0x98-byte object, vtable first)
constexpr const char *ItemStackCopy =
    "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 F3 03 00 AA 08 00 00 94 28 B1 01 D0 08 E1 3F 91 68 06 08 F8 E0 03 13 AA F3 0B "
    "40 F9 FD 7B C2 A8 29 62 E4 17 FF 43 01 D1";
// ItemStack::ItemStack(const Block&, int count, const CompoundTag*): constructs in place (0x98 bytes). The bl to the
// ItemStackBase ctor is kept exact: it is what tells this ctor from its sibling ItemStack ctors.
constexpr const char *ItemStackFromBlock =
    "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 F3 03 00 AA AD F7 FF 97 ? ? ? ? ? ? ? ? 68 06 08 F8 E0 03 13 AA F3 0B 40 F9 FD "
    "7B C2 A8 4D 62 E4 17";
// ServerPlayer's override of Player vtable slot 0xC8; runs on the server thread, `this` is the ServerPlayer
constexpr const char *ServerPlayerTick =
    "FF 83 03 D1 FD 7B 09 A9 F9 53 00 F9 F8 5F 0B A9 F6 57 0C A9 F4 4F 0D A9 FD 43 02 91 57 D0 3B D5 F3 03 00 AA 00 20 "
    "00 91 E8 16 40 F9";
// Player::getPlayerGameType(): reads ActorGameTypeComponent from the ECS (-1 if absent); 5 (default) is resolved
// through the Level's vtable slot 0x468. 0 survival, 1 creative, 2 adventure, 6 spectator.
constexpr const char *PlayerGameType =
    "FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 F3 03 00 AA 00 20 00 91 ? ? ? ? 1F 14 00 71 01 01 00 54 E0 03 13 AA ? ? ? ? 08 "
    "00 40 F9 01 35 42 F9 F3 0B 40 F9 FD 7B C2 A8 20 00 1F D6";
// Returns the global block registry (the getter sits right before BlockLookupByName)
constexpr const char *BlockRegistry = "? ? ? ? ? ? ? ? C0 03 5F D6 E2 03 1F 2A E3 03 1F 2A E4 03 1F 2A";
// Looks up a BlockType by its full name (HashedString); old names like minecraft:grass resolve too
constexpr const char *BlockLookupByName = "E2 03 1F 2A E3 03 1F 2A E4 03 1F 2A ? ? ? ? FF 03 02 D1 FD 7B 03 A9";

// Textured hologram (experimental)
// BlockTessellator::BlockTessellator(BlockSource*) — the object is kBlockTessellatorSize bytes
constexpr const char *BlockTessellatorCtor =
    "FF 43 03 D1 FD 7B 07 A9 FC 6F 08 A9 FA 67 09 A9 F8 5F 0A A9 F6 57 0B A9 F4 4F 0C A9 FD C3 01 91 5B D0 3B D5 F3 03 00 "
    "AA 16 B0 05 91";
// BlockTessellator::appendTessellatedBlock(Tessellator&, const Block&): one block at the tessellator's offset
constexpr const char *AppendTessellatedBlock =
    "FF 83 07 D1 E9 23 17 6D FD 7B 18 A9 FC 6F 19 A9 FA 67 1A A9 F8 5F 1B A9 F6 57 1C A9 F4 4F 1D A9 FD 03 06 91 5C D0 3B D5";
// Material group lookup by name, returning a MaterialPtr
constexpr const char *MaterialGroupGet = "E2 03 01 AA 01 80 00 91 E0 03 08 AA ? ? ? ? FF C3 01 D1 FD 7B 01 A9";
// mce::Mesh render: (mesh, context, material, start, count, textures, ...)
constexpr const char *MeshRender =
    "FF C3 07 D1 FD 7B 1A A9 FC DB 00 F9 F8 5F 1C A9 F6 57 1D A9 F4 4F 1E A9 FD 83 06 91 00 E4 00 6F 54 D0 3B D5 F5 43 01 91";
// Code that loads the global material group (adrp + add at MaterialGroupRefAdrp) to create "entity_alphablend"
constexpr const char *MaterialGroupRef =
    "? ? ? ? ? ? ? ? A0 C3 00 D1 ? ? ? ? ? ? ? ? ? ? ? ? A8 03 01 D1 A1 C3 00 D1 ? ? ? ? BB 67 7C A9 ? ? ? ? 21 23 00 91 "
    "20 00 80 52 ? ? ? ? 78 FE 40 F9";
constexpr uintptr_t MaterialGroupRefAdrp = 0x10;
// Tessellator::end(Tessellator&, mode, name, nameLen, flag) -> mce::Mesh through x8; "Untagged Tessellator Mesh" callers pass mode 0, flag 0
constexpr const char *TessellatorEnd =
    "FD 7B BB A9 FC 67 01 A9 F8 5F 02 A9 F6 57 03 A9 F4 4F 04 A9 FD 03 00 91 FF 03 0A D1 58 D0 3B D5";
// mce::Mesh::~Mesh()
constexpr const char *MeshDtor = "FD 7B BE A9 F4 4F 01 A9 FD 03 00 91 F3 03 00 AA ? ? ? ? 60 26 41 F9";
// mce::Mesh::isValid() (has at least one draw range)
constexpr const char *MeshValid = "09 A0 45 A9 08 01 09 CB 69 55 95 52 08 FD 42 D3 49 55 B5 72 08 7D 09 1B";
// Builds a texture-variant holder (variant at +0x18, index at +0x30) by copying a variant through the game's own visitor
constexpr const char *TextureVariantWrap =
    "FF 03 01 D1 FD 7B 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 43 00 91 55 D0 3B D5 F4 03 00 AA A8 16 40 F9 E8 07 00 F9 08 00 "
    "80 12 76 18 40 B9 01 08 00 A9";
}  // namespace sig

namespace off {
constexpr unsigned LevelRenderer_levelRendererPlayer = 0x430;
constexpr unsigned LevelRendererPlayer_cameraPos = 0x620;          // Vec3, world position of the camera
constexpr unsigned LevelRendererPlayer_selectionOverlayMat = 0x1028;  // MaterialPtr, translucent faces
constexpr unsigned LevelRendererPlayer_selectionBoxMat = 0x1088;      // MaterialPtr, lines
constexpr unsigned ScreenContext_colorHolder = 0x30;                // float[4] tint applied to meshes
constexpr unsigned ScreenContext_tessellator = 0xB0;
constexpr unsigned Tessellator_postTransformOffset = 0x1CC;         // Vec3
constexpr unsigned GameMode_player = 0x8;
constexpr unsigned GameMode_hasAltHit = 0x41;   // bool: the hit position below is in use
constexpr unsigned GameMode_altHitPos = 0x6C;   // Vec3
constexpr unsigned Level_vtGetHitResult = 0xA80; // vtable byte offset
constexpr unsigned HitResult_rayStart = 0x0, HitResult_rayDir = 0xC;  // Vec3 each: the player's aim ray
constexpr unsigned HitResult_type = 0x18, HitResult_face = 0x1C, HitResult_pos = 0x20, HitResult_hitPos = 0x2C;
constexpr unsigned Block_blockType = 0x68;
constexpr unsigned PlayerInventory_selectedSlot = 0x10;
constexpr unsigned PlayerInventory_container = 0xB8;  // selectSlot reads it for the slot count and getItem
constexpr unsigned Container_vtGetItem = 0x40;  // vtable byte offset: ItemStack* getItem(int slot)
constexpr unsigned Container_vtSetItem = 0x68;  // vtable byte offset: setItem(int slot, const ItemStack&); Inventory's is a thunk to setItemWithForceBalance
constexpr size_t ItemStack_size = 0x98;
// Textured hologram (1.26.60.4, static RE; see knowledge note 10)
constexpr unsigned LevelRenderer_clientInstance = 0x3D8;   // RenderLevel: ldr x22,[x0,#0x3d8]; it is ClientInstance (vtable slot 0x278 / BaseActorRenderContext ctor arg)
constexpr unsigned LevelRenderer_atlasTexture = 0x1B8;     // TexturePtr "atlas.terrain" (written in the LevelRenderer ctor at lib+0xb7936f0)
constexpr unsigned ClientInstance_blockTessellator = 0x618; // game-owned BlockTessellator(nullptr BlockSource), 0x47960 bytes
constexpr unsigned ClientInstance_itemInHandRenderer = 0x640;
constexpr unsigned ItemInHandRenderer_matOpaqueBlock = 0x1A0;       // MaterialPtr (16 bytes): "opaque_block"
constexpr unsigned ItemInHandRenderer_matOpaqueBlockColor = 0x1B0;  // "opaque_block_color"
constexpr unsigned ItemInHandRenderer_matAlphaBlock = 0x1C0;        // "entity_alphatest"
constexpr unsigned ItemInHandRenderer_matBlendBlock = 0x1E0;        // "entity_alphablend"
constexpr unsigned ItemInHandRenderer_matAlphaColoredBlock = 0x200; // "item_in_hand_entity_alphatest_color"
constexpr unsigned ItemInHandRenderer_matAlphaOneSidedBlock = 0x220;        // "entity_alphatest_one_sided"
constexpr unsigned ItemInHandRenderer_matAlphaOneSidedColoredBlock = 0x230; // "item_in_hand_entity_alphatest_one_sided_color"
constexpr unsigned Mesh_maxSize = 0x270;  // renderMeshImmediately keeps a Mesh in a 0x270-byte frame; reads fields up to +0x250
}  // namespace off

enum Primitive : int { QuadList = 1, LineList = 4 };

using RenderLevelFn = void (*)(void *self, void *screenContext, void *frameRenderObject);
using TessBeginFn = void (*)(void *tess, void *name, int mode, int vertexCount, int buildFaceData);
using TessColorFn = void (*)(void *tess, float r, float g, float b, float a);
using TessVertexFn = void (*)(void *tess, float x, float y, float z);
using RenderMeshFn = void (*)(void *screenContext, void *tess, void *material, char *scratch);

struct BlockPos {
  int x, y, z;
};
struct BlockTypeRef {
  void *blockType;
  uint64_t extra;
};
// Minecraft's HashedString: FNV-1a 64 hash, the string, and a lookup cache pointer.
struct HashedString {
  uint64_t hash = 0;
  std::string str;
  const HashedString *lastMatch = nullptr;
  explicit HashedString(std::string s) : str(std::move(s)) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (unsigned char c : str) h = (h * 0x100000001B3ULL) ^ c;
    hash = str.empty() ? 0 : h;
  }
};

using BuildBlockFn = bool (*)(void *gameMode, const BlockPos *pos, uint8_t face, uint8_t hand, bool flag);
using UseItemFn = bool (*)(void *gameMode, void *itemStack, uint32_t flag);
using GetBlockFn = void *(*)(void *blockSource, const BlockPos &pos);
using ItemBlockTypeFn = BlockTypeRef (*)(const void *itemStack);
using SelectSlotFn = bool (*)(void *playerInventory, int slot, uint8_t containerId);
using ItemStackHasItemFn = bool (*)(const void *itemStack);
using ItemStackCopyFn = void *(*)(void *dst, const void *src);
using ItemStackFromBlockFn = void *(*)(void *dst, const void *block, int count, const void *tag);
using ServerTickFn = uint64_t (*)(void *serverPlayer, void *a1, void *a2, void *a3);
using SetItemFn = void (*)(void *container, int slot, const void *itemStack);
using PlayerGameTypeFn = int (*)(void *player);
using PickBlockFn = void (*)(void *player, const void *hitResult, bool withData);
using HandItemFn = const void *(*)(void *player, uint8_t hand);
using BlockRegistryFn = void *(*)();
using BlockLookupFn = void *(*)(void *registry, const HashedString *name);
using GetHitResultFn = void *(*)(void *level);

// The game allocates 0x47960 bytes (`mov w0,#0x7960; movk w0,#4,lsl #16` before every ctor call); 0x7960 overflowed the heap.
constexpr size_t kBlockTessellatorSize = 0x47960;
// shared_ptr-like handle; the user-provided destructor makes the ABI return it through x8 like the game does.
// Deliberately never released: materials live for the whole session.
struct MaterialPtr {
  void *ptr = nullptr;
  void *ctrl = nullptr;
  ~MaterialPtr() {}
};
using BlockTessCtorFn = void *(*)(void *self, void *blockSource);
using AppendBlockFn = void (*)(void *blockTessellator, void *tess, const void *block);
// Takes a HashedString (hash first, then the name), like block lookups.
using MaterialGetFn = MaterialPtr (*)(void *group, const HashedString &name);

using MeshRenderFn = void (*)(void *mesh, void *context, void *material, uint64_t a3, uint64_t a4, void *textures,
                              void *a6, void *a7);

}  // namespace bematica::game
