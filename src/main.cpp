#include <pl/Mod.hpp>

namespace bematica {

class Mod {
public:
  static Mod &instance() {
    static Mod mod;
    return mod;
  }

  bool load(pl::mod::ModContext &ctx) {
    ctx.logger().info("BE-Matica {} loaded", ctx.info().version);
    return true;
  }

  bool enable(pl::mod::ModContext &ctx) {
    ctx.logger().info("BE-Matica enabled");
    return true;
  }

  bool disable(pl::mod::ModContext &ctx) {
    ctx.logger().info("BE-Matica disabled");
    return true;
  }
};

}  // namespace bematica

PL_REGISTER_MOD(bematica::Mod, bematica::Mod::instance())
