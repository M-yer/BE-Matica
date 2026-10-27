#pragma once

#include "mcstructure.hpp"
#include "mesh.hpp"

#include <array>
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

// Shows `s` in the world. If `atPlayer` is set, it is placed at the player's feet on the next frame.
void show(std::shared_ptr<const Structure> s, bool atPlayer);
void moveToPlayer();
void nudge(int dx, int dy, int dz);
void setView(const View &view);
void setVisible(bool visible);
void setOpacity(float alpha);  // 0..1

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
