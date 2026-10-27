#include "library.hpp"

#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace bematica {

namespace fs = std::filesystem;
namespace mm = pl::modmenu;

constexpr const char *kModule = "bematica.main";

// The launcher app can always read its own folder on shared storage; Download needs "All files access".
fs::path appStorageDir() {
  std::string pkg;
  std::getline(std::ifstream("/proc/self/cmdline"), pkg, '\0');
  pkg = pkg.substr(0, pkg.find(':'));
  return fs::path("/sdcard/Android/data") / pkg / "files";
}

std::string sizeText(const Structure &s) {
  return std::to_string(s.size[0]) + " x " + std::to_string(s.size[1]) + " x " + std::to_string(s.size[2]);
}

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
    structuresDir_ = appStorageDir() / "BE-Matica" / "structures";
    std::error_code ec;
    fs::create_directories(structuresDir_, ec);

    bool ok = mm::ModuleBuilder(kModule, "BE-Matica")
                  .description("Load a structure and see the blocks it needs.")
                  .modId(ctx.id())
                  .defaultEnabled(true)
                  .hideInHudEditor(false)
                  .onToggle([this](std::string_view, bool on) {
                    std::lock_guard lock(mu_);
                    enabled_ = on;
                    drawOverlayLocked();
                  })
                  .onConfigChanged([this](std::string_view, std::string_view key, std::string_view value) {
                    onSetting(key, value);
                  })
                  .registerModule();
    if (!ok) ctx.logger().warn("BE-Matica: menu registration failed");

    std::lock_guard lock(mu_);
    scanLocked();
    publishLocked();
    ctx.logger().info("BE-Matica enabled, structures folder: {}", structuresDir_.string());
    return true;
  }

  bool disable(pl::mod::ModContext &ctx) {
    mm::clearConfigSchema(kModule);
    mm::unregisterModule(kModule);
    ctx.logger().info("BE-Matica disabled");
    return true;
  }

private:
  void onSetting(std::string_view key, std::string_view value) {
    if (key == "import") return importFile();
    std::lock_guard lock(mu_);
    if (publishing_) return;  // the launcher echoing values from the page we are setting
    if (key == "file") {
      loadLocked(std::string(value));
    } else if (key == "refresh") {
      scanLocked();
      status_ = files_.empty() ? "No structures yet" : std::to_string(files_.size()) + " structures found";
    } else if (key == "overlay") {
      overlay_ = value == "true" || value == "1";
    } else {
      return;
    }
    publishLocked();
  }

  void importFile() {
    bool started = pl::input::openDocument("*/*", [this](const pl::input::DocumentResult &r) {
      std::lock_guard lock(mu_);
      if (!r.success) {
        if (!r.error.empty()) status_ = "Import failed: " + r.error;
      } else {
        // Only the name is kept, so a crafted display name can't write outside the folder.
        std::string name = fs::path(r.displayName.empty() ? r.path : r.displayName).filename().string();
        Structure probe;
        std::string error;
        if (fs::path(name).extension() != ".mcstructure") {
          status_ = "Only .mcstructure files for now";
        } else if (!loadStructureFile(r.path, probe, error)) {
          status_ = "Not a valid structure: " + error;
        } else {
          std::error_code ec;
          fs::copy_file(r.path, structuresDir_ / name, fs::copy_options::overwrite_existing, ec);
          if (ec) {
            status_ = "Could not save file: " + ec.message();
          } else {
            scanLocked();
            loadLocked(name);
          }
        }
      }
      if (!status_.empty()) ctx_->logger().info("BE-Matica import: {}", status_);
      publishLocked();
    });
    if (!started) {
      std::lock_guard lock(mu_);
      status_ = "File picker unavailable";
      publishLocked();
    }
  }

  void scanLocked() {
    files_.clear();
    for (const auto &p : findStructures({structuresDir_})) files_.push_back(p.filename().string());
  }

  void loadLocked(const std::string &name) {
    Structure s;
    std::string error;
    if (!loadStructureFile(structuresDir_ / fs::path(name).filename(), s, error)) {
      status_ = "Failed to load " + name + ": " + error;
      ctx_->logger().warn("BE-Matica: {}", status_);
      return;
    }
    loaded_ = std::move(s);
    loadedName_ = name;
    materials_ = countMaterials(loaded_);
    status_ = "Loaded " + name;
    ctx_->logger().info("BE-Matica: loaded {} ({})", name, sizeText(loaded_));
  }

  // Rebuilds the settings page (the file list and material list change at runtime) and the overlay.
  void publishLocked() {
    using N = mm::ConfigNodeV2;
    using T = mm::ConfigControlTypeV2;
    mm::ConfigSchemaBuilder schema;
    schema.category("main", "BE-Matica").defaultCategory("main");

    schema.node(N{.id = "import", .key = "import", .category = "main", .title = "Import structure",
                  .description = "Pick a .mcstructure file from your phone.", .type = T::Button});

    N file{.id = "file", .key = "file", .category = "main", .title = "Structure", .type = T::Choice,
           .choiceStyle = mm::ConfigChoiceStyleV2::Dropdown, .currentValue = loadedName_, .hasCurrentValue = true};
    for (const auto &f : files_) file.options.push_back({.value = f, .label = f});
    if (files_.empty()) file.description = "Import a structure to get started.";
    schema.node(std::move(file));

    schema.node(N{.id = "refresh", .key = "refresh", .category = "main", .title = "Refresh list",
                  .description = "Rescan " + structuresDir_.string().substr(8), .type = T::Button});
    schema.node(N{.id = "overlay", .key = "overlay", .category = "main", .title = "Material list on screen",
                  .type = T::Toggle, .defaultValue = "false", .currentValue = overlay_ ? "true" : "false",
                  .hasCurrentValue = true});
    if (!status_.empty())
      schema.node(N{.id = "status", .category = "main", .title = status_, .type = T::Info});

    if (!loadedName_.empty()) {
      size_t total = 0;
      std::string list;
      for (const auto &m : materials_) {
        total += m.count;
        list += std::to_string(m.count) + "  " + m.name + "\n";
      }
      if (!list.empty()) list.pop_back();
      schema.node(N{.id = "materials", .category = "main",
                    .title = loadedName_ + "  (" + sizeText(loaded_) + ", " + std::to_string(total) + " blocks)",
                    .description = list, .section = "Materials", .type = T::Info});
    }

    publishing_ = true;
    if (!mm::setConfigSchemaJson(kModule, schema.toJson())) ctx_->logger().warn("BE-Matica: settings page rejected");
    publishing_ = false;
    drawOverlayLocked();
  }

  void drawOverlayLocked() {
    std::vector<mm::DrawCommand> cmds;
    if (enabled_ && overlay_ && !loadedName_.empty()) {
      std::vector<std::string> lines{loadedName_ + "  " + sizeText(loaded_)};
      constexpr size_t kShown = 12;
      for (size_t k = 0; k < materials_.size() && k < kShown; ++k)
        lines.push_back(std::to_string(materials_[k].count) + "  " + materials_[k].name);
      if (materials_.size() > kShown) lines.push_back("+" + std::to_string(materials_.size() - kShown) + " more");

      constexpr float kX = 16, kY = 80, kLine = 20, kWidth = 360;
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
  // Recursive: the launcher may call back into onSetting while a settings page is being set.
  std::recursive_mutex mu_;
  bool publishing_ = false;
  bool enabled_ = true;
  bool overlay_ = false;
  std::vector<std::string> files_;
  std::string status_;
  Structure loaded_;
  std::string loadedName_;
  std::vector<Material> materials_;
};

}  // namespace bematica

PL_REGISTER_MOD(bematica::Mod, bematica::Mod::instance())
