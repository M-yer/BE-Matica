#include "library.hpp"

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace bematica {

namespace fs = std::filesystem;
namespace mm = pl::modmenu;

constexpr const char *kModule = "bematica.main";
constexpr const char *kPanelButton = "bematica.panel";

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
    ctx_ = &ctx;
    structuresDir_ = ctx.configDir() / "structures";
    std::error_code ec;
    fs::create_directories(structuresDir_, ec);

    bool ok = mm::ModuleBuilder(kModule, "BE-Matica")
                  .description("Load a .mcstructure file and see the blocks it needs.")
                  .modId(ctx.id())
                  .defaultEnabled(true)
                  .config("next", "Next file", mm::ConfigType::Button)
                  .config("load", "Load selected file", mm::ConfigType::Button)
                  .config("refresh", "Refresh file list", mm::ConfigType::Button)
                  .config("panel", "Show panel", mm::ConfigType::Toggle, "true")
                  .onToggle([this](std::string_view, bool on) { setPanel(on); })
                  .onConfigChanged([this](std::string_view, std::string_view key, std::string_view value) {
                    onAction(key, value);
                  })
                  .registerModule();
    ok &= mm::ButtonBuilder(kPanelButton, "BE-Matica panel")
              .moduleId(kModule)
              .modId(ctx.id())
              .label("BM")
              .onEvent([this](std::string_view, mm::ButtonEvent e, float) {
                if (e == mm::ButtonEvent::Click) setPanel(!panel_);
              })
              .registerButton();
    if (!ok) ctx.logger().warn("BE-Matica: menu registration failed");

    refresh();
    ctx.logger().info("BE-Matica enabled, structures folder: {}", structuresDir_.string());
    return true;
  }

  bool disable(pl::mod::ModContext &ctx) {
    mm::unregisterButton(kPanelButton);
    mm::unregisterModule(kModule);
    ctx.logger().info("BE-Matica disabled");
    return true;
  }

private:
  void onAction(std::string_view key, std::string_view value) {
    if (key == "next") next();
    else if (key == "load") loadSelected();
    else if (key == "refresh") refresh();
    else if (key == "panel") setPanel(value == "true" || value == "1");
  }

  void setPanel(bool on) {
    std::lock_guard lock(mu_);
    panel_ = on;
    drawLocked();
  }

  void refresh() {
    std::lock_guard lock(mu_);
    // Download is where phones save files from the browser, so users don't have to find the mod folder.
    files_ = findStructures({structuresDir_, "/sdcard/Download"});
    if (selected_ >= files_.size()) selected_ = 0;
    status_ = files_.empty() ? "No .mcstructure files found" : std::to_string(files_.size()) + " files found";
    drawLocked();
  }

  void next() {
    std::lock_guard lock(mu_);
    if (!files_.empty()) selected_ = (selected_ + 1) % files_.size();
    drawLocked();
  }

  void loadSelected() {
    std::lock_guard lock(mu_);
    if (files_.empty()) return;
    const auto &file = files_[selected_];
    std::string error;
    Structure s;
    if (!loadStructureFile(file, s, error)) {
      status_ = "Failed: " + error;
      ctx_->logger().warn("BE-Matica: cannot load {}: {}", file.string(), error);
    } else {
      loaded_ = std::move(s);
      loadedName_ = file.filename().string();
      materials_ = countMaterials(loaded_);
      status_ = "Loaded";
      ctx_->logger().info("BE-Matica: loaded {} ({}x{}x{})", loadedName_, loaded_.size[0], loaded_.size[1],
                          loaded_.size[2]);
    }
    drawLocked();
  }

  void drawLocked() {
    std::vector<mm::DrawCommand> cmds;
    if (panel_) {
      std::vector<std::string> lines{"BE-Matica"};
      lines.push_back(files_.empty() ? "Put .mcstructure files in Download"
                                     : "File " + std::to_string(selected_ + 1) + "/" + std::to_string(files_.size()) +
                                           ": " + files_[selected_].filename().string());
      lines.push_back(status_);
      if (!loadedName_.empty()) {
        size_t total = 0;
        for (const auto &m : materials_) total += m.count;
        lines.push_back(loadedName_ + "  " + std::to_string(loaded_.size[0]) + "x" + std::to_string(loaded_.size[1]) +
                        "x" + std::to_string(loaded_.size[2]) + ", " + std::to_string(total) + " blocks");
        constexpr size_t kShown = 12;
        for (size_t k = 0; k < materials_.size() && k < kShown; ++k)
          lines.push_back(std::to_string(materials_[k].count) + "  " + materials_[k].name);
        if (materials_.size() > kShown) lines.push_back("+" + std::to_string(materials_.size() - kShown) + " more");
      }

      constexpr float kX = 16, kY = 80, kLine = 20, kWidth = 420;
      cmds.push_back({.type = mm::DrawCommandType::RectFilled, .x = kX, .y = kY, .w = kWidth,
                      .h = kLine * lines.size() + 12, .color = 0xB0000000});
      for (size_t k = 0; k < lines.size(); ++k)
        cmds.push_back({.type = mm::DrawCommandType::Text, .x = kX + 8, .y = kY + 6 + kLine * k,
                        .color = 0xFFFFFFFF, .size = 16, .text = lines[k]});
    }
    mm::submitDrawCommands(kModule, cmds);
  }

  pl::mod::ModContext *ctx_ = nullptr;
  fs::path structuresDir_;
  std::mutex mu_;
  bool panel_ = true;
  std::vector<fs::path> files_;
  size_t selected_ = 0;
  std::string status_;
  Structure loaded_;
  std::string loadedName_;
  std::vector<Material> materials_;
};

}  // namespace bematica

PL_REGISTER_MOD(bematica::Mod, bematica::Mod::instance())
