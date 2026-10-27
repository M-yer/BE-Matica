#!/usr/bin/env bash
# Developer shortcut: installs release/BE-Matica.levipack into LeviLauncher on a USB-connected phone.
# Closes LeviLauncher first (that quits a running game), then asks it to import the pack.
set -euo pipefail
cd "$(dirname "$0")/.."
ADB="${ADB:-adb}"
PKG="${LAUNCHER_PACKAGE:-org.levimc.launcher}"
# The launcher can always read its own folder; it can't read Download without "All files access".
DEST="/sdcard/Android/data/$PKG/files/BE-Matica.levipack"
"$ADB" shell am force-stop "$PKG"
"$ADB" push release/BE-Matica.levipack "$DEST" >/dev/null
"$ADB" shell am start -a android.intent.action.VIEW -d "file://$DEST" -t application/zip -p "$PKG" >/dev/null
echo "Sent to $PKG: confirm the import on the phone if it asks, then start the game."
