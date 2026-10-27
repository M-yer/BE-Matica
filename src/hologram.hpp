#pragma once

#include "mcstructure.hpp"

#include <array>
#include <memory>
#include <string>

namespace pl::log {
class Logger;
}

namespace bematica::hologram {

// Finds the game functions and hooks world rendering. Returns false (with a reason) if this
// Minecraft version isn't supported; the rest of the mod keeps working without the hologram.
bool install(pl::log::Logger &log, std::string &error);
void uninstall();

// Shows `s` in the world. If `atPlayer` is set, it is placed at the player's feet on the next frame.
void show(std::shared_ptr<const Structure> s, bool atPlayer);
void moveToPlayer();
void setVisible(bool visible);
void setOpacity(float alpha);  // 0..1

}  // namespace bematica::hologram
