#pragma once

#include "mcstructure.hpp"
#include "mesh.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace pl::log {
class Logger;
}

namespace bematica::hologram {

// Finds the game functions and hooks world rendering on a background thread: scanning the game
// takes seconds, and blocking the loading screen that long makes Android kill the app.
// `done` reports success, or why this Minecraft version isn't supported.
void installAsync(pl::log::Logger &log, std::function<void(bool ok, const std::string &error)> done);
void uninstall();

// The first "minecraft:..." string field of a game object (BlockType, Item), found by scanning its first 0x200 bytes the
// first time; `offset` keeps where (one per object kind, -1 = not found yet). "" if none.
std::string nameField(const void *obj, std::atomic<int> &offset);

// Shows `s` in the world. If `atPlayer` is set, it is placed at the player's feet on the next frame.
void show(std::shared_ptr<const Structure> s, bool atPlayer);
void moveToPlayer();
void setOrigin(std::array<int, 3> origin);  // world position of the structure's minimum corner
// Called on the render thread whenever the structure's origin changes (moves, or placed at the player).
void setOriginListener(std::function<void(std::array<int, 3> origin)> listener);
void nudge(int dx, int dy, int dz);
// Moves along the player's view, snapped to X or Z: `forward` away from the player, `right` to their right.
void nudgeView(int forward, int right);
void setView(const View &view);
void setVisible(bool visible);
void setOpacity(float alpha);  // 0..1
void setOutlines(bool on);     // light blue edges on every ghost block
void setFill(bool on);         // tinted faces on every ghost block
// A block was just placed at this world position: drop its ghost if it now matches the structure.
void checkPlaced(int x, int y, int z);
// Called on the render thread whenever the count of correctly built blocks (of those shown) changes.
void setProgressListener(std::function<void(size_t built, size_t total)> listener);

// What the player is looking at, for the look-at card. `want` is the structure's block at that cell when `inside`.
struct LookInfo {
  bool valid = false;  // false when nothing is under the crosshair, or it can't be read
  std::string name;    // display name of the block under the crosshair ("Air" for air)
  std::string want;
  bool inside = false, match = false;
  std::string note;  // replaces the "Wants" line: why the ghost block under the crosshair isn't placed
};
// Called on the render thread when the block under the crosshair changes. Null turns the reads off.
void setLookListener(std::function<void(const LookInfo &info)> listener);

// What's shown right now, for Easy Place. `structure` is null when nothing is shown.
struct Snapshot {
  std::shared_ptr<const Structure> structure;
  std::array<int, 3> origin{};
  View view;
  Vec3f camera{};  // last rendered camera position
  bool haveCamera = false;
};
Snapshot snapshot();

}  // namespace bematica::hologram
