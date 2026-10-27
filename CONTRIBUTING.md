# Contributing to BE-Matica

Thanks for helping. This guide covers the one hard rule, how to build, and how to send changes.

## The clean-room rule (read this first)

BE-Matica is MIT-licensed, so every line in it must be code its author has the right to give away.

- **Do not copy code from other mods**, including closed-source ones and source that has no license. "Public on GitHub" does not mean "free to copy". No license means all rights reserved.
- **Do not commit decompiler output** (Ghidra, IDA, rea, jadx, etc.) or code translated line by line from it.
- **Do not commit Minecraft binaries or game assets.**
- **You may** use what you learn from studying the game or other mods (which function to hook, how a file format is laid out), as long as you write the code yourself. Describe findings in your PR in your own words.
- Code from projects with a compatible license (MIT, BSD, Apache-2.0, zlib) is fine. Keep their license notice and mention it in the PR.

By opening a pull request you confirm your contribution is your own work and you license it under the MIT License.

## Building

You need Linux, macOS or WSL with:

- [Android NDK r28c](https://developer.android.com/ndk/downloads)
- CMake 3.22+, Ninja, Python 3, git, `readelf` (binutils)

```bash
git clone --recursive https://github.com/M-yer/BE-Matica.git
cd BE-Matica
NDK=/path/to/android-ndk-r28c ./scripts/build.sh
```

The output is `release/BE-Matica.levipack`. The first build downloads the SDK's helper libraries (fmt, nlohmann/json, magic_enum, Boost.PFR, lunasvg). To build offline, pass local copies, for example `./scripts/build.sh -DFETCHCONTENT_SOURCE_DIR_FMT=/path/to/fmt`.

The [preloader-android SDK](https://github.com/LiteLDev/preloader-android) is a git submodule, not copied into this repo. If `vendor/preloader-android` is empty, run `git submodule update --init`.

## Unit tests

Code that doesn't touch the game (file readers, conversions) has tests that run on your computer, no NDK or phone needed:

```bash
./scripts/test.sh
```

To check the reader against your own files: `build/host-tests/mcstructure_test path/to/*.mcstructure`

## Testing on a device

1. Build, then copy `release/BE-Matica.levipack` to your phone.
2. Import it in LeviLaunchroid and enable it.
3. Read the logs over USB: `adb logcat -s BE-Matica`

## Sending changes

1. Open an issue first for anything bigger than a small fix, so we can agree on the approach.
2. Fork, then create a branch: `git checkout -b feature/layer-view`.
3. Keep each pull request to one feature or fix.
4. Make sure `./scripts/build.sh` succeeds.
5. In the PR, say what you changed, how you tested it (device and Minecraft version), and add a screenshot or clip for anything visual.

## Code style

- C++20. Match the style of the surrounding code.
- Comments explain *why*, not *what*.
- Game memory addresses and signatures break on every Minecraft update. Keep them in one place and note the Minecraft version they were found on.

## Reporting bugs

Open an issue with your device, Android version, Minecraft version, LeviLaunchroid version, steps to reproduce, and the `adb logcat -s BE-Matica` output.
