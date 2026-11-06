#!/usr/bin/env bash
# Renders the overlay panel to build/preview/*.png on the desktop (no phone, no NDK). See tools/panel_preview.cpp.
# Needs: the header-only deps the Android build fetches (BEMATICA_DEPS, default build/_deps or the main checkout's),
# the Mojang bedrock-samples zip (BEDROCK_SAMPLES) and a Java-edition font sheet (JAVA_FONT) - local files, never committed.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=build/preview
mkdir -p "$OUT/obj"

DEPS="${BEMATICA_DEPS:-build/_deps}"
[[ -d "$DEPS/lunasvg-src" ]] || DEPS="$(git rev-parse --git-common-dir)/../build/_deps"
SAMPLES="${BEDROCK_SAMPLES:-/run/media/mayer/Primary/DevTools/downloads/bedrock-samples-1.26.50.4.zip}"
JAVA_FONT="${JAVA_FONT:-/run/media/mayer/Primary/DevTools/downloads/mc-java-26.3/assets/minecraft/textures/font/ascii.png}"
LUNA="$DEPS/lunasvg-src"
CXX="${CXX:-c++}"

# lunasvg (+ plutovg) for the host, built once.
if [[ ! -f "$OUT/obj/liblunasvg.a" ]]; then
  for f in "$LUNA"/plutovg/source/*.c; do
    cc -O2 -c -DPLUTOVG_BUILD_STATIC -I"$LUNA/plutovg/include" -I"$LUNA/plutovg/source" "$f" -o "$OUT/obj/pl_$(basename "$f" .c).o" &
  done
  for f in "$LUNA"/source/*.cpp; do
    "$CXX" -std=c++17 -O2 -c -DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD_STATIC -I"$LUNA/include" -I"$LUNA/plutovg/include" \
      -I"$LUNA/source" "$f" -o "$OUT/obj/ls_$(basename "$f" .cpp).o" &
  done
  wait
  rm -f "$OUT/obj/liblunasvg.a"
  ar rcs "$OUT/obj/liblunasvg.a" "$OUT"/obj/pl_*.o "$OUT"/obj/ls_*.o
fi

# The stand-in "apk": the vanilla resource pack folder with the Java font sheet as the 8x8 ASCII font, plus the UI textures.
python3 - "$SAMPLES" "$JAVA_FONT" "$OUT/fake_game.apk" <<'PY'
import sys, zipfile
samples, font, out = sys.argv[1:]
root = "assets/resource_packs/vanilla/"
want = ["dialog_background_opaque", "button_borderless_dark", "button_borderless_lightpressed",
        "button_borderless_lighthover", "edit_box_indent"]
with zipfile.ZipFile(samples) as src, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as dst:
    for n in src.namelist():
        base = n.split("/resource_pack/", 1)
        if len(base) == 2 and base[1] == "textures/terrain_texture.json":  # only so ResourcePack accepts the archive
            dst.writestr(root + base[1], '{"texture_data": {}}')
        elif len(base) == 2 and base[1].startswith("textures/ui/") and base[1].rsplit("/", 1)[1].rsplit(".", 1)[0] in want:
            dst.writestr(root + base[1], src.read(n))
    dst.write(font, root + "font/default8.png")
PY

sed "s|@BLOCK_MAP@|$PWD/data/java_to_bedrock.tsv.gz|" src/block_map_data.S.in > "$OUT/block_map_data.S"
SRC=(src/materials_panel.cpp src/skin.cpp src/skin_core.cpp src/icons.cpp src/image.cpp src/zipfile.cpp src/atlas.cpp
     src/texresolve.cpp src/nbt.cpp src/mcstructure.cpp src/library.cpp src/mesh.cpp src/gzip.cpp src/blockmap.cpp
     src/litematic.cpp "$OUT/block_map_data.S")
"$CXX" -std=c++20 -O1 -g -w -Isrc -Itools/host-stubs -Ivendor/preloader-android/include \
  -I"$DEPS/nlohmann_json-src/include" -I"$LUNA/include" -isystem "$LUNA/plutovg/source" -isystem "$LUNA/plutovg/include" \
  "${SRC[@]}" tools/panel_preview.cpp "$OUT/obj/liblunasvg.a" -lz -o "$OUT/panel_preview"

rm -f "$OUT"/*.png
# PREVIEW_APK=<a real game apk> previews with what that game version ships.
BEMATICA_GAME_APK="${PREVIEW_APK:-$PWD/$OUT/fake_game.apk}" "$OUT/panel_preview" "$OUT" skin
"$OUT/panel_preview" "$OUT" flat
