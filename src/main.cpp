#include "easyplace.hpp"
#include "hologram.hpp"
#include "textured.hpp"
#include "library.hpp"
#include "litematic.hpp"
#include "materials_panel.hpp"

#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace bematica {

namespace fs = std::filesystem;
namespace mm = pl::modmenu;

constexpr const char *kModule = "bematica.main";
// Its own module so the control panel can be switched on and off in the launcher.
constexpr const char *kControlsModule = "bematica.controls";

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
    ctx.logger().info("BE-Matica {} loaded (built {} {})", ctx.info().version, __DATE__, __TIME__);
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
    ok = mm::ModuleBuilder(kControlsModule, "BE-Matica Controls")
             .description("Floating control panel: move, rotate, layer controls and the block list.")
             .modId(ctx.id())
             .defaultEnabled(true)
             .hideInHudEditor(false)
             .onToggle([this](std::string_view, bool on) {
               std::lock_guard lock(mu_);
               controls_ = on;
               syncPanelLocked();
             })
             .registerModule();
    if (!ok) ctx.logger().warn("BE-Matica: controls module registration failed");
    panel_.init(kControlsModule, ctx.id(), [this](std::string_view key, std::string_view value) { onSetting(key, value); });
    hologram::setProgressListener([this](size_t built, size_t total) {
      panel_.setProgress(built, total);
      std::lock_guard lock(mu_);
      autoLockLocked(built, total);
    });
    const auto surface = mm::getHudSurfaceSize();
    ctx.logger().info("BE-Matica: HUD surface {}x{}", surface.width, surface.height);

    loadSettings();
    hologram::setOpacity(opacity_ / 100.0f);
    hologram::setOutlines(outlinesOn_);
    hologram::setFill(fillOn_);
    hologram::setVisible(hologramOn_);
    textured::setEnabled(texturedOn_);
    easyplace::setEnabled(easyPlaceOn_);
    hologram::setOriginListener([this](std::array<int, 3> origin) {
      std::lock_guard lock(mu_);
      origin_ = origin, haveOrigin_ = true;
      saveSettingsLocked();
      // Keeps the Location fields current, except right after one was typed (that would reset the field).
      if (!originTyped_) publishLocked();
      originTyped_ = false;
    });
    {
      // The structure comes back where it was left, with the same rotation, mirror and layer view.
      std::lock_guard lock(mu_);
      if (rememberOn_ && !savedName_.empty() && haveOrigin_) {
        const auto origin = origin_;
        loadLocked(savedName_, &savedView_, &origin);
      }
    }
    easyplace::setAirPlace(airPlaceOn_);
    {
      std::lock_guard lock(mu_);
      syncPanelLocked();
    }
    easyplace::installAsync(
        ctx.logger(), [this](const std::string &text) { panel_.toast(text); },
        [this] {
          // Runs on the game thread right after a placement; the settings page is updated from here too.
          std::lock_guard lock(mu_);
          if (loadedName_.empty() || view_.layers != View::Layers::Single) return;
          if (view_.layer + 1 >= loaded_.size[1]) {
            panel_.toast("Structure complete!");
            return;
          }
          ++view_.layer;
          hologram::setView(view_);
          saveSettingsLocked();
          panel_.toast("Layer " + std::to_string(view_.layer) + " done, now layer " + std::to_string(view_.layer + 1));
          publishLocked();
        },
        [this](bool ok, const std::string &error) {
          std::lock_guard lock(mu_);
          easyPlaceOk_ = ok;
          if (!ok) {
            status_ = "Easy Place unavailable: " + error;
            ctx_->logger().warn("BE-Matica: {}", status_);
          }
          publishLocked();
        });
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
    hologram::setProgressListener(nullptr);
    hologram::setOriginListener(nullptr);
    panel_.shutdown();
    hologram::uninstall();
    easyplace::uninstall();
    mm::clearConfigSchema(kModule);
    mm::unregisterModule(kModule);
    mm::unregisterModule(kControlsModule);
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
    } else if (key == "texture_next") {
      status_ = textured::nextTexture();
    } else if (key == "textured") {
      texturedOn_ = value == "true" || value == "1";
      textured::setEnabled(texturedOn_);
      saveSettingsLocked();
    } else if (key == "hologram") {
      hologramOn_ = value == "true" || value == "1";
      hologram::setVisible(enabled_ && hologramOn_);
      saveSettingsLocked();
    } else if (key == "easyplace") {
      easyPlaceOn_ = value == "true" || value == "1";
      easyplace::setEnabled(easyPlaceOn_);
      saveSettingsLocked();
      syncLayerLocked();
      return;
    } else if (key == "airplace") {
      airPlaceOn_ = value == "true" || value == "1";
      easyplace::setAirPlace(airPlaceOn_);
      saveSettingsLocked();
      return;
    } else if (key == "lock") {
      locked_ = value == "true" || value == "1";
      saveSettingsLocked();
      panel_.toast(locked_ ? "Placement locked" : "Placement unlocked");
    } else if (key == "autolock" || key == "remember") {
      (key == "autolock" ? autoLockOn_ : rememberOn_) = value == "true" || value == "1";
      saveSettingsLocked();
    } else if (key == "autolock_pct") {
      autoLockPct_ = std::clamp(std::atoi(std::string(value).c_str()), 1, 100);
      saveSettingsLocked();
      return;  // no page change needed while the slider is dragged
    } else if (key == "origin_x" || key == "origin_y" || key == "origin_z") {
      // Typed coordinates apply once they are a whole number; half-typed text ("-") is ignored.
      const std::string text(value);
      char *end = nullptr;
      const long v = std::strtol(text.c_str(), &end, 10);
      if (text.empty() || *end || locked_) {
        if (locked_) panel_.toast("Placement is locked");
        return;
      }
      origin_[key.back() - 'x'] = static_cast<int>(v);
      originTyped_ = true;
      hologram::setOrigin(origin_);
      return;
    } else if (key == "move") {
      if (locked_) return panel_.toast("Placement is locked");
      hologram::moveToPlayer();
      status_ = "Hologram moved to you";
    } else if (onPlacementLocked(key, value)) {
      return;  // hologram-only change; the page stays as it is
    } else if (key == "outlines" || key == "fill") {
      const bool on = value == "true" || value == "1";
      (key == "outlines" ? outlinesOn_ : fillOn_) = on;
      hologram::setOutlines(outlinesOn_);
      hologram::setFill(fillOn_);
      saveSettingsLocked();
    } else if (key == "opacity") {
      opacity_ = std::clamp(std::atoi(std::string(value).c_str()), 10, 100);
      hologram::setOpacity(opacity_ / 100.0f);
      saveSettingsLocked();
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
    const bool moves = key == "rotate" || key == "mirror" ||
                       std::any_of(std::begin(kNudges), std::end(kNudges), [&](const auto &n) { return key == n.key; });
    if (moves && locked_) {
      panel_.toast("Placement is locked");
      return true;
    }
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
      saveSettingsLocked();
      publishLocked();  // keeps the layer slider in step with the buttons
      return true;
    } else {
      return false;
    }
    hologram::setView(view_);
    saveSettingsLocked();
    syncLayerLocked();
    return true;
  }

  // Locks placement once the build passes the auto-lock percentage. Only on the way up past it, so a structure
  // that comes back already past it, or one unlocked by hand, stays as the user left it.
  void autoLockLocked(size_t built, size_t total) {
    if (total == 0) return;
    const int pct = static_cast<int>(built * 100 / total), last = lastPct_;
    lastPct_ = pct;
    if (!autoLockOn_ || locked_ || last < 0 || last >= autoLockPct_ || pct < autoLockPct_) return;
    locked_ = true;
    saveSettingsLocked();
    panel_.toast("Placement locked at " + std::to_string(pct) + "% built");
    publishLocked();
  }

  void addSettingsNodesLocked(mm::ConfigSchemaBuilder &schema) {
    using N = mm::ConfigNodeV2;
    using T = mm::ConfigControlTypeV2;
    auto flag = [](bool on) { return on ? "true" : "false"; };
    schema.node(N{.id = "lock", .key = "lock", .category = "settings", .title = "Lock placement",
                  .description = "Stops moving, rotating and mirroring the structure. Also the padlock on the panel.",
                  .section = "Placement", .type = T::Toggle, .defaultValue = "false", .currentValue = flag(locked_),
                  .hasCurrentValue = true});
    schema.node(N{.id = "autolock", .key = "autolock", .category = "settings", .title = "Auto-lock",
                  .description = "Lock the placement by itself once this much of the structure is built.",
                  .section = "Placement", .type = T::Toggle, .defaultValue = "true",
                  .currentValue = flag(autoLockOn_), .hasCurrentValue = true});
    schema.node(N{.id = "autolock_pct", .key = "autolock_pct", .category = "settings", .title = "Auto-lock at",
                  .section = "Placement", .type = T::SliderInt, .defaultValue = "10",
                  .currentValue = std::to_string(autoLockPct_), .hasCurrentValue = true, .minValue = "1",
                  .maxValue = "100", .step = "1", .unit = "%",
                  .visibleWhen = {{.key = "autolock", .op = mm::ConfigConditionOpV2::Truthy}}});
    schema.node(N{.id = "remember", .key = "remember", .category = "settings", .title = "Remember placement",
                  .description = "Bring the structure back where it was when the game restarts or you rejoin.",
                  .section = "Placement", .type = T::Toggle, .defaultValue = "true",
                  .currentValue = flag(rememberOn_), .hasCurrentValue = true});
    if (loadedName_.empty()) return;
    static constexpr const char *kAxes[][2] = {{"origin_x", "X"}, {"origin_y", "Y"}, {"origin_z", "Z"}};
    for (int k = 0; k < 3; ++k)
      schema.node(N{.id = kAxes[k][0], .key = kAxes[k][0], .category = "settings", .title = kAxes[k][1],
                    .description = k == 0 ? "World position of the structure's corner. Type a number to move it there."
                                          : "",
                    .section = "Location", .type = T::Text, .currentValue = std::to_string(origin_[k]),
                    .hasCurrentValue = true, .placeholder = "0", .maxLength = 9, .disabled = locked_});
  }

  // The Help tab: one Info row per topic (the launcher caps a description at 1024 characters).
  static void addHelpNodes(mm::ConfigSchemaBuilder &schema) {
    using N = mm::ConfigNodeV2;
    static constexpr const char *kTopics[][2] = {
        {"Getting started",
         "1. On the BE-Matica tab, tap Import structure and pick a .mcstructure (or a Java .litematic, which is "
         "converted).\n2. Choose it under Structure. The hologram appears with its corner where you stand.\n"
         "3. Build inside the blue ghost blocks. Blocks you place correctly disappear from the hologram."},
        {"Control panel",
         "The floating panel is on screen while BE-Matica Controls is on. Drag its header (the dots) to move it; "
         "the arrow folds it away.\nThe header shows how much of the shown structure is built.\n"
         "Move tab: the D-pad shifts the hologram one block along X and Z like a map (up = north), the \u25B2 / \u25BC "
         "pair moves it up and down, Rotate +90\u00B0 turns it, Mirror flips it, "
         "< and > pick a single layer, All shows every layer.\nBlocks tab: every block the structure needs; tap a "
         "row to tick it off."},
        {"Lock and saved placement",
         "The structure, its position, rotation, mirror and layer view are saved whenever they change, and come "
         "back when you restart the game or rejoin.\nThe padlock in the panel header locks the placement: moving, "
         "rotating, mirroring and Move hologram to me are blocked until you unlock it."},
        {"Easy Place",
         "Turn it on in the Easy Place tab. Hold the block the structure wants, aim at its ghost block and tap: "
         "it is placed exactly there. Holding the wrong block tells you which one to hold.\nWater and lava: "
         "use a bucket and aim at the spot; the game pours it as usual."},
        {"Hologram look",
         "Hologram opacity, Block outlines and Tinted fill change how ghost blocks are drawn. Show hologram hides "
         "it without unloading the structure."},
        {"Where files go",
         "Imported structures are kept in Android/data/<launcher>/files/BE-Matica/structures. Tap Refresh list "
         "after adding files there yourself."},
    };
    for (size_t k = 0; k < std::size(kTopics); ++k)
      schema.node(N{.id = "help" + std::to_string(k), .category = "help", .title = kTopics[k][0],
                    .description = kTopics[k][1], .type = mm::ConfigControlTypeV2::Info});
  }

  void syncLayerLocked() {
    panel_.setLayer(view_.layer, loadedName_.empty() ? 0 : loaded_.size[1], view_.layers != View::Layers::All);
    panel_.setState(hologramOn_, easyPlaceOn_, view_.mirror, locked_);
  }

  // Converts a Java Litematica schematic and keeps it as <name>.mcstructure next to the others.
  void importLitematicLocked(const std::string &path, const std::string &name) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Structure s;
    LitematicInfo info;
    std::string error;
    if (!loadLitematic(data, s, info, error)) {
      status_ = "Could not convert: " + error;
      return;
    }
    const std::string outName = fs::path(name).stem().string() + ".mcstructure";
    {
      const auto bytes = saveMcstructure(s);
      std::ofstream out(structuresDir_ / outName, std::ios::binary);
      if (!out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        status_ = "Could not save converted file";
        return;
      }
    }
    scanLocked();
    // Show the structure already in memory rather than reading back the file just written.
    showLocked(outName, std::move(s));
    status_ = "Converted " + name + " to " + outName;
    if (!info.unknownBlocks.empty())
      status_ += " (" + std::to_string(info.unknownBlocks.size()) + " Java block types have no Bedrock match and were left out)";
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
        if (fs::path(name).extension() == ".litematic") {
          importLitematicLocked(r.path, name);
        } else if (fs::path(name).extension() != ".mcstructure") {
          status_ = "Pick a .mcstructure or .litematic file";
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

  // `view` and `origin` restore a saved placement; without them the structure appears at the player, or at the
  // current spot while placement is locked.
  void loadLocked(const std::string &name, const View *view = nullptr, const std::array<int, 3> *origin = nullptr) {
    Structure s;
    std::string error;
    if (!loadStructureFile(structuresDir_ / fs::path(name).filename(), s, error)) {
      status_ = "Failed to load " + name + ": " + error;
      ctx_->logger().warn("BE-Matica: {}", status_);
      return;
    }
    showLocked(name, std::move(s), view, origin);
  }

  void showLocked(const std::string &name, Structure &&s, const View *view = nullptr,
                  const std::array<int, 3> *origin = nullptr) {
    loaded_ = std::move(s);
    loadedName_ = name;
    materials_ = countMaterials(loaded_);
    status_ = "Loaded " + name;
    panel_.setContent(name.substr(0, name.rfind('.')), materials_);
    view_ = view ? *view : View{};
    lastPct_ = -1;
    hologram::setView(view_);
    const bool keep = origin || (locked_ && haveOrigin_);
    hologram::show(std::make_shared<const Structure>(loaded_), !keep);
    if (keep) hologram::setOrigin(origin ? *origin : origin_);
    saveSettingsLocked();
    syncLayerLocked();
    ctx_->logger().info("BE-Matica: loaded {} ({})", name, sizeText(loaded_));
  }

  // Rebuilds the settings page (the file list and material list change at runtime) and the overlay.
  void publishLocked() {
    syncLayerLocked();
    using N = mm::ConfigNodeV2;
    using T = mm::ConfigControlTypeV2;
    mm::ConfigSchemaBuilder schema;
    schema.category("main", "BE-Matica").defaultCategory("main");
    if (hologramOk_ && easyPlaceOk_) schema.category("easyplace", "Easy Place");
    if (!loadedName_.empty()) {
      if (hologramOk_) schema.category("place", "Placement & Layers");
      schema.category("materials", "Materials");
    }
    schema.category("settings", "Settings");
    schema.category("help", "Help");

    schema.node(N{.id = "import", .key = "import", .category = "main", .title = "Import structure",
                  .description = "Pick a .mcstructure file, or a Java .litematic to convert, from your phone.", .type = T::Button});

    N file{.id = "file", .key = "file", .category = "main", .title = "Structure", .type = T::Choice,
           .choiceStyle = mm::ConfigChoiceStyleV2::Dropdown, .currentValue = loadedName_, .hasCurrentValue = true};
    for (const auto &f : files_) file.options.push_back({.value = f, .label = f});
    if (files_.empty()) file.description = "Import a structure to get started.";
    schema.node(std::move(file));

    if (hologramOk_) {
      schema.node(N{.id = "hologram", .key = "hologram", .category = "main", .title = "Show hologram",
                    .type = T::Toggle, .defaultValue = "true", .currentValue = hologramOn_ ? "true" : "false",
                    .hasCurrentValue = true});
      if (false)  // experimental textured hologram is disabled in code for now (see textured.cpp)
      schema.node(N{.id = "textured", .key = "textured", .category = "main", .title = "Real block textures (experimental)",
                    .description = "Draw ghost blocks with the game's own models and textures. Hold any block once so "
                                   "the game shows its textures. Turn off if anything looks wrong.",
                    .type = T::Toggle, .defaultValue = "false", .currentValue = texturedOn_ ? "true" : "false",
                    .hasCurrentValue = true});
      if (false && texturedOn_)
        schema.node(N{.id = "texture_next", .key = "texture_next", .category = "main", .title = "Cycle textures (debug)",
                      .description = "Starts or stops switching the ghost blocks to the next game texture every 4 seconds.",
                      .type = T::Button});
      if (easyPlaceOk_) {
        schema.node(N{.id = "easyplace", .key = "easyplace", .category = "easyplace", .title = "Easy Place",
                      .description = "Tap where a ghost block is to place it there. Only the right block is "
                                     "placed: hold it in your hand.",
                      .type = T::Toggle, .defaultValue = "false", .currentValue = easyPlaceOn_ ? "true" : "false",
                      .hasCurrentValue = true});
        schema.node(N{.id = "airplace", .key = "airplace", .category = "easyplace", .title = "Air build",
                      .description = "Place ghost blocks in mid-air, even with nothing next to them.",
                      .type = T::Toggle, .defaultValue = "true", .currentValue = airPlaceOn_ ? "true" : "false",
                      .hasCurrentValue = true});
        schema.node(N{.id = "easyplace_help", .category = "easyplace", .title = "How it works",
                      .description = "Aim at a ghost block, or through it at the ground behind, and tap to place. "
                                     "Taps outside the hologram place blocks normally.\n"
                                     "Holding the wrong block shows which one to hold instead.\n"
                                     "With Placement & Layers set to One layer, the next layer starts by itself "
                                     "when one is finished.",
                      .type = T::Info});
      }
      schema.node(N{.id = "move", .key = "move", .category = "main", .title = "Move hologram to me",
                    .description = "Puts the structure's corner where you stand.", .type = T::Button});
      schema.node(N{.id = "opacity", .key = "opacity", .category = "main", .title = "Hologram opacity",
                    .type = T::SliderInt, .defaultValue = "45", .currentValue = std::to_string(opacity_),
                    .hasCurrentValue = true, .minValue = "10", .maxValue = "100", .step = "5", .unit = "%"});
      schema.node(N{.id = "outlines", .key = "outlines", .category = "main", .title = "Block outlines",
                    .description = "Light blue edges on every ghost block.", .type = T::Toggle, .defaultValue = "true",
                    .currentValue = outlinesOn_ ? "true" : "false", .hasCurrentValue = true});
      schema.node(N{.id = "fill", .key = "fill", .category = "main", .title = "Tinted fill",
                    .description = "Coloured see-through faces. Turn off for outlines only.", .type = T::Toggle,
                    .defaultValue = "true", .currentValue = fillOn_ ? "true" : "false", .hasCurrentValue = true});
      if (!loadedName_.empty()) addPlacementNodes(schema);
    }
    schema.node(N{.id = "refresh", .key = "refresh", .category = "main", .title = "Refresh list",
                  .description = "Rescan " + structuresDir_.string().substr(8), .type = T::Button});
    if (!status_.empty())
      schema.node(N{.id = "status", .category = "main", .title = status_, .type = T::Info});

    if (!loadedName_.empty()) {
      size_t total = 0;
      for (const auto &m : materials_) total += m.count;
      schema.node(N{.id = "materials", .category = "materials",
                    .title = loadedName_ + "  (" + sizeText(loaded_) + ", " + std::to_string(total) + " blocks)",
                    .section = "Materials", .type = T::Info});
      // One row per block: the launcher caps a single description at 1024 characters, and it stays under
      // its 512-node limit with room for the controls above.
      constexpr size_t kMaxRows = 400;
      for (size_t k = 0; k < materials_.size() && k < kMaxRows; ++k)
        schema.node(N{.id = "m" + std::to_string(k), .category = "materials",
                      .title = std::to_string(materials_[k].count) + "   " + materials_[k].name, .section = "Materials",
                      .type = T::Info});
    }

    addSettingsNodesLocked(schema);
    addHelpNodes(schema);

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
      schema.node(N{.id = key, .key = key, .category = "place", .title = title, .section = "Placement",
                    .type = T::Button});

    const char *mode = view_.layers == View::Layers::Single ? "single"
                       : view_.layers == View::Layers::UpTo ? "upto"
                                                            : "all";
    N layers{.id = "layers", .key = "layers", .category = "place", .title = "Show", .section = "Layers",
             .type = T::Choice, .choiceStyle = mm::ConfigChoiceStyleV2::Segmented, .defaultValue = "all",
             .currentValue = mode, .hasCurrentValue = true};
    layers.options = {{.value = "all", .label = "All"}, {.value = "single", .label = "One layer"},
                      {.value = "upto", .label = "Up to layer"}};
    schema.node(std::move(layers));
    schema.node(N{.id = "layer", .key = "layer", .category = "place", .title = "Layer", .section = "Layers",
                  .type = T::SliderInt, .defaultValue = "1", .currentValue = std::to_string(view_.layer + 1),
                  .hasCurrentValue = true, .minValue = "1", .maxValue = std::to_string(loaded_.size[1]),
                  .step = "1"});
    schema.node(N{.id = "layer_up", .key = "layer_up", .category = "place", .title = "Layer up",
                  .section = "Layers", .type = T::Button});
    schema.node(N{.id = "layer_down", .key = "layer_down", .category = "place", .title = "Layer down",
                  .section = "Layers", .type = T::Button});
  }

  // The launcher doesn't restore toggle values between launches, so BE-Matica keeps its own.
  fs::path settingsFile() const { return structuresDir_.parent_path() / "settings.txt"; }

  void loadSettings() {
    std::ifstream in(settingsFile());
    for (std::string key, value; std::getline(in, key, '=') && std::getline(in, value);) {
      if (key == "easyplace") easyPlaceOn_ = value == "1";
      else if (key == "airplace") airPlaceOn_ = value == "1";
      else if (key == "hologram") hologramOn_ = value == "1";
      else if (key == "textured") texturedOn_ = value == "1";
      else if (key == "outlines") outlinesOn_ = value == "1";
      else if (key == "fill") fillOn_ = value == "1";
      else if (key == "opacity") opacity_ = std::clamp(std::atoi(value.c_str()), 10, 100);
      else if (key == "structure") savedName_ = value;
      else if (key == "origin") haveOrigin_ = std::sscanf(value.c_str(), "%d,%d,%d", &origin_[0], &origin_[1], &origin_[2]) == 3;
      else if (key == "rotation") savedView_.rotation = std::clamp(std::atoi(value.c_str()), 0, 3);
      else if (key == "mirror") savedView_.mirror = value == "1";
      else if (key == "layers") savedView_.layers = static_cast<View::Layers>(std::clamp(std::atoi(value.c_str()), 0, 2));
      else if (key == "layer") savedView_.layer = std::max(0, std::atoi(value.c_str()));
      else if (key == "locked") locked_ = value == "1";
      else if (key == "autolock") autoLockOn_ = value == "1";
      else if (key == "autolock_pct") autoLockPct_ = std::clamp(std::atoi(value.c_str()), 1, 100);
      else if (key == "remember") rememberOn_ = value == "1";
    }
  }

  void saveSettingsLocked() {
    std::ofstream(settingsFile()) << "easyplace=" << easyPlaceOn_ << "\nairplace=" << airPlaceOn_ << "\nhologram=" << hologramOn_ << "\ntextured=" << texturedOn_
                                  << "\noutlines=" << outlinesOn_ << "\nfill=" << fillOn_ << "\nopacity=" << opacity_ << "\n"
                                  << "structure=" << loadedName_ << "\norigin=" << origin_[0] << ',' << origin_[1] << ','
                                  << origin_[2] << "\nrotation=" << view_.rotation << "\nmirror=" << view_.mirror
                                  << "\nlayers=" << static_cast<int>(view_.layers) << "\nlayer=" << view_.layer
                                  << "\nlocked=" << locked_ << "\nautolock=" << autoLockOn_
                                  << "\nautolock_pct=" << autoLockPct_ << "\nremember=" << rememberOn_ << "\n";
  }

  void syncPanelLocked() { panel_.setVisible(controls_); }

  pl::mod::ModContext *ctx_ = nullptr;
  MaterialsPanel panel_;
  bool hologramOk_ = false, hologramOn_ = true, texturedOn_ = false, outlinesOn_ = true, fillOn_ = true;
  bool easyPlaceOk_ = false, easyPlaceOn_ = false, airPlaceOn_ = true;
  int opacity_ = 45;
  View view_;
  // Placement kept across launches: the structure's world origin, and the name/view read at start-up.
  std::array<int, 3> origin_{};
  bool haveOrigin_ = false, locked_ = false;
  bool autoLockOn_ = true, rememberOn_ = true;
  int autoLockPct_ = 10;
  int lastPct_ = -1;
  bool originTyped_ = false;  // build progress at the last report, -1 until the structure's first check
  std::string savedName_;
  View savedView_;
  fs::path structuresDir_;
  // Recursive: the launcher may call back into onSetting while a settings page is being set.
  std::recursive_mutex mu_;
  bool publishing_ = false;
  bool enabled_ = true;
  bool controls_ = true;
  std::vector<std::string> files_;
  std::string status_;
  Structure loaded_;
  std::string loadedName_;
  std::vector<Material> materials_;
};

}  // namespace bematica

PL_REGISTER_MOD(bematica::Mod, bematica::Mod::instance())
