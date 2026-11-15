#pragma once

#include "mesh.hpp"

#include <functional>
#include <string>

namespace pl::log {
class Logger;
}

// Easy Place: a block placement aimed at or through the hologram goes into the first unbuilt
// ghost block along the player's view, and only if the held block is the one the structure wants.
namespace bematica::easyplace {

// Hooks block placement on a background thread (see hologram::installAsync for why).
// `message` shows short feedback to the player, e.g. which block to hold.
// `layerDone` fires when Easy Place fills the last block of the shown single layer.
void installAsync(pl::log::Logger &log, std::function<void(const std::string &)> message,
                  std::function<void()> layerDone, std::function<void(bool ok, const std::string &error)> done);
void uninstall();
void setEnabled(bool enabled);
// Air build: with nothing next to a ghost block to click against, place it in mid-air anyway.
void setAirPlace(bool on);
// Auto-select: when the held block is wrong, switch to the hotbar slot that has the wanted one.
void setAutoSelect(bool on);
// The next block tap places nothing: `done` gets the full name of the item in the player's hand instead
// ("minecraft:cobblestone", or "" for an empty hand). Works whether Easy Place is on or not.
// Printer: holding the place button fills the nearest ghost blocks in reach that the player holds or has in the hotbar.
// Runs in singleplayer / a world this device hosts; on other servers only with setPrinterOnServers.
void setPrinter(bool on);
void setPrinterOnServers(bool on);
// Creative, local world only: the in-process server sets every block of the shown structure (like /setblock).
void paste();
// True while this device runs the world's server (singleplayer or hosting).
bool inLocalWorld();
// The game's Block for `bs` with its states (turned with `view`), or its default state when those can't be applied;
// null before Easy Place's hooks are installed or for unknown blocks. Any thread.
const void *blockWithStates(const BlockState &bs, const View &view);
void captureHeldOnce(std::function<void(const std::string &)> done);

}  // namespace bematica::easyplace
