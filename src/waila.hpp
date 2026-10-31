#pragma once

#include "hologram.hpp"

#include <mutex>
#include <string>

namespace bematica {

// Card at the top of the screen naming the block under the crosshair, and what the structure wants in that cell.
// Hidden while `info` is not valid.
class LookCard {
public:
  void init(std::string moduleId);
  void update(const hologram::LookInfo &info);

private:
  std::mutex mu_;
  std::string moduleId_;
};

}  // namespace bematica
