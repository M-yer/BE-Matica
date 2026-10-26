# BE-Matica

An open-source, Litematica-style schematic mod for **Minecraft Bedrock Edition on Android**, loaded by [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid).

Load a saved build, see it as a ghost preview in your world, and build it block by block.

> **Status: early.** The mod currently loads, registers with the launcher and logs its lifecycle. No preview yet; see the roadmap.

## Roadmap

- [x] Mod skeleton that loads in LeviLaunchroid
- [ ] Read `.mcstructure` files (Bedrock structure block exports)
- [ ] Ghost preview of the structure in the world
- [ ] Move, rotate and mirror the preview
- [ ] Layer view (show one Y level at a time)
- [ ] Material list: what blocks you need and what you're missing
- [ ] Highlight missing and wrong blocks
- [ ] **Easy Place**: tap a ghost block to place the correct block with the correct rotation/state, like Litematica's Easy Place mode
- [ ] Java formats: `.litematic`, `.schem`, `.schematic`, `.nbt`

## Install

1. Install [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid/releases) and the official Google Play Minecraft.
2. Download `BE-Matica.levipack` from [Releases](../../releases).
3. Import it in LeviLaunchroid's mod manager and enable it.

Only arm64 (64-bit ARM) Android devices are supported.

## Building

See [CONTRIBUTING.md](CONTRIBUTING.md#building).

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first, especially the **clean-room rule**.

## License

[MIT](LICENSE). Not affiliated with Mojang, Microsoft, LiteLDev or the Litematica authors.
