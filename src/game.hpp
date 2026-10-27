#pragma once

#include <cstdint>

// Minecraft internals the hologram relies on. Found for Minecraft Bedrock 1.26.52.3 (arm64).
// Signatures and offsets break on game updates: re-check all of them together.
namespace bematica::game {

namespace sig {
// LevelRenderer::renderLevel(ScreenContext&, FrameRenderObject&)
constexpr const char *RenderLevel =
    "EE 0F 16 FC ED 33 01 6D EB 2B 02 6D E9 23 03 6D FD 7B 04 A9 FC 6F 05 A9 FA 67 06 A9 F8 5F 07 A9 F6 57 08 A9 F4 4F "
    "09 A9 FD 03 01 91 FF 03 06 D1";
// Tessellator::begin(name, primitive mode, vertex count, build face data)
constexpr const char *TessellatorBegin = "FD 7B BC A9 F8 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 08 60 4A 39";
// Tessellator::color(r, g, b, a)
constexpr const char *TessellatorColor = "E8 6F A8 52 0C 50 49 39 04 01 27 1E 00 08 24 1E 21 08 24 1E 42 08 24 1E";
// Tessellator::vertex(x, y, z)
constexpr const char *TessellatorVertex =
    "FF C3 02 D1 EA 1B 00 FD E9 23 04 6D FD 7B 05 A9 FC 6F 06 A9 FA 67 07 A9 F8 5F 08 A9 F6 57 09 A9 F4 4F 0A A9 FD 43 "
    "01 91 5A D0 3B D5 48 17 40 F9";
// MeshHelpers::renderMeshImmediately(ScreenContext&, Tessellator&, const MaterialPtr&, ...)
constexpr const char *RenderMeshImmediately =
    "FD 7B BC A9 FC 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 03 00 91 FF 03 0A D1 57 D0 3B D5 F6 03 00 AA";
}  // namespace sig

namespace off {
constexpr unsigned LevelRenderer_levelRendererPlayer = 0x420;
constexpr unsigned LevelRendererPlayer_cameraPos = 0x628;          // Vec3, world position of the camera
constexpr unsigned LevelRendererPlayer_selectionOverlayMat = 0x1030;  // MaterialPtr, translucent faces
constexpr unsigned LevelRendererPlayer_selectionBoxMat = 0x1090;      // MaterialPtr, lines
constexpr unsigned ScreenContext_colorHolder = 0x30;                // float[4] tint applied to meshes
constexpr unsigned ScreenContext_tessellator = 0xB8;
constexpr unsigned Tessellator_postTransformOffset = 0x1CC;         // Vec3
}  // namespace off

enum Primitive : int { QuadList = 1, LineList = 4 };

using RenderLevelFn = void (*)(void *self, void *screenContext, void *frameRenderObject);
using TessBeginFn = void (*)(void *tess, void *name, int mode, int vertexCount, int buildFaceData);
using TessColorFn = void (*)(void *tess, float r, float g, float b, float a);
using TessVertexFn = void (*)(void *tess, float x, float y, float z);
using RenderMeshFn = void (*)(void *screenContext, void *tess, void *material, char *scratch);

}  // namespace bematica::game
