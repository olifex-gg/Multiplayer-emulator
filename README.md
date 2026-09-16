> **Multiplayer fork.** This repository is a fork of
> [flyngmt/ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port) that adds a shared,
> server-hosted town for up to four residents. The design, decisions, and status live in
> [docs/MULTIPLAYER.md](docs/MULTIPLAYER.md). The server lives in [`server/`](server/).
> Everything below is the upstream port's README and still applies to building the game.

# Animal Crossing PC Port

A native PC port of Animal Crossing (GameCube) built on top of the [ac-decomp](https://github.com/ACreTeam/ac-decomp) decompilation project.

The game's original C code runs natively on x86, with a custom translation layer replacing the GameCube's GX graphics API with OpenGL 3.3.

This repository does not contain any game assets or assembly whatsoever. An existing copy of the game is required.

Supported versions: GAFE01_00: Rev 0 (USA)

## Quick Start (Pre-built Release)

Pre-built releases are available on the [Releases](https://github.com/flyngmt/ACGC-PC-Port/releases) page. No build tools required.

1. Download and extract the latest release zip
2. Place your disc image in the `rom/` folder
3. Run `AnimalCrossing.exe`

The game reads all assets directly from the disc image at startup. No extraction or preprocessing step is needed.

## Playing online (this fork)

Up to four people play as the four residents of one shared town at the same time.
Everyone needs the same `AnimalCrossing.exe`, `AnimalCrossingOnline.exe`, `shaders/` and
their own disc image in `rom/`.

1. One person clicks **Host a town** in `AnimalCrossingOnline.exe` and shares the invite
   code (friends on the same Wi-Fi use the address the launcher shows; friends elsewhere
   need the host's public IP and UDP port 7777 forwarded to the host's PC).
2. Everyone else clicks **Join a town**, enters the code and address, and picks a resident
   name. The waiting room shows who lives in the town and who is online.
3. In the game: **T** opens the chat line, **Enter** sends, **Esc** cancels. What you say
   appears in the same speech window the villagers talk in, with your name on the tag,
   typed out letter by letter, in your game and in everyone else's. The window is only as
   tall as the message (up to four lines; a longer one carries on in a second window after
   two seconds, up to 200 characters) and closes by itself; nobody is frozen while it is up,
   and a villager who talks to you always gets the window first.

What is shared: the town itself (trees, flowers, dropped items, holes, buried things),
every resident's character and house, the weather and the calendar (the host's clock is
the town's clock), and the villagers, who stand in the same places for everyone. You see
the other residents walk, run, emote, hold their tools and umbrellas, and talk to
villagers, and you cannot walk through each other. Saving is safe: the server keeps each
resident's own character and house, so nobody can overwrite anybody else's.

Every game writes `aclog.txt` next to the exe; send it along when something goes wrong.

## Building from Source

Only needed if you want to modify the code. Otherwise, use the [pre-built release](https://github.com/flyngmt/ACGC-PC-Port/releases) above.

### Requirements

- **MSYS2** (https://www.msys2.org/)
- **Animal Crossing (USA) disc image** (ISO, GCM, or CISO format)

### MSYS2 Packages

Open **MSYS2 MINGW32** from your Start menu and install:

```bash
pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-SDL2 mingw-w64-i686-make
```

### Build Steps

1. Clone the repository:
   ```bash
   git clone https://github.com/flyngmt/ACGC-PC-Port.git
   cd ACGC-PC-Port
   ```

2. Build (from **MSYS2 MINGW32** shell):
   ```bash
   ./build_pc.sh
   ```

3. Place your disc image in the `rom/` folder:
   ```
   pc/build32/bin/rom/YourGame.ciso
   ```

4. Run:
   ```bash
   pc/build32/bin/AnimalCrossing.exe
   ```

## Controls

Keyboard bindings are customizable via `keybindings.ini` (next to the executable). Mouse buttons (Mouse1/Mouse2/Mouse3) can also be assigned.

### Keyboard (defaults)

| Key | Action |
|-----|--------|
| WASD | Move (left stick) |
| Arrow Keys | Camera (C-stick) |
| Space | A button |
| Left Shift | B button |
| Enter | Start |
| X | X button |
| Y | Y button |
| Q / E | L / R triggers |
| Z | Z trigger |
| I / J / K / L | D-pad (up/left/down/right) |

### Gamepad

SDL2 game controllers are supported with automatic hotplug detection. Button mapping follows the standard GameCube layout.

## Command Line Options

| Flag | Description |
|------|-------------|
| `--verbose` | Enable diagnostic logging |
| `--no-framelimit` | Disable frame limiter (unlocked FPS) |
| `--model-viewer [index]` | Launch debug model viewer (structures, NPCs, fish) |
| `--time HOUR` | Override in-game hour (0-23) |

## Settings

Graphics settings are stored in `settings.ini` and can be edited manually or through the in-game options menu:

- Resolution (up to 4K)
- Fullscreen toggle
- VSync
- MSAA (anti-aliasing)
- Texture Loading/Caching (No need to enable if you aren't using a texture pack)

## Texture Packs

Custom textures can be placed in `texture_pack/`. Dolphin-compatible format (XXHash64, DDS).

I highly recommend the following texture pack from the talented artists of Animal Crossing community.

[HD Texture Pack](https://forums.dolphin-emu.org/Thread-animal-crossing-hd-texture-pack-version-23-feb-22nd-2026)

## Save Data

Save files are stored in `save/` using the standard GCI format, compatible with Dolphin emulator saves. Place a Dolphin GCI export in the save directory to import an existing save.

## Credits

This project would not be possible without the work of the [ACreTeam](https://github.com/ACreTeam) decompilation team. Their complete C decompilation of Animal Crossing is the foundation this port is built on.

## AI Notice

AI tools such as Claude were used in this project (PC port code only).

## FAQ

See [FAQ](FAQ.md) for more info.
