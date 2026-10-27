#include "hologram.hpp"
#include "library.hpp"
#include "materials_panel.hpp"

#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <algorithm>
#include <cstdlib>
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
                    hologram::setVisible(enabled_ && hologramOn_);
                    syncPanelLocked();
                  })
                  .onConfigChanged([this](std::string_view, std::string_view key, std::string_view value) {
                    onSetting(key, value);
                  })
                  .registerModule();
    if (!ok) ctx.logger().warn("BE-Matica: menu registration failed");
    panel_.init(kModule, ctx.id());
    const auto surface = mm::getHudSurfaceSize();
    ctx.logger().info("BE-Matica: HUD surface {}x{}", surface.width, surface.height);

    hologram::setOpacity(opacity_ / 100.0f);
    hologram::installAsync(ctx.logger(), [this](bool ok, const std::string &error) {
      std::lock_guard lock(mu_);
      hologramOk_ = ok;
      if (!ok) {
        status_ = "Hologram unavailable: " + error;
        ctx_->logger().warn("BE-Matica: {}", status_);
      }
      publishLocked();
    });

    std::lock_guard lock(mu_);
    scanLocked();
    publishLocked();
    ctx.logger().info("BE-Matica enabled, structures folder: {}", structuresDir_.string());
    return true;
  }

  bool disable(pl::mod::ModContext &ctx) {
    panel_.shutdown();
    hologram::uninstall();
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
    } else if (key == "hologram") {
      hologramOn_ = value == "true" || value == "1";
      hologram::setVisible(enabled_ && hologramOn_);
    } else if (key == "move") {
      hologram::moveToPlayer();
      status_ = "Hologram moved to you";
    } else if (onPlacementLocked(key, value)) {
      return;  // hologram-only change; the page stays as it is
    } else if (key == "opacity") {
      opacity_ = std::clamp(std::atoi(std::string(value).c_str()), 10, 100);
      hologram::setOpacity(opacity_ / 100.0f);
      return;  // no page change needed while the slider is dragged
    } else {
      return;
    }
    publishLocked();
  }

  // Nudge, rotate, mirror and layer controls. Returns false if `key` isn't one of them.
  bool onPlacementLocked(std::string_view key, std::string_view value) {
    static constexpr struct {
      const char *key;
      int dx, dy, dz;
    } kNudges[] = {{"east", 1, 0, 0}, {"west", -1, 0, 0}, {"up", 0, 1, 0},
                   {"down", 0, -1, 0}, {"south", 0, 0, 1}, {"north", 0, 0, -1}};
    for (const auto &n : kNudges)
      if (key == n.key) {
        hologram::nudge(n.dx, n.dy, n.dz);
        return true;
      }
    const int maxLayer = loadedName_.empty() ? 0 : loaded_.size[1] - 1;
    if (key == "rotate") {
      view_.rotation = (view_.rotation + 1) % 4;
    } else if (key == "mirror") {
      view_.mirror = !view_.mirror;
    } else if (key == "layers") {
      view_.layers = value == "single" ? View::Layers::Single
                     : value == "upto" ? View::Layers::UpTo
                                       : View::Layers::All;
    } else if (key == "layer") {
      view_.layer = std::clamp(std::atoi(std::string(value).c_str()) - 1, 0, maxLayer);
    } else if (key == "layer_up" || key == "layer_down") {
      view_.layer = std::clamp(view_.layer + (key == "layer_up" ? 1 : -1), 0, maxLayer);
      if (view_.layers == View::Layers::All) view_.layers = View::Layers::Single;
      hologram::setView(view_);
      publishLocked();  // keeps the layer slider in step with the buttons
      return true;
    } else {
      return false;
    }
    hologram::setView(view_);
    return true;
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
    panel_.setContent(name.substr(0, name.rfind('.')), materials_);
    view_ = View{};
    hologram::setView(view_);
    hologram::show(std::make_shared<const Structure>(loaded_), true);
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

    if (hologramOk_) {
      schema.node(N{.id = "hologram", .key = "hologram", .category = "main", .title = "Show hologram",
                    .type = T::Toggle, .defaultValue = "true", .currentValue = hologramOn_ ? "true" : "false",
                    .hasCurrentValue = true});
      schema.node(N{.id = "move", .key = "move", .category = "main", .title = "Move hologram to me",
                    .description = "Puts the structure's corner where you stand.", .type = T::Button});
      schema.node(N{.id = "opacity", .key = "opacity", .category = "main", .title = "Hologram opacity",
                    .type = T::SliderInt, .defaultValue = "45", .currentValue = std::to_string(opacity_),
                    .hasCurrentValue = true, .minValue = "10", .maxValue = "100", .step = "5", .unit = "%"});
      if (!loadedName_.empty()) addPlacementNodes(schema);
    }
    schema.node(N{.id = "refresh", .key = "refresh", .category = "main", .title = "Refresh list",
                  .description = "Rescan " + structuresDir_.string().substr(8), .type = T::Button});
    schema.node(N{.id = "overlay", .key = "overlay", .category = "main", .title = "Blocks button on screen",
                  .description = "Opens a scrollable block list while you build. Tap a block to tick it off.",
                  .type = T::Toggle, .defaultValue = "false", .currentValue = overlay_ ? "true" : "false",
                  .hasCurrentValue = true});
    if (!status_.empty())
      schema.node(N{.id = "status", .category = "main", .title = status_, .type = T::Info});

    if (!loadedName_.empty()) {
      size_t total = 0;
      for (const auto &m : materials_) total += m.count;
      schema.node(N{.id = "materials", .category = "main",
                    .title = loadedName_ + "  (" + sizeText(loaded_) + ", " + std::to_string(total) + " blocks)",
                    .section = "Materials", .type = T::Info});
      // One row per block: the launcher caps a single description at 1024 characters, and it stays under
      // its 512-node limit with room for the controls above.
      constexpr size_t kMaxRows = 400;
      for (size_t k = 0; k < materials_.size() && k < kMaxRows; ++k)
        schema.node(N{.id = "m" + std::to_string(k), .category = "main",
                      .title = std::to_string(materials_[k].count) + "   " + materials_[k].name, .section = "Materials",
                      .type = T::Info});
    }

    publishing_ = true;
    if (!mm::setConfigSchemaJson(kModule, schema.toJson())) ctx_->logger().warn("BE-Matica: settings page rejected");
    publishing_ = false;
    syncPanelLocked();
  }

  void addPlacementNodes(mm::ConfigSchemaBuilder &schema) {
    using N = mm::ConfigNodeV2;
    using T = mm::ConfigControlTypeV2;
    const std::pair<const char *, const char *> moves[] = {
        {"up", "Move up"},         {"down", "Move down"},       {"north", "Move north (-Z)"},
        {"south", "Move south (+Z)"}, {"west", "Move west (-X)"}, {"east", "Move east (+X)"},
        {"rotate", "Rotate 90\u00B0"}, {"mirror", "Mirror"}};
    for (const auto &[key, title] : moves)
      schema.node(N{.id = key, .key = key, .category = "main", .title = title, .section = "Placement",
                    .type = T::Button});

    const char *mode = view_.layers == View::Layers::Single ? "single"
                       : view_.layers == View::Layers::UpTo ? "upto"
                                                            : "all";
    N layers{.id = "layers", .key = "layers", .category = "main", .title = "Show", .section = "Layers",
             .type = T::Choice, .choiceStyle = mm::ConfigChoiceStyleV2::Segmented, .defaultValue = "all",
             .currentValue = mode, .hasCurrentValue = true};
    layers.options = {{.value = "all", .label = "All"}, {.value = "single", .label = "One layer"},
                      {.value = "upto", .label = "Up to layer"}};
    schema.node(std::move(layers));
    schema.node(N{.id = "layer", .key = "layer", .category = "main", .title = "Layer", .section = "Layers",
                  .type = T::SliderInt, .defaultValue = "1", .currentValue = std::to_string(view_.layer + 1),
                  .hasCurrentValue = true, .minValue = "1", .maxValue = std::to_string(loaded_.size[1]),
                  .step = "1"});
    schema.node(N{.id = "layer_up", .key = "layer_up", .category = "main", .title = "Layer up",
                  .section = "Layers", .type = T::Button});
    schema.node(N{.id = "layer_down", .key = "layer_down", .category = "main", .title = "Layer down",
                  .section = "Layers", .type = T::Button});
  }

  void syncPanelLocked() { panel_.setVisible(enabled_ && overlay_ && !loadedName_.empty()); }

  pl::mod::ModContext *ctx_ = nullptr;
  MaterialsPanel panel_;
  bool hologramOk_ = false, hologramOn_ = true;
  int opacity_ = 45;
  View view_;
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
