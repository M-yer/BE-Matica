"""Zip a built mod library and its manifest into a .levipack, after basic sanity checks.

Usage: package.py <lib.so> <manifest.json> <out.levipack>
"""
import json
import subprocess
import sys
import zipfile
from pathlib import Path

lib, manifest_path, out = map(Path, sys.argv[1:4])
manifest = json.loads(manifest_path.read_text())
data = lib.read_bytes()

if data[:4] != b"\x7fELF" or int.from_bytes(data[18:20], "little") != 0xB7:
    sys.exit(f"{lib} is not an arm64 ELF library")
if manifest.get("type") != "preload-native" or manifest.get("entry") != lib.name:
    sys.exit("manifest.json must have type 'preload-native' and entry matching the library name")
symbols = subprocess.run(["readelf", "--dyn-syms", "-W", str(lib)], capture_output=True, text=True).stdout
if "PLGetModRegistration" not in symbols:
    sys.exit("PLGetModRegistration is not exported; the launcher cannot load this mod")

out.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(manifest_path, "manifest.json")
    z.write(lib, lib.name)
print(f"Packed {out} ({out.stat().st_size} bytes)")
