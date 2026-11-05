# BE-Matica code guide

How the mod is put together, and every place where a block is treated differently on purpose. Keep this file in step
with the code: when you add a block to one of the lists below, add it here with the reason.

Target: Minecraft Bedrock 1.26.60.4, arm64 Android, loaded by LeviLaunchroid. All game addresses and struct offsets live
in `src/game.hpp`; a game update means re-checking every one of them together.

## Files

| File | What it does |
|---|---|
| `main.cpp` | The mod itself: launcher settings page, Help tab, loading structures, substitutions, saved settings (`settings.txt`), wiring the modules below together. |
| `materials_panel.*` | The floating panel BE-Matica draws itself (Move and Blocks tabs, Substitute view, round open/close button). |
| `icons.*` | Lucide SVG icons rendered with lunasvg and registered as overlay images. |
| `waila.*` | The card at the top of the screen: the block under the crosshair and what the structure wants there. |
| `hologram.*` | Hooks world rendering and draws the ghost blocks; tracks which cells are already built. |
| `mesh.*` | Turns a structure plus view (offset, rotation, mirror, layer) into quads; `expectedAt` answers "what goes in this world cell". |
| `textured.*`, `glquads.*`, `atlas.*`, `texresolve.*`, `image.*`, `zipfile.*` | Experimental textured hologram: builds a texture atlas from the installed game's own files and draws it with our own GL program. |
| `easyplace.*` | Easy Place: hooks block placement, picks the ghost cell on the view ray, checks or picks the held block, then gets the block's states (facing etc.) right. |
| `orient.hpp` | Pure functions: rotate/mirror direction states, and which blocks get their facing from the clicked face or the player's yaw. |
| `itemmap.hpp` | Pure functions: which item places a block, and which saved forms of a block the item can't place. |
| `library.*` | Structure folder listing, file loading, material counts, substitution, powered-form clean-up. |
| `mcstructure.*`, `litematic.*`, `nbt.*`, `gzip.*` | File formats. `.litematic` is converted to `.mcstructure` with `blockmap.*` (Java → Bedrock table in `data/`). |

Pure files (`orient.hpp`, `itemmap.hpp`, `library`, `mesh`, file formats) have host tests in `tests/`, run by
`scripts/test.sh`. Everything that touches the game can only be tested on a device.

## Loading a structure

1. `main.cpp showLocked` reads the file into `original_`.
2. `unpowerBlocks` (library.cpp) replaces the forms a player can't place, see [Powered forms](#powered-forms).
3. `applySubsLocked` copies it to `loaded_` and applies the player's substitutions in order. `loaded_` is what the
   hologram, Easy Place and the material list all use; the file itself is never changed.

## One Easy Place tap

All in `easyplace.cpp`, starting at `buildBlockInner` (tap on a block) or `useItemDetour` (tap on air).

1. **Target:** `findTarget` walks the view ray to the first empty cell the structure wants filled.
2. **Hand:** `holdingRight` checks the held item against `itemmap::itemFor` or the block's own item. If it is wrong,
   `autoPick` tries the hotbar, then the inventory (a move job on the server thread), then creative pick-block.
3. **Click:** `placeAt` clicks a solid neighbour's face, avoiding [interactable](#interactable-blocks) ones, or the face
   given by `nativeFace` when the game takes the facing from the clicked face.
4. **States:** after the game placed the vanilla block, the structure's states are applied in one of three ways:
   - **Clicked face** (`orient::clickFace`): wall signs, wall banners and wall heads. The right face is clicked, then the
     result is only checked, never changed.
   - **At placement** (`postFacing` → `applyFacing`): blocks whose facing comes from the player's yaw *and* that hold a
     block entity. The game's own `getPlacementBlock` is hooked and returns the wanted permutation, so the block entity
     is created for the final block. See [Facing at placement](#facing-at-placement).
   - **After placement** (`queueOrient` → `runOrient`): everything else. On the next server tick the placed block is
     swapped (`BlockSource::setBlock`) for the permutation with the structure's states: facing, repeater delay, slab
     half, and so on. The job waits up to 200 ticks for the block to appear, and logs `orient <block> ... -> ok/failed`.

## Special-case lists

### Block entities: no swap after placement

`hasBlockEntity` in `easyplace.cpp`. These blocks are **never** swapped with `setBlock` after placement.

Why: swapping a barrel's permutation is the only `setBlock` the device ran right before the server thread crashed
while saving that chunk. Until the cause is known, blocks with a block entity keep the state the game gave them.
`kOrientBlockEntities = true` turns the swap back on for testing.

What it means for players: unless the block is also in [Facing at placement](#facing-at-placement) or is a wall
sign, banner or head, it keeps whatever facing vanilla placement gives it (from where the player looks, or for a hopper
from the face Easy Place clicked). Today that affects: crafter, brewing stand,
chiseled bookshelf, jukebox, beacon, enchanting table, beds, command blocks, decorated pot, daylight detector, conduit,
bell, end portal/gateway, spawners, vault, structure block, jigsaw, sculk blocks, standing signs and banners, shulker
boxes, hanging signs, cauldron, hopper. Hopper direction is the one most likely to matter in builds.

### Facing at placement

`orient::placementHook` and the `Placement*` signatures in `game.hpp`. The bit says which of the game's
`getPlacementBlock` functions is hooked:

| Bit | Function | Blocks |
|---|---|---|
| 1 | base `BlockType::getPlacementBlock` (block traits) | chest, trapped chest, ender chest, furnace, blast furnace, smoker, lectern, campfire, soul campfire, comparator |
| 2 | `BarrelBlock` | barrel |
| 4 | `DispenserBlock` | dispenser, dropper |

Besides direction states (`isDirectionKey`), the comparator's `output_subtract_bit` is set here too, because a block
entity is never swapped afterwards. If a signature isn't found, that family logs `skipped` and keeps the vanilla facing.

To add a block: confirm its class's vtable slot 0x280 points at one of the hooked functions, then add it to
`placementHook` and to the test in `tests/orient_test.cpp`.

### Interactable blocks

`interactable` in `easyplace.cpp`: chests, furnaces, doors, trapdoors, buttons, levers, repeaters, comparators,
shulker boxes and the like. Clicking one would open or toggle it instead of placing, so `placeAt` doesn't click
against them. Where it has to, it sneaks for that one click (`useItemOnDetour`).

### Powered forms

`itemmap::kPoweredForms`. A structure saved with a running circuit holds forms the item never places:

| Saved | Loaded as |
|---|---|
| powered_repeater | unpowered_repeater |
| powered_comparator | unpowered_comparator (output_lit_bit cleared) |
| unlit_redstone_torch | redstone_torch |
| lit_redstone_lamp | redstone_lamp |

`unpowerBlocks` converts them on load. `hologram.cpp familyOf` treats either form in the world as built, because the
circuit switches between them by itself.

### Items with another name

`itemmap::itemFor`: water and lava (buckets), sugar cane, cake, cauldron, flower pot, kelp, redstone wire, repeater,
comparator, crops and stems (seeds), cocoa, every sign (the sign item picks wall or standing), every door.
Liquids are left to the game (`wantsLiquid`); BE-Matica only picks the bucket.

### Rotation and mirror

`orient::transformState` turns these states when the hologram is rotated or mirrored: `cardinal_direction`,
`facing_direction`, `weirdo_direction` (stairs), `direction` (trapdoors, beds, bells...), `coral_direction`,
`torch_facing_direction`, `lever_direction`, `pillar_axis`, `portal_axis`, `door_hinge_bit`, `ground_sign_direction`,
`rail_direction`, `vine_direction_bits`, `multi_face_direction_bits`. Any other state is copied unchanged on purpose:
the code never guesses an encoding.

## Logs

Lines are logged under the `BE-Matica` logcat tag (`adb logcat -s BE-Matica:V`). Useful lines:

- `EasyPlace build: ...` once per tap, saying why a tap did nothing.
- `EasyPlace: orient <block> <states> -> ok | failed (...)` for swaps after placement.
- `EasyPlace: native-facing <block> ... -> ok | passthrough | failed(...)` for facing at placement.
- `EasyPlace: orient <block> ... -> skipped (block entity)` for blocks on the no-swap list.
- `Hologram: x y z wants <block> ...` when a placed block doesn't match.
