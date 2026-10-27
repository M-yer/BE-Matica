#pragma once

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

}  // namespace bematica::easyplace
