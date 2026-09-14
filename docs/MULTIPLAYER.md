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

## Chosen route: one shared town, hosted on a server

Decision (Sept 2026): all players are **residents of one town**, using the four resident
slots the game already has, and the town lives on a **server** rather than on any player's
PC. The earlier visit-and-passport model stays documented at the bottom as a possible
later add-on for guests, but it is not the build target.

### Why this is not more work than the visit model

- The game already has everything the resident model needs: four `private_data` slots and
  four houses in one save, and a player select screen that loads any resident with
  `mSDI_INIT_MODE_FROM`. No passport or travel machinery is used at all.
- The server does **not** run the game. The engine only simulates the acres around a
  player, so a headless copy of the game on the server could not simulate the whole town
  anyway, and it is not needed: the game already computes everything that happened while
  nobody was around when a save is loaded (`mSDI_StartDataInit` renews time, growth,
  season, mail). A town that nobody is in does not need to tick.
- The server is therefore a small ordinary program: a save vault, a lobby, a packet relay,
  a sequencer for conflicting actions, and the town clock. A few thousand lines in Go or
  Rust, one binary, one cheap VPS.

### What a town is on disk

One contiguous `Save_t` block of 0x72000 bytes (the GCI payload) plus three side blocks the
GameCube kept in ARAM: mail, original designs, diary. Inside `Save_t` the four resident
blocks sit at fixed offsets (0x20 plus 0x2440 per slot) and houses at 0x9CE8, so the server
can splice per-resident data by offset without understanding the game.

### Authority model

- **Your resident block is yours.** Each client is authoritative for its own
  `private_data` slot and house. The server always splices the latest copy from that
  client into the stored town.
- **Land data converges two ways.** Player actions that change the town (pick up, drop,
  shake, dig, plant, bury, mail, gate, furniture) are events the server orders and
  broadcasts, so everyone sees them immediately. Everything else (villager walks, weeds,
  day change, shop stock) is covered by a periodic delta of the land block from the
  **world authority**, the oldest connected client, which the server elects and migrates
  when that client leaves.
- **Actors are owned per acre.** Whichever client has an acre loaded owns the villagers in
  it and streams their positions. Ties go to the lowest client id. Other clients only
  display them. This is the sm64ex-coop ownership pattern.
- **Server clock is town clock.** Every client offsets its in-game clock to the server's,
  so shop hours and villager schedules agree. The port already has a time override.
- **Join consistency.** The first client to join loads the vault copy and runs the
  normal load-time catch-up. Later joiners receive a live snapshot from the world
  authority and skip catch-up, so two clients never invent two different versions of the
  same missed week.
- **Persistence.** The world authority uploads a delta every few seconds and a full
  snapshot on every in-game save and on leave. A crash loses at most a few seconds.

### Limits

- Four players per town. That is the engine's resident limit. More than four means a
  second town on the same server, or guest visitors via the passport model later.
- Trust is at the friend-group level. The server bounds-checks and rate-limits, and a
  client can only ever write its own resident block directly, but deep rule validation
  would need the game logic and is out of scope. Invite codes, not public listing.

### Plan

**Step 0. Set up the fork.** Import ACGC-PC-Port into this repository with upstream
tracking, vendor ENet under `pc/lib/`, reproduce the MSYS2 MINGW32 build. Start the server
project alongside it. Windows-only code in the port is about fifteen lines across six
files, so a Linux client build is plausible later.

**Step 1. Server vault and resident login.** The client gets a "connect to server" path
next to its local save path: on login it downloads the town snapshot into the same buffers
the port fills from a GCI file, then the existing player select runs with the server
enforcing one human per resident slot. Saving uploads. Milestone: two people load the same
town from the server as two different residents. No sync yet, last save wins.

**Step 2. Puppet actor.** Unchanged from before: a new actor profile embedding
`PLAYER_ACTOR`, registered outside the player slot, with the model, face, cloth, and
palette lookups in `m_player_lib.c` taking a `Private_c*`. Puppets are built from the other
residents' blocks, which every client already has in its own copy of the save. Milestone:
residents see each other walk around.

**Step 3. Convergence.** Player action events, acre ownership for villagers, periodic land
delta from the world authority, per-resident splicing on the server, join consistency,
authority migration. This is the hardest step of the project in any route and the
dedicated server does not make it harder. Milestone: four residents in one coherent town
for an evening with no visible drift.

**Step 4. Chat, clock, hardening.** Chat through the game's text entry, server clock
offset, reconnect handling, snapshot cadence tuning, a `docker compose` for the server.

### Status (this branch)

Step 0 is done and step 1 is in place end to end, built and tested here:

- **Fork imported.** `flyngmt/ACGC-PC-Port` is merged in with full history under the
  `upstream` remote, so its fixes come in with a plain merge. The upstream README is at the
  root behind a fork banner.
- **ENet vendored.** `pc/lib/enet` (v1.3.18, MIT) builds as a static library for both the
  game client and the server.
- **The game builds on 64-bit Linux hosts as a 32-bit binary.** The upstream port was
  Windows-only in practice; a handful of decompiled headers clashed with glibc
  (`bcmp`/`bcopy`, the pad `errno` field, `fsqrt`, `memcpy` `noexcept`, an MSL `ctype`
  guard). Each is fixed behind `#if defined(TARGET_PC) && !defined(_WIN32)`, so the Windows
  build is untouched. Build it with the 32-bit toolchain (`pc/cmake/Toolchain-linux32.cmake`
  or `-m32` with `libsdl2-dev:i386`).
- **The server is written, builds, and passes an end-to-end test** (`server/`, run with
  `ctest`). It stores each town, assigns resident slots by login name, serves the town on
  login, and on upload splices back every other resident's own private and house blocks and
  repairs the save checksum, so no client can overwrite another resident's character. It
  also relays chat and player-state packets, elects and migrates a world-authority client,
  and serves its clock. It never runs the game.
- **The client connects** (`pc/src/pc_net.c`). Reading a `[Network]` block from
  `settings.ini`, it logs in, downloads the shared town onto disk before the game's normal
  load path runs, and re-uploads after each in-game save. With no server configured every
  entry point is a no-op, so single-player is unchanged.
- **Base-port fix: characters no longer snap upside down for a frame.** The game converts
  float angles to `s16` everywhere and relies on out-of-range values wrapping modulo 2^16 as
  the GameCube's `fctiwz`+`sth` does. On i686 GCC defaults to x87 math and compiles that
  conversion to a 16-bit `fistp`, which turns any out-of-range value into `0x8000` = exactly
  180 degrees, so joints flipped whenever an animation blend crossed the +-180 boundary
  (walk start, talk start, the title demo). `pc/CMakeLists.txt` now builds everything with
  `-msse2 -mfpmath=sse` (plus `-mstackrealign` for Win32 callbacks), which converts through a
  32-bit `cvttss2si` and truncates like the console. Playtested: fixed. No game code changed.

What step 1 does **not** yet do: show a second character (that is step 2, the puppet actor),
apply incoming player-state or chat (counted but not yet rendered), or converge live town
changes (step 3). Today two people can load the same town as different residents from the
server and save it back safely; they will not see each other move until step 2.

### Not doing (and why)

- **Running the game headless on the server.** Would need a Linux 32-bit build, a null GX
  backend, a hidden dummy player, and engine changes to keep every acre loaded at once.
  It buys nothing the load-time catch-up does not already give us.
- **Visit-and-passport mode.** The port's `pc_m_card.c` already implements slot-B travel,
  so it remains a cheap later add-on for guests from other towns. Not on the path.

### Out of scope

Dolphin support, the N64 version, e-Reader and GBA link, and any distribution of game data.

## Sources

- ac-decomp: https://github.com/ACreTeam/ac-decomp
- ACGC-PC-Port: https://github.com/flyngmt/ACGC-PC-Port
- AC-Netplay: https://github.com/KadeStanford/AC-Netplay
- ModLoader64: https://github.com/hylian-modding/ModLoader64
- Dolphin Memory Engine (Python): https://pypi.org/project/dolphin-memory-engine/
