#include "texresolve.hpp"

#include "orient.hpp"

#include <array>
#include <string_view>

namespace bematica {
namespace {

struct Vec {
  int x, y, z;
};
using Mat = std::array<std::array<int, 3>, 3>;  // rows

constexpr Vec kNormal[6] = {{0, 1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
// Texture axes of a face seen from outside: right, up (the picture's top edge). Same winding as kCorner in mesh.cpp.
constexpr Vec kRight[6] = {{1, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
constexpr Vec kUp[6] = {{0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}};

int dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec applyInverse(const Mat &m, Vec v) {  // rotations: the inverse is the transpose
  return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
          m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
}

Face faceOf(Vec n) {
  for (int f = 0; f < 6; ++f)
    if (kNormal[f].x == n.x && kNormal[f].y == n.y && kNormal[f].z == n.z) return static_cast<Face>(f);
  return Face::Up;
}

// Quarter-turn rotation taking the axis `from` to the axis `to` (R v = a x v + a (a . v) with a = from x to).
Mat rotateTo(Face from, Face to) {
  Mat m{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  if (from == to) return m;
  const Vec f = kNormal[static_cast<int>(from)], t = kNormal[static_cast<int>(to)];
  Vec a = cross(f, t);
  const bool opposite = a.x == 0 && a.y == 0 && a.z == 0;
  if (opposite) a = f.y != 0 ? Vec{1, 0, 0} : Vec{0, 1, 0};  // half turn about a horizontal / the vertical axis
  const Vec basis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int c = 0; c < 3; ++c) {
    const Vec v = basis[c];
    const Vec k = cross(a, v);
    const int d = dot(a, v);
    const Vec r = opposite ? Vec{2 * a.x * d - v.x, 2 * a.y * d - v.y, 2 * a.z * d - v.z}
                           : Vec{k.x + a.x * d, k.y + a.y * d, k.z + a.z * d};
    m[0][c] = r.x, m[1][c] = r.y, m[2][c] = r.z;
  }
  return m;
}

bool readState(const nbt::Tag &states, const char *key, orient::StateValue &v) {
  const nbt::Tag *t = states.get(key);
  if (!t) t = states.get(std::string("minecraft:") + key);
  if (!t) return false;
  if (t->type == nbt::Type::String) v = {0, t->s, true};
  else v = {static_cast<int>(t->i), {}, false};
  return true;
}

Face compass(std::string_view s, bool &ok) {
  ok = true;
  if (s == "north") return Face::North;
  if (s == "south") return Face::South;
  if (s == "west") return Face::West;
  if (s == "east") return Face::East;
  if (s == "up") return Face::Up;
  if (s == "down") return Face::Down;
  ok = false;
  return Face::North;
}

// The way the block points (its front, its open side, its head), after the hologram's rotation / mirror.
bool facingOf(const BlockState &b, std::string_view name, int rotation, bool mirror, Face &out) {
  orient::StateValue v;
  const nbt::Tag &st = b.states;
  const char *keys[] = {"cardinal_direction", "facing_direction", "direction"};
  for (const char *key : keys) {
    if (!readState(st, key, v)) continue;
    orient::transformState(name, key, v, rotation, mirror);
    bool ok = false;
    if (v.isText) {
      out = compass(v.s, ok);
      if (ok) return true;
    } else if (key == keys[1] && v.i >= 0 && v.i < 6) {
      static constexpr Face byValue[6] = {Face::Down, Face::Up, Face::North, Face::South, Face::West, Face::East};
      out = byValue[v.i];
      return true;
    } else if (key == keys[2] && v.i >= 0 && v.i < 4) {  // beehives, looms, bookshelves: 0 south, 1 west, 2 north, 3 east
      static constexpr Face byValue[4] = {Face::South, Face::West, Face::North, Face::East};
      out = byValue[v.i];
      return true;
    }
  }
  return false;
}

// Blocks whose blocks.json faces are written for a model whose front is on a fixed side (the legacy layout).
struct Oriented {
  std::string_view name;
  Face front;
};
constexpr Oriented kOriented[] = {
    {"furnace", Face::South},       {"blast_furnace", Face::South},         {"smoker", Face::South},
    {"lit_furnace", Face::East},    {"lit_blast_furnace", Face::East},      {"lit_smoker", Face::East},
    {"carved_pumpkin", Face::South}, {"lit_pumpkin", Face::South},           {"beehive", Face::South},
    {"bee_nest", Face::South},      {"observer", Face::North},              {"command_block", Face::North},
    {"chain_command_block", Face::North}, {"repeating_command_block", Face::North}, {"loom", Face::North},
    {"lectern", Face::North},       {"vault", Face::North},                 {"chiseled_bookshelf", Face::North},
    {"piston", Face::Up},           {"sticky_piston", Face::Up},
};

bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string_view canonicalName(std::string_view name) {
  constexpr std::string_view prefix = "minecraft:";
  if (name.substr(0, prefix.size()) == prefix) name.remove_prefix(prefix.size());
  // Newer block names that blocks.json still lists under their old ones.
  if (name == "grass_block") return "grass";
  if (name == "iron_chain") return "chain";
  if (name == "sea_lantern") return "seaLantern";
  return name;
}

}  // namespace

Face worldFace(Face f, int rotation, bool mirror) {
  if (f == Face::Up || f == Face::Down) return f;
  if (mirror && (f == Face::East || f == Face::West)) f = f == Face::East ? Face::West : Face::East;
  static constexpr Face clockwise[4] = {Face::North, Face::East, Face::South, Face::West};
  int i = 0;
  while (clockwise[i] != f) ++i;
  return clockwise[(i + (((rotation % 4) + 4) % 4)) & 3];
}

void faceCoords(Face f, float px, float py, float pz, float &a, float &b) {
  const int i = static_cast<int>(f);
  const float p[3] = {px, py, pz};
  auto axis = [&](Vec v) {  // coordinate along v, 0..1
    const float c = v.x != 0 ? p[0] : v.y != 0 ? p[1] : p[2];
    return v.x + v.y + v.z > 0 ? c : 1.0f - c;
  };
  a = axis(kRight[i]);
  b = axis(kUp[i]);
}

bool TextureResolver::resolve(const BlockState &block, Face face, int rotation, bool mirror, FaceTexture &out) const {
  const std::string_view name = canonicalName(block.name);
  const auto it = atlas_.blocks.find(std::string(name));
  if (it == atlas_.blocks.end()) return false;
  const BlockModel &m = it->second;
  const bool carried = preferCarried_ && !m.carried[0].empty();
  std::array<std::string, 6> model = carried ? m.carried : m.faces;
  const auto &src = model;  // faces as written in blocks.json

  Mat R{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  orient::StateValue v;
  Face dir = Face::Up;
  int variant = 0;
  if (readState(block.states, "pillar_axis", v)) {
    orient::transformState(name, "pillar_axis", v, rotation, mirror);
    if (v.isText && v.s == "x") R = rotateTo(Face::Up, Face::East);
    else if (v.isText && v.s == "z") R = rotateTo(Face::Up, Face::South);
  } else if (name == "barrel") {
    // blocks.json draws a barrel lying with its opening to the north; the model here is the standing one (opening up).
    const std::string top = src[static_cast<int>(Face::North)], bottom = src[static_cast<int>(Face::South)],
                      side = src[static_cast<int>(Face::Up)];
    model = {top, bottom, side, side, side, side};
    if (facingOf(block, name, rotation, mirror, dir)) R = rotateTo(Face::Up, dir);
    if (readState(block.states, "open_bit", v) && !v.isText && v.i) variant = 1;
  } else if (name == "dispenser" || name == "dropper") {
    if (!facingOf(block, name, rotation, mirror, dir)) dir = Face::South;
    if (dir == Face::Up || dir == Face::Down) {  // blocks.json keeps the vertical front on its east face
      const std::string front = src[static_cast<int>(Face::East)], back = src[static_cast<int>(Face::Up)],
                        side = src[static_cast<int>(Face::North)];
      model = {front, back, side, side, side, side};
      R = rotateTo(Face::Up, dir);
    } else {
      R = rotateTo(Face::South, dir);
    }
  } else {
    for (const Oriented &o : kOriented)
      if (o.name == name && facingOf(block, name, rotation, mirror, dir)) R = rotateTo(o.front, dir);
    if (endsWith(name, "chest") && facingOf(block, name, rotation, mirror, dir)) R = rotateTo(Face::South, dir);
    if (name == "observer" && readState(block.states, "powered_bit", v) && !v.isText && v.i) variant = 1;
  }

  const int fi = static_cast<int>(face);
  const Face from = faceOf(applyInverse(R, kNormal[fi]));
  const auto tilesIt = atlas_.tiles.find(model[static_cast<int>(from)]);
  if (tilesIt == atlas_.tiles.end() || tilesIt->second.empty()) return false;
  const auto &tiles = tilesIt->second;
  out.tile = tiles[static_cast<size_t>(variant) < tiles.size() ? variant : 0];
  out.u0 = static_cast<float>(out.tile.x) / atlas_.width;
  out.v0 = static_cast<float>(out.tile.y) / atlas_.height;
  out.u1 = static_cast<float>(out.tile.x + out.tile.w) / atlas_.width;
  out.v1 = static_cast<float>(out.tile.y + out.tile.h) / atlas_.height;
  const Vec right = applyInverse(R, kRight[fi]), up = applyInverse(R, kUp[fi]);
  const int mi = static_cast<int>(from);
  out.turn[0][0] = dot(right, kRight[mi]), out.turn[0][1] = dot(up, kRight[mi]);
  out.turn[1][0] = dot(right, kUp[mi]), out.turn[1][1] = dot(up, kUp[mi]);
  return true;
}

}  // namespace bematica
