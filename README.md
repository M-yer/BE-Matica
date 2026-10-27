# BE-Matica

An open-source, Litematica-style schematic mod for **Minecraft Bedrock Edition on Android**, loaded by [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid).

Load a saved build, see it as a ghost preview in your world, and build it block by block.

> **Status: early but usable.** Preview, layers, material list and Easy Place work on a test device; expect rough edges.

## Roadmap

- [x] Mod skeleton that loads in LeviLaunchroid
- [x] Read `.mcstructure` files (Bedrock structure block exports)
- [x] Ghost preview of the structure in the world
- [x] Move, rotate and mirror the preview
- [x] Layer view (show one Y level at a time)
- [x] Material list: what blocks you need, with a tick-off list on screen
- [ ] Highlight missing and wrong blocks
- [x] **Easy Place**: tap a ghost block to place the correct block, including in mid-air (Air build)
- [ ] Easy Place: matching rotation/state, not just the block
- [x] Java `.litematic` import: converted to `.mcstructure` (blocks only; chest contents and entities are not carried over)
- [ ] Other Java formats: `.schem`, `.schematic`, `.nbt`

## Install

1. Install [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid/releases) and the official Google Play Minecraft.
2. Download `BE-Matica.levipack` from [Releases](../../releases).
3. Import it in LeviLaunchroid's mod manager and enable it.

Only arm64 (64-bit ARM) Android devices are supported.

## Building

See [CONTRIBUTING.md](CONTRIBUTING.md#building).

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first, especially the **clean-room rule**.

## Credits

- Java → Bedrock block table: [PrismarineJS/minecraft-data](https://github.com/PrismarineJS/minecraft-data) (MIT), see [data/README.md](data/README.md).
- Sample `.litematic` test files: [schematic4j](https://github.com/SandroHc/schematic4j) (MIT), see [tests/data/litematic](tests/data/litematic).

## License

[MIT](LICENSE). Not affiliated with Mojang, Microsoft, LiteLDev or the Litematica authors.
