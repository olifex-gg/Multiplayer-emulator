# Multiplayer Animal Crossing (GameCube)

Goal: play the original GameCube *Animal Crossing* with friends **at the same time**,
in the same town, over the internet.

This document is the research and decision record for how to get there.

## TL;DR

**Don't build an emulator, and don't build on top of one.** The emulator was never the
bottleneck. The game itself only knows about one human player at a time. The fix has to go
into the game, and as of 2026 that is finally practical because:

1. The game has been decompiled to C: [ACreTeam/ac-decomp](https://github.com/ACreTeam/ac-decomp)
   (CC0, USA v1.0 `GAFE01`).
2. That decompilation already runs natively on PC: [flyngmt/ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port)
   (MIT port layer, OpenGL 3.3, Windows/Linux/Steam Deck, Dolphin-compatible saves).

The recommended path is to add real multiplayer **inside the native PC port**, the same way
`sm64ex-coop` did it for Mario 64 and Ship of Harkinian's Anchor mod did it for Ocarina of
Time once their decomps landed.

## Why "just use Dolphin netplay" isn't it

Dolphin's built-in netplay synchronizes *controller inputs* between copies of the same game.
Animal Crossing only reads controller port 1, so netplay gives you one town with one
character that everyone takes turns steering. That is "hot seat over the internet", not
simultaneous play. Any emulator-level netplay has this ceiling for this game.

## How the game actually models other humans

- Up to four human residents live in a town, but they exist only as **save data**. The game
  has exactly one live player actor (`PLAYER_ACTOR` in the decomp's `include/m_player.h`),
  and the camera, NPC dialogue, shops, and collision all assume it.
- Visiting a friend's town was done with a **second memory card in slot B**: the game loads
  the other town's data, you ride the train there, play as your own character, and on the
  way home both saves get written back.

That slot-B visit flow is the key. It already handles "a character from another save walks
around in this town". Multiplayer becomes: **the network replaces memory card B, plus a
second visible character.**

## Options considered

| # | Approach | Verdict |
|---|----------|---------|
| A | Dolphin netplay / shared save file | Zero work, turn taking only. Baseline, not the goal. |
| B | Stock Dolphin + external "puppet" client that reads/writes emulated RAM and injects remote players (the ModLoader64 / OoT Online model) | Possible in principle, but the game has no spare "other human" actor to write into. You would have to inject a custom actor with ASM/Gecko patches and chase heap pointers. Fragile and the hardest option now that the source exists. |
| C | **Native PC port + built-in multiplayer** | **Recommended.** You have the C source, so add a puppet actor and netcode directly. Robust, cross-platform, no memory hacking. |
| D | Write our own GameCube emulator | No. Dolphin is decades of work and would not solve the single-player problem anyway. |
| E | N64 original (*Doubutsu no Mori*) via ModLoader64 | Japanese only, a strict subset of the GC game, and ModLoader64 has no support for it. Skip. |

### Note on the existing "AC-Netplay" project

[KadeStanford/AC-Netplay](https://github.com/KadeStanford/AC-Netplay) (Python, MIT) claims
option B for Dolphin: relay server, RAM reader/writer, remote players written into "visitor
NPC slots". Treat it as **unverified**. It has no users, no screenshots or video, and its
memory map describes visitor actor tables that do not correspond to anything in the
decompiled engine. It is a useful sketch of the client/relay shape, nothing more.

## What the PC port already gives us (verified from source, Sept 2026)

- Decompilation is 99.52% complete. The port is 32-bit only because JSystem casts pointers
  to `u32`, so builds use MSYS2 MINGW32 on Windows. A Linux i686 toolchain file exists but
  Linux is unofficial.
- **Slot-B travel is already implemented on PC** in `pc/src/pc_m_card.c`, despite the README
  listing it as missing. A second GCI file in `save/` is treated as memory card B. The train
  station checks it, the visiting player's "passport" (their `Private_c` player block plus
  the departing villager) is written and read, and the visited town is saved back on return.
  The relevant entry points are `mCD_CheckStation_bg`, `mCD_SaveStation_NextLand_bg`,
  `mCD_SaveStation_Passport_bg`, `mCD_toNextLand`, and `mCD_ReCheckLoadLand`.
- Sizes that matter: a whole town save is 0x72000 bytes (about 467 KB), a passport is about
  10 KB. Sending either over the network at join time is trivial.
- The player is a singleton by construction: `GET_PLAYER_ACTOR_NOW()` returns the first
  actor in the `ACTOR_PART_PLAYER` list, used from 36 files. Actors are spawned through
  `Actor_info_make_actor(profile_no, ...)`, with 200 actor slots across 8 parts.
- The player draw code in `src/game/m_player_draw.c_inc` is data driven from the actor and
  never touches the current save. Model, face, clothing, and palette selection in
  `src/game/m_player_lib.c` do read the global `Now_Private`, in roughly ten small
  functions. That is the one refactor a second visible human needs.
- Once-per-frame hook point for a network tick: `VIWaitForRetrace` in `pc/src/pc_vi.c`,
  which already pumps SDL events.
- The maintainer's FAQ says online play is **not planned** and raises security concerns.
  So this is a fork, with foundation fixes offered upstream. The security point is valid and
  is designed for below.

## Plan

### Step 0. Set up the fork

Import ACGC-PC-Port into this repository as the base (add it as `upstream`, merge its
history) so upstream fixes can be pulled with a plain merge. Vendor ENet under `pc/lib/`
next to `glad` and `fixnes`, matching how the port already vendors libraries. Reproduce the
MSYS2 MINGW32 build and confirm a slot-B visit works locally with two GCI files.

### Step 1. Networked memory card B

Replace the "second GCI in `save/`" source with a network source behind the same functions
in `pc_m_card.c`. Host mode serves its town save on join. Visitor mode receives it into the
card-B buffer, so the existing station code sees a valid travel destination and the visitor
arrives by train exactly as today. Passport goes visitor to host. Return trip sends the
visitor's updated passport home and the host's town save-back stays local. Milestone: two
PCs, one visitor riding into the host's town over the network, with no second visible
character yet.

### Step 2. Puppet actor

New actor profile, `mAc_PROFILE_PUPPET`, registered in the NPC part so nothing that looks
up the player finds it. Its struct embeds `PLAYER_ACTOR` so `Player_actor_draw_Normal` and
the animation code work unchanged. Give the model, face, cloth, and palette lookups in
`m_player_lib.c` a `Private_c*` parameter with the old global as the default, and pass the
remote player's passport for puppets. Each client sends its own player state at 20 to 30 Hz:
position, facing, animation index and frame, held item, emote. Interpolate on receive.
Milestone: host and visitor see each other walk around.

### Step 3. Host-authoritative world events

Anything that changes the town is an event validated by the host: pick up, drop, tree
shake, rock hit, dig, fill, bells, furniture, gate. Visitors apply the host's decision.
Villager positions, schedules, and dialogue locks stream host to visitors. Host clock is the
world clock. Puppets are only drawn in the same room or acre.

### Step 4. Chat, then three and four players

Chat reuses the game's keyboard text entry, sanitized to the game's character set on
receive. Extend puppet slots and passports to four players. Add an optional relay server
for players behind NAT.

### Security design (why RAM injection was the wrong tool)

Because all sync goes through explicit, typed messages, the host validates every byte:
checksum and bounds-check the passport, reject impossible positions and item ids, never
apply raw memory writes. The visitor never has write access to the host's town except
through validated events. This is the answer to the upstream FAQ's concern.

### Out of scope

Dolphin support, the N64 version, e-Reader and GBA link, and any distribution of game data.

## Sources

- ac-decomp: https://github.com/ACreTeam/ac-decomp
- ACGC-PC-Port: https://github.com/flyngmt/ACGC-PC-Port
- AC-Netplay: https://github.com/KadeStanford/AC-Netplay
- ModLoader64: https://github.com/hylian-modding/ModLoader64
- Dolphin Memory Engine (Python): https://pypi.org/project/dolphin-memory-engine/
