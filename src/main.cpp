#include "blockmap.hpp"
#include "easyplace.hpp"
#include "hologram.hpp"
#include "textured.hpp"
#include "library.hpp"
#include "litematic.hpp"
#include "materials_panel.hpp"
#include "waila.hpp"

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
constexpr const char *kLookModule = "bematica.lookat";
constexpr const char *kPanelButton = "bematica.panel";
// Lucide "box" (ISC), white so it shows on the launcher's dark keycap.
constexpr const char *kPanelButtonSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" )"
    R"(stroke-linecap="round" stroke-linejoin="round"><path d="M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 )"
    R"(0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z"/><path d="m3.3 7 8.7 5 8.7-5"/>)"
    R"(<path d="M12 22V12"/></svg>)";

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
                    applyLookLocked();
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
    // Floating button that shows and hides the panel (same as switching BE-Matica Controls). The launcher only
    // collects buttons when the game starts, so it is registered here.
    ok = mm::ButtonBuilder(kPanelButton, "BE-Matica panel")
             .moduleId(kModule)  // not the Controls module: the button must stay while the panel is hidden
             .modId(ctx.id())
             .label("BE")
             .svgIcon(kPanelButtonSvg)
             .onEvent([this](std::string_view, mm::ButtonEvent event, float) {
               if (event != mm::ButtonEvent::Click) return;
               bool on;
               {
                 std::lock_guard lock(mu_);
                 on = controls_ = !controls_;
                 syncPanelLocked();
               }
               mm::setModuleEnabled(kControlsModule, on);
             })
             .registerButton();
    if (!ok) ctx.logger().warn("BE-Matica: panel button registration failed");
    ok = mm::ModuleBuilder(kLookModule, "BE-Matica Look-at")
             .description("Card naming the block under the crosshair.")
             .modId(ctx.id())
             .defaultEnabled(true)
             .hideInHudEditor(true)
             .registerModule();
    if (!ok) ctx.logger().warn("BE-Matica: look-at module registration failed");
    waila_.init(kLookModule);
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
    textured::setCacheDir(structuresDir_.parent_path() / "texture_cache");
    textured::setEnabled(texturedOn_);
    applyLookLocked();
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
    easyplace::setAutoSelect(autoSelectOn_);
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
    hologram::setLookListener(nullptr);
    waila_.update({});
    panel_.shutdown();
    hologram::uninstall();
    easyplace::uninstall();
    mm::clearConfigSchema(kModule);
    mm::unregisterModule(kModule);
    mm::unregisterButton(kPanelButton);
    mm::unregisterModule(kControlsModule);
    mm::unregisterModule(kLookModule);
    ctx.logger().info("BE-Matica disabled");
    return true;
  }

private:
  void onSetting(std::string_view key, std::string_view value) {
    if (key == "import") return importFile();
    std::lock_guard lock(mu_);
    if (publishing_) return;  // the launcher echoing values from the page we are setting
    if (onSubstituteLocked(key, value)) return;
    if (key == "file") {
      // The launcher can send the chosen file again on its own; reloading would drop substitutions.
      if (value == loadedName_) return;
      loadLocked(std::string(value));
    } else if (key == "refresh") {
      scanLocked();
      status_ = files_.empty() ? "No structures yet" : std::to_string(files_.size()) + " structures found";
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
    } else if (key == "autoselect") {
      autoSelectOn_ = value == "true" || value == "1";
      easyplace::setAutoSelect(autoSelectOn_);
      saveSettingsLocked();
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
    } else if (key == "lookat") {
      lookOn_ = value == "true" || value == "1";
      applyLookLocked();
      saveSettingsLocked();
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
    } kNudges[] = {{"up", 0, 1, 0}, {"down", 0, -1, 0}};
    // The D-pad follows the player's view: x is to the right, z is forward.
    static constexpr struct {
      const char *key;
      int right, forward;
    } kViewNudges[] = {{"forward", 0, 1}, {"back", 0, -1}, {"left", -1, 0}, {"right", 1, 0}};
    const bool moves = key == "rotate" || key == "mirror" ||
                       std::any_of(std::begin(kNudges), std::end(kNudges), [&](const auto &n) { return key == n.key; }) ||
                       std::any_of(std::begin(kViewNudges), std::end(kViewNudges), [&](const auto &n) { return key == n.key; });
    if (moves && locked_) {
      panel_.toast("Placement is locked");
      return true;
    }
    for (const auto &n : kNudges)
      if (key == n.key) {
        hologram::nudge(n.dx, n.dy, n.dz);
        return true;
      }
    for (const auto &n : kViewNudges)
      if (key == n.key) {
        hologram::nudgeView(n.forward, n.right);
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
    schema.node(N{.id = "lookat", .key = "lookat", .category = "settings", .title = "Look-at info",
                  .description = "Name the block under the crosshair, and what the structure wants in its place.",
                  .type = T::Toggle, .defaultValue = "true", .currentValue = flag(lookOn_), .hasCurrentValue = true});
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
         "Move tab: the D-pad shifts the hologram one block the way you're facing (up = forward, left = your left), the \u25B2 / \u25BC "
         "pair moves it up and down, Rotate +90\u00B0 turns it, Mirror flips it, "
         "< and > pick a single layer, All shows every layer.\nBlocks tab: every block the structure needs; tap a "
         "row to tick it off."},
        {"Substitute a block",
         "Use another block where the type doesn't matter, e.g. cobblestone for 500 smooth stone.\n"
         "In the panel's Blocks tab, tap \u22EF on a row. Type part of a name and tap a result, or tap Held block and "
         "then tap any block while holding the replacement (that tap places nothing).\nEvery block of that type is "
         "replaced: hologram, Easy Place and counts follow. Substituted rows show *; tap \u22EF again for Undo. "
         "The structure file is not changed. Substitutions stay until you load another structure."},
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
    original_ = std::move(s);
    loadedName_ = name;
    // Substitutions belong to one placement: only a restored placement (`view` given) keeps them.
    if (!view) subs_.clear();
    originalNames_.clear();
    for (const auto &m : countMaterials(original_)) originalNames_.push_back(m.name);
    applySubsLocked();
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

  // loaded_ = the structure as read, with every substitution applied in order (so a -> b then b -> c makes both c).
  void applySubsLocked() {
    loaded_ = original_;
    const auto &blocks = BlockMap::bedrockBlocks();
    for (const auto &[from, to] : subs_) {
      auto it = std::find_if(blocks.begin(), blocks.end(), [&](const BlockState &b) { return b.name == "minecraft:" + to; });
      // A held item missing from the table (a newer block) is used with no states.
      substituteBlock(loaded_, from,
                      it != blocks.end() ? *it
                                         : BlockState{"minecraft:" + to, nbt::makeCompound(),
                                                      blocks.empty() ? 0 : blocks.front().version});
    }
    materials_ = countMaterials(loaded_);
    for (auto &m : materials_) m.was = wasLocked(m.name);
  }

  // Shows the substituted structure in place: same origin and view, Easy Place and progress follow it.
  void resubLocked() {
    applySubsLocked();
    panel_.setContent(loadedName_.substr(0, loadedName_.rfind('.')), materials_);
    lastPct_ = -1;
    hologram::show(std::make_shared<const Structure>(loaded_), false);
    saveSettingsLocked();
    publishLocked();
  }

  void addSubLocked(const std::string &from, std::string to) {
    if (to.rfind("minecraft:", 0) == 0) to.erase(0, 10);
    if (to.empty() || to == from) return;
    subs_.emplace_back(from, to);
    resubLocked();
    size_t now = 0;
    for (const auto &m : materials_)
      if (m.name == to) now = m.count;
    ctx_->logger().info("BE-Matica: substitute {} -> {} ({} {} now)", from, to, now, to);
    panel_.toast(from + " \u2192 " + to);
  }

  // The original blocks that now show as `name`, e.g. "smooth_stone" for a cobblestone that replaced it.
  std::string wasLocked(const std::string &name) const {
    std::string was;
    for (const auto &orig : originalNames_) {
      std::string now = orig;
      for (const auto &[from, to] : subs_)
        if (now == from) now = to;
      if (now == name && orig != name) was += (was.empty() ? "" : ", ") + orig;
    }
    return was;
  }

  // The panel's Substitute view. Returns false if `key` isn't one of its keys.
  bool onSubstituteLocked(std::string_view key, std::string_view value) {
    if (key == "substitute") {
      const size_t gt = value.find('>');
      if (gt != std::string_view::npos) addSubLocked(std::string(value.substr(0, gt)), std::string(value.substr(gt + 1)));
    } else if (key == "sub_held") {
      if (!easyPlaceOk_) {
        panel_.toast("Use held block needs Easy Place's hooks, which did not load");
        return true;
      }
      panel_.toast("Tap any block while holding the replacement");
      easyplace::captureHeldOnce([this, from = std::string(value)](const std::string &item) {
        std::lock_guard lock(mu_);
        ctx_->logger().info("BE-Matica: held block for {}: '{}'", from, item);
        if (item.empty()) return panel_.toast("Your hand was empty: nothing substituted");
        addSubLocked(from, item);
      });
    } else if (key == "sub_undo") {
      for (auto it = subs_.rbegin(); it != subs_.rend(); ++it)
        if (it->second == value) {
          ctx_->logger().info("BE-Matica: undo substitute {} -> {}", it->first, it->second);
          subs_.erase(std::next(it).base());
          break;
        }
      resubLocked();
    } else {
      return false;
    }
    return true;
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
      schema.node(N{.id = "textured", .key = "textured", .category = "main", .title = "Real block textures (experimental)",
                    .description = "Draw the ghost blocks near you (about 8 blocks) with the game's own models and "
                                   "textures. Turn off if anything looks wrong.",
                    .type = T::Toggle, .defaultValue = "false", .currentValue = texturedOn_ ? "true" : "false",
                    .hasCurrentValue = true});
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
        schema.node(N{.id = "autoselect", .key = "autoselect", .category = "easyplace", .title = "Auto-select block",
                      .description = "Select or pick the block a ghost block needs into an empty hotbar slot.",
                      .type = T::Toggle, .defaultValue = "true", .currentValue = autoSelectOn_ ? "true" : "false",
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
                      .title = std::to_string(materials_[k].count) + "   " + materials_[k].name,
                      .description = materials_[k].was.empty() ? "" : "Substituted for " + materials_[k].was,
                      .section = "Materials", .type = T::Info});
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
    // Same controls and directions as the panel's Move tab: the D-pad, the Y pair, then rotate and mirror.
    static constexpr const char *kMoves[][3] = {
        {"forward", "\u25B2  Forward", "D-pad (follows your view)"}, {"left", "\u25C0  Left", "D-pad (follows your view)"},
        {"right", "\u25B6  Right", "D-pad (follows your view)"},     {"back", "\u25BC  Back", "D-pad (follows your view)"},
        {"up", "\u25B2  Raise  (+Y)", "Height (Y)"},            {"down", "\u25BC  Lower  (-Y)", "Height (Y)"},
        {"rotate", "Rotate +90\u00B0", "Turn"},                 {"mirror", "Mirror", "Turn"}};
    for (const auto &m : kMoves)
      schema.node(N{.id = m[0], .key = m[0], .category = "place", .title = m[1], .section = m[2],
                    .type = T::Button, .disabled = locked_});

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
      else if (key == "autoselect") autoSelectOn_ = value == "1";
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
      else if (key == "lookat") lookOn_ = value == "1";
      else if (key == "subs")
        for (size_t start = 0; start < value.size();) {
          const size_t end = std::min(value.find(';', start), value.size()), gt = value.find('>', start);
          if (gt < end) subs_.emplace_back(value.substr(start, gt - start), value.substr(gt + 1, end - gt - 1));
          start = end + 1;
        }
    }
  }

  void saveSettingsLocked() {
    std::ofstream(settingsFile()) << "easyplace=" << easyPlaceOn_ << "\nairplace=" << airPlaceOn_ << "\nautoselect=" << autoSelectOn_ << "\nhologram=" << hologramOn_ << "\ntextured=" << texturedOn_
                                  << "\noutlines=" << outlinesOn_ << "\nfill=" << fillOn_ << "\nopacity=" << opacity_ << "\n"
                                  << "structure=" << loadedName_ << "\norigin=" << origin_[0] << ',' << origin_[1] << ','
                                  << origin_[2] << "\nrotation=" << view_.rotation << "\nmirror=" << view_.mirror
                                  << "\nlayers=" << static_cast<int>(view_.layers) << "\nlayer=" << view_.layer
                                  << "\nlocked=" << locked_ << "\nautolock=" << autoLockOn_
                                  << "\nautolock_pct=" << autoLockPct_ << "\nremember=" << rememberOn_
                                  << "\nlookat=" << lookOn_ << "\nsubs=";
    std::ofstream(settingsFile(), std::ios::app) << [&] {
      std::string out;
      for (const auto &[from, to] : subs_) out += (out.empty() ? "" : ";") + from + ">" + to;
      return out + "\n";
    }();
  }

  void syncPanelLocked() { panel_.setVisible(controls_); }

  // The look-at card reads the block under the crosshair only while it is on and BE-Matica is enabled.
  void applyLookLocked() {
    if (lookOn_ && enabled_) {
      hologram::setLookListener([this](const hologram::LookInfo &info) { waila_.update(info); });
    } else {
      hologram::setLookListener(nullptr);
      waila_.update({});
    }
  }

  pl::mod::ModContext *ctx_ = nullptr;
  MaterialsPanel panel_;
  LookCard waila_;
  bool lookOn_ = true;
  bool hologramOk_ = false, hologramOn_ = true, texturedOn_ = false, outlinesOn_ = true, fillOn_ = true;
  bool easyPlaceOk_ = false, easyPlaceOn_ = false, airPlaceOn_ = true, autoSelectOn_ = true;
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
  Structure original_;  // the structure as read from its file
  Structure loaded_;    // original_ with the substitutions applied: what the hologram and Easy Place use
  std::vector<std::pair<std::string, std::string>> subs_;  // block -> replacement, without "minecraft:", in order
  std::vector<std::string> originalNames_;  // block names in original_, for "Substituted for ..."
  std::string loadedName_;
  std::vector<Material> materials_;
};

}  // namespace bematica

PL_REGISTER_MOD(bematica::Mod, bematica::Mod::instance())
