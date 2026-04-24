# CC-SurvivalTest ("+survival")

`CC-SurvivalTest` is a ClassiCube plugin that adds Survival Test-based functionality on top of the client.

- Survival and creative gamemode switching
- Health + food, drowning, lava damage, fall damage, death, and respawn
- Progressive block breaking instead of instant break
- A survival inventory UI crafting/transmutation
- Death messages and a game-over screen
- Per-world persistence for health, score, inventory, position, and gamemode

The plugin also reacts to server-provided hack/MOTD flags so a world can force or disable parts of the survival layer.

## Commands

- `/gamemode survival|creative`
  Switches the local client between survival and creative, unless the server/world forces a gamemode.
- `/inv`
  Opens or closes the survival inventory UI when survival inventory is enabled.
- `/ccst status`
  Shows the current survival-layer state, including effective gamemode, health, score, and persistence status.
- `/ccst force`
  Forces restore of the most likely persisted world save candidate.
- `/ccst reset`
  Resets the saved state for the current world to the player's current state.

## Server policy tokens

The plugin parses survival-related flags from the server MOTD / hack-permissions string and adjusts behavior accordingly.

Supported tokens include:

- `+survival | -creative`
  Force survival mode.
- `+creative | -survival`
  Force creative mode.
- `+god`
  Enables god-mode semantics for the parsed policy state. No damage taken
- `+mine` / `-mine`
  Forces mining on or off.
- `-inv`
  Disables the survival inventory.
- `+heal` / `-heal`
  Forces healing/food behavior on or off.
- `+combat` / `-combat`
  Forces combat behavior on or off.
- `-deathmsg`
  Disables relayed death messages.

## Build requirements

- A C compiler
- The ClassiCube source tree (targets 1.3.8)
- `make`

By default the build expects the ClassiCube source directory at:

```text
ClassiCube/src
```

## Build

Clone ClassiCube into the active directory... `ClassiCube/` folder should be next to `src/`.

```sh
make
```

The output will be written to the `plugins` directory.

If ClassiCube is in a different location:

```sh
make CLASSICUBE_SRC=/path/to/ClassiCube/src
```

## Notes

- This is a client-side gameplay layer. No explicit communication with the server for anticheat/player interaction is made, excluding MOTD flags.
- The plugin appends `+survival` to the appname when connected to multiplayer servers.
- A lot of this plugin's "weight" is reimplementing methods that are not exposed by CC_API | CC_VAR, or circumventing typical plugin restrictions.
- Generative AI was used to assist with specific implementations (notably persistent saves based on world detection, transmute rules/scoring, p2p communication specs) The "design" of these systems was created by me (i.e. measuring texture(s) statistics + block volume, or encoding world data in multiple frequencies using spectral analysis), but I do not hold the specific implementations to be my own.
- Implementation made with lots of reference to REAL Minecraft client code and assets, particularly c0.28, b1.7.3, and r1.8.9 MCP.

## LICENSE & ATTRIBUTION

This project is licensed under the **GNU General Public License v3.0 (GPLv3)**. 

### Third-Party & Derivative Works
- **ClassiCube:** This plugin is a derivative of the ClassiCube project. Copyright (c) 2014 - 2024, UnknownShadow200
- **Implementation Logic:** Certain gameplay mechanics were implemented with the assistance of generative AI based on original designs by the author.
- **References:** Gameplay logic and constants are based on reimplementations of historical Minecraft versions. 

See the `LICENSE` file for the full legal text.
