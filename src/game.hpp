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
// Returns the global block registry (the getter sits right before BlockLookupByName)
constexpr const char *BlockRegistry = "? ? ? ? ? ? ? ? C0 03 5F D6 E2 03 1F 2A E3 03 1F 2A E4 03 1F 2A";
// Looks up a BlockType by its full name (HashedString); old names like minecraft:grass resolve too
constexpr const char *BlockLookupByName = "E2 03 1F 2A E3 03 1F 2A E4 03 1F 2A ? ? ? ? FF 03 02 D1 FD 7B 03 A9";
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
using HandItemFn = const void *(*)(void *player, uint8_t hand);
using BlockRegistryFn = void *(*)();
using BlockLookupFn = void *(*)(void *registry, const HashedString *name);
using GetHitResultFn = void *(*)(void *level);

}  // namespace bematica::game
