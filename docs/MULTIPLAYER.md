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
  client into the stored town. A resident's *slot* is defined as the index of the save
  block their character occupies: the server's first guess at login (by name, then the
  first block with no character) is corrected by the game itself as soon as the resident
  is in town (`ACNET_MSG_CLAIM_SLOT`), swapping with a slot-holder who has never saved and
  refusing to take a block that holds someone else's saved character.
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

- Eight players per town (Sept 2026; it was four, the engine's original resident limit,
  until the save layout was widened -- see "Eight residents" in HANDOFF.md). More means a
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

- **The launcher has a waiting room.** Before entering the town the player sees whether
  they actually reached the server, which of the four resident slots are claimed and who is
  connected right now, and whether the town has been founded yet. It runs on a pre-login
  `STATUS_REQUEST` the server answers without logging anyone in, so sitting in the lobby
  claims no resident slot and disturbs nobody playing; it also never creates a town, which
  keeps a status poll from being a way to allocate rooms. This closes a real hole: the game
  itself fails silently, so a wrong address, a wrong invite code or a full town all used to
  end with the game quietly starting in single-player.

- **Live resident sync (step 3, first slice).** When a resident's save lands on the server,
  the server sends that resident's own two blocks (`Private_c` and `mHm_hs_c`, ~19 KB,
  big-endian as stored) to everyone else in the town. The client parks them until the
  game's per-frame hook takes them, copies them over that resident's blocks in the running
  `Save_t` and byte-swaps them in place. Overwriting in place is what makes it safe: every
  pointer into the save stays valid, only the contents change, and it happens between the
  game's own reads. So a new resident choosing a house, or anyone changing their look or
  furniture, shows up for the others as soon as they save, with no relaunch. Land changes
  (trees, items, flowers) are still the event relay to come.

- **Puppet actor (step 2, first cut).** Another resident is drawn as a second
  `PLAYER_ACTOR` driven by their streamed position and facing. It is registered in
  `ACTOR_PART_UNUSED`, the one actor part nothing in the game claims: the player part
  would make `GET_PLAYER_ACTOR_NOW()` return it, and the NPC part crashed the game because
  villager code casts every entry of that list to `NPC_ACTOR*` (see `docs/HANDOFF.md`).
  It draws the skeleton directly with no per-joint callbacks. It plays exactly what the
  other resident's game is playing: the state stream carries both keyframe animation
  indices, the part table, frames and speed, and the puppet runs the same
  `cKF_SkeletonInfo_R_combine_play` the player does, resyncing to the sender's frame only
  when it drifts. It is dressed from that resident's own `Private_c` block (gender picks
  the boy/girl model; face, sunburn, bee-sting and shirt pick the textures), which every
  client has in the shared save and live resident sync keeps current, so it looks like
  them rather than like the local player. Still to do: collision (villagers walk through
  it), held items, and the head-turn/footprint callbacks the real player has.

- **Live world, first slice (step 3).** Puppets are solid: they carry the player's 20 x 60
  collision pipe in their own collision group as an immovable body, villagers are shoved
  by it like they are by the player, and the local player takes the collision system's
  push when the thing it bumped was a puppet (the original game never pushes the player,
  only villagers). A resident is only drawn to those in the same scene (`area` = scene
  id), so someone indoors does not appear at room coordinates in the field. The world
  authority alone rolls the weather and runs the daily land renewal; it sends its
  weather and renewal timestamp (`ACNET_MSG_WEATHER`, relayed only from the authority)
  and the others switch to it. The land relay ignores the game's runtime placeholders
  (`DUMMY_START`..0xFE00 and 0xFFFF) both ways and the server refuses to store them. A resident whose
  game died silently can log in again after 5 s of silence instead of waiting out ENet's
  timeout.

- **Villagers, first slice (step 3, acre ownership).** Each villager has exactly one
  owner: the resident talking to it, else the connected game with the lowest client id
  among those that have its acre loaded (the 3x3 of acres around each player), computed
  by every game from the same streamed player positions. The owner runs the villager's
  real AI and streams its position, facing and walking flag ten times a second
  (`ACNET_MSG_NPC_STATE`, stamped by the server); the other games skip that villager's
  own decision-making and give their copy walk-to-point / wait requests through its
  normal act system, so it animates and avoids obstacles, snapping only when hundreds of
  units off. Confirmed with two games: a copy that had wandered 400 units away was
  brought over and then tracked its owner within a tile. Still missing: villagers'
  *actions* beyond walking and standing (an owner's villager sitting, fishing, going
  indoors) are not mirrored yet, and a copy that never had the villager loaded does not
  spawn it.

- **The finish (step 4).** *Held items:* the state packet carries the item kind, the
  tool's own animation and frame; a puppet draws the axe or shovel as a display list at
  its right hand, the net and rod as their own small skeletons playing the sender's
  animation, and an umbrella as the game's umbrella tool actor born as the puppet's
  child (all tool models are shared statics in the executable, so nothing is DMA'd or
  fought over). *Which house:* `area` is the scene id plus, for the house scenes, the
  house owner the game recorded on the way in (`house_owner_name`: a house index for a
  resident's house, the villager's name for theirs), so two residents in different houses
  of the same size no longer see each other. *Villager acts:* the villager stream carries
  the owner's act and flags; a following copy hides when the owner's villager goes
  indoors and comes out where it did, runs when it runs, opens and closes its umbrella
  with it, and plays the singing and clapping acts (self-contained ones the copy can run
  alone). *Resident push (`ACNET_MSG_RESIDENT_PUSH`):* a game hashes its own character and
  house blocks once a second and sends them, without a save, when they change (at most
  every 5 s); the server stores only those blocks, marks the resident as saved so nobody
  else's upload can overwrite them, and relays them as `RESIDENT_DATA`, so a brand-new
  character, a new shirt or moved furniture reach the others at once. *Town clock:* the
  server sends its UTC time and its local-time offset at login and on every ping (one a
  minute); the client adds the difference between the host's wall clock and its own to
  the hardware time in `lbRTC_GetHardTime`, so every resident's game agrees on the hour
  and the day. *Chat:* T opens a line at the bottom of the screen, Enter sends, Esc
  cancels; each message is shown in the villagers' own speech window (`m_msg.c`: the
  name tag with the sender's name, the letter-by-letter text with its voice, the pop-in
  and pop-out), the window drawn only as tall as the text (one to four lines) and closing
  by itself after a time proportional to the length. It is opened directly, not through
  a conversation, so the reader is not frozen; a villager's conversation always takes the
  window and chat waits for it (`src/game/m_chat_bubble.c`, with three small hooks in
  `m_msg`: raw text for message number `mMsg_CHAT_MSG_NO`, a given name on the tag, the
  shorter body). The keyboard is the chat's while the line is
  open (and until the Enter that closed it is released, since Enter is also Start). All confirmed with two games on one PC; protocol
  version 4, e2e checks 22-24.

- **Audit (step 4b).** Protocol version 5: the puppet plays each animation layer in the
  sender's playback mode (one-shot or loop) instead of looping everything, and shows the
  item a player lifts overhead after a pick-up. All 157 player animations were run
  through a puppet, posed and playing, without a crash. A player whose game is paused
  (pockets, menus) keeps streaming from the play loop, so they no longer vanish from the
  others' towns. Pockets confirmed per resident with two real games: a dropped shirt left
  one player's pockets, not the other's, crossed to the other town and survived a restart.

What is **not** there yet: a villager copy that was never loaded on the follower is not
spawned; villager acts with a target (chasing an insect, reacting to a tool, greeting the
player) run as walk/stand on the copies; a puppet's held item plays its animation but not
its effects (the net's catch, the rod's float); hats and accessories drawn by the player's
per-joint callbacks; more than four residents.

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

## Seats and the roster (any number of residents, eight at a time)

The save has eight character blocks and eight houses, so at most eight residents can
be *in* the town. The server keeps everyone else on a roster:

- **Seats.** A login by a new name takes a free seat as before. When all eight seats are
  owned, the server frees the seat of a resident who is **away** (not connected): first
  choice someone who never saved (there is nothing to keep), otherwise the one seen
  longest ago (`residents.txt` now carries a last-seen time per seat, touched at login
  and logout). Nobody who is connected is ever moved. If all eight are in town, the
  ninth login is refused with "all eight residents are in town right now".
- **The roster.** The evicted resident's character block and house block go to
  `towns/<invite>/roster/<name>.bin` (Private_c then mHm_hs_c, as stored). When they log
  in again they get whatever seat is freed for them then, and those two blocks are
  copied back into it; their name, bells, pockets, house and furniture are as they
  left them. Villagers remember people by name and id, not by seat, so the town does not
  notice the move. (The same is true of a seat itself: rule 7 in CLAUDE.md, a slot is
  the save block the character lives in, still holds; the block just changes hands.)
- **Blanking a seat.** A freed seat is filled with the newcomer's own roster blocks if
  they have any, else with the town's *empty-block template*: a copy of an unused
  character block and an unused house, taken the first time the server sees a town
  with one (`towns/<invite>/empty.bin`). A town founded full of characters never gets a
  template, so its ninth login is simply refused, as before.
- **Houses are not seats.** A resident's house is the `homes[]` block their
  `house_arrangement` entry names, and a fifth resident's is whichever free house they
  picked in the second acre, not block 4. So every place the house half of a resident's
  data moves -- the push (`ACNET_MSG_RESIDENT_PUSH`), the relay
  (`ACNET_MSG_RESIDENT_DATA`), the server's splice on upload, the roster -- carries or
  looks up that house index. The server also protects each saved resident's
  arrangement byte on upload, so a game that has not yet heard where a newcomer moved
  in cannot undo it with its own save. Protocol 8.
- **What a running game sees.** When a seat changes hands the server relays the seat's
  new blocks to everyone in town, so the old resident's puppet and house become the
  blank (or returning) ones without a reload.
