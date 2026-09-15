# Handoff — state of play

Written at the end of the first long build-and-playtest session and updated since, so a
fresh session can pick up without re-deriving anything. `CLAUDE.md` has the standing
rules; this file has the situation. `docs/MULTIPLAYER.md` has the design reasoning.

**Last updated 2026-09-14 (late evening), the first session on the user's own PC.** The
puppet crash below is fixed in source and built; the build is in the user's game folder
but has not been played yet. See "The puppet crash" for what to look for.

## Where things actually stand

Two people on two PCs, in different houses, have played in the same town. That much works.

**Working, confirmed by playtest**

- Host/Join through the launcher, invite codes, the waiting room showing who owns which
  resident slot and who is online.
- The shared town: the host's save goes to the server when their game connects, a joiner
  downloads it and is created as a *new resident of that town* (Rover asks their name and
  which town they are headed to — the game's own new-resident flow).
- Saving: the server splices each resident's own character and house back in, so nobody
  overwrites anybody. Verified end-to-end in `server/test/e2e.sh`.
- Both PCs reach a server across the internet with UDP 7777 forwarded on the host's router.

**Built but never seen working**

- **Seeing each other.** Crashed the game until 2026-09-14; the cause is fixed (see
  below) and the fix is built and in the user's folder, but nobody has played it yet.
- **Live resident sync** — when a resident saves, their character and house are pushed to
  everyone else's *running* game. Shipped to the user, never confirmed, because the crash
  below got in the way.
- **The land relay** — field-item cells (trees, flowers, dropped items, holes) diffed each
  frame, relayed, and written into the stored town. Server side is covered by the e2e
  suite. It is in the user's current build but unconfirmed in play.

**Not started:** chat through the game's text entry, the shared town clock, giving each
puppet the right face/outfit, and more than four residents.

## The puppet crash: fixed 2026-09-14, awaiting a playtest

Symptom was: with two people in the town, both games crash the moment either joins.

Root cause was proven from the user's `aclog.txt` plus disassembly of the exact binary
they ran:

```
[puppet] ct: building body for resident 0
[puppet] ct: body ready
[puppet] resident 0 appeared at 2180,200,824: puppet created
*** CRASH: exception 0xC0000005 at 00c1c79c
*** in module ...\AnimalCrossing.exe (base 00af0000, offset 0x12C79C)
*** reading address 3f8008e2
```

`[puppet] first draw` never printed, so it died before drawing. Mapping offset `0x12C79C`
(ImageBase `0x400000` → VA `0x52C79C`) with `i686-w64-mingw32-objdump -d` lands inside
`_aNPC_actor_move_c`:

```
mov    0x17c(%ebx),%eax      ; a pointer field in NPC_ACTOR
test   %eax,%eax
je     ...                   ; non-NULL, so continue
movzbl 0x8e2(%eax),%eax      ; FAULT: eax = 0x3f800000, the float 1.0
```

The chain: `aNPC_actor_move_c` (every villager, every frame) calls
`aNPC_greeting_area_check`, which walks **every** actor in `ACTOR_PART_NPC` and casts each
one to `NPC_ACTOR*`. The puppet is a `PLAYER_ACTOR` placed in that part, so offset `0x17C`
— a pointer in `NPC_ACTOR` — is `keyframe0.frame_control.max_frames` in a `PLAYER_ACTOR`,
which `cKF_FrameControl_ct` sets to `1.0f` = `0x3f800000`. Dereferenced, that is the
faulting address. (`offsetof` confirms `PLAYER_ACTOR.keyframe0 == 0x174`.)

The puppet was put in `ACTOR_PART_NPC` deliberately, so that `GET_PLAYER_ACTOR_NOW()`
(which returns `list[ACTOR_PART_PLAYER].actor[0]`) would not find it. That solved one
problem and created a worse one.

**An earlier fix attempt is in the shipped build and was aimed at the wrong thing.** It
replaced `Player_actor_draw_Normal` with a minimal skeleton draw with no callbacks. That
change is still worth keeping — the player draw really does run per-joint callbacks for
footprints, head turning and the held-item clip that a puppet has no state for — but it is
not what was crashing.

**Mitigation already shipped:** `settings.ini` `show_other_players` (default 1) gates
spawning and updating puppets, surfaced in the launcher as a "Showing other players"
toggle it remembers. Off, everything else still works.

**The fix (2026-09-14):** the puppet now lives in `ACTOR_PART_UNUSED` — the one actor
part no profile in the game uses. An audit of every reader of that list found only code
that touches the common `ACTOR` header and never casts: the generic per-part loops in
`m_actor.c` (`Actor_info_call_actor`, `Actor_info_draw_actor`, `Actor_info_dt`,
`Actor_info_save_actor`, `restore_fgdata_all`, whose `restore_flag[]` is FALSE for that
part), `Camera2_ClearActorTalking_Cull` in `m_camera2.c`, and `aINS_get_stress` in
`ac_insect_move.c_inc` (which only reads positions — so insects now get spooked by
puppets, like by players). `ac_birth_control.c` and `ac_ev_broker_move.c_inc` walk
parts by name and never name this one. Two consequences handled: outside the NPC part a
`mv_proc` only runs while in view or with `ACTOR_STATE_NO_MOVE_WHILE_CULLED`, so the
profile sets that flag; and `Actor_ct`/`mNpc_SetNpcinfo` no longer run their NPC-only
texture-bank and animal-info code on the puppet, which they had been doing with
`npc_id == EMPTY_NO`. The reasoning is also in the header comment of `m_puppet.c_inc`.

**What to look for in the next `aclog.txt`:** after `[puppet] resident N appeared`, the
line `[puppet] first draw (resident N)` must appear and the game must keep running. If it
crashes again, the crash handler's module + offset is the next lead; map it with
`objdump -d` on the exact exe as before. Both players need the new `AnimalCrossing.exe`
— the crash is in whichever game *draws* the other person.

**Still true after the fix:** the puppet stands in the bind pose (no walking animation),
wears the local player's outfit and face (one texture bank), and has no collision, so
villagers walk through it. Those are the next iterations of step 2.

## Things that already bit us

Beyond the rules in `CLAUDE.md`:

- **The launcher crashed one second after opening** because registering a second window
  class reused the `WNDCLASSA` struct and the main window was then created from
  `wc.lpszClassName`, which by that line named the lobby class. The main window ran the
  lobby's `WM_CREATE`, armed its 1 s timer, and the first tick entered a critical section
  nothing had initialised. Create windows from an explicit class name.
- **A joiner got the brand-new-town intro** (name a town, pick a map) because the host's
  town only reached the server on the host's first in-game *save*. Fixed by uploading at
  connect time when the server reports no town and this PC has one.
- **"AnimalCrossing.exe was not found next to this launcher"** is almost always someone
  double-clicking the launcher from *inside* a `.zip`: Windows copies that one file to
  `%TEMP%` and runs it alone. The launcher now detects running from a temp path and says so.
- **The port's settings menu rewrites `settings.ini` and drops the `[Network]` block.** The
  launcher re-adds it on every Play, so it only matters if the game is launched directly.
- **The server's `mkdir_p` failed on `D:/...` paths** because it tried to `_mkdir("D:")`,
  which fails without `EEXIST`. Fixed (drive prefix skipped, `\` accepted as a separator).
- **ENet drops a peer's undelivered packets when it handles that peer's DISCONNECT.** A
  client that sends something and disconnects in the next instant can lose it if the
  server reads both datagrams in one service pass — this lost the land cell in the e2e
  suite on the user's fast PC. `acnet_cli` now uses `enet_peer_disconnect_later`, which
  waits for acknowledgements. The game client uploads the whole town on leave, so it is
  not exposed, but any future "send then quit" path must remember this.
- **A UDP client that talks to a port before the server binds it fails outright on
  Windows** (ICMP unreachable surfaces as an error on the next socket call). The e2e
  harness now waits for the server's "listening" log line instead of sleeping 0.5 s.

## Working on the user's PC (since 2026-09-14)

Sessions now run in the Claude desktop app on the user's Windows 11 machine. Practical
notes, all learned the hard way in the first evening:

- **Toolchain:** MSYS2 at `D:\msys64` (installed by Claude with permission; installer
  kept at `D:\Downloads\msys2-installer\`). Not on PATH. Run builds from PowerShell as
  `$env:MSYSTEM="MINGW32"; & D:\msys64\usr\bin\bash.exe -lc "cd /c/Users/owenq/Documents/GitHub/Multiplayer-emulator && <command>"`.
  Packages: `mingw-w64-i686-{gcc,cmake,ninja,make,SDL2}`, plus `python` and `diffutils`
  for the test suite. There is no WSL and no Docker, so no Linux compile check here.
- **The user's rule:** anything downloaded or installed goes on `D:`, and all of it gets
  uninstalled when they say the project is done (`D:\msys64\uninstall.exe pr
  --confirm-command`). Ask before installing anything new.
- **Deploying:** copy `pc/build32/bin/AnimalCrossing.exe`, `AnimalCrossingOnline.exe` and
  `server/build/acnet_server.exe` into `D:\Downloads\ACPC`. Keep the previous exes in a
  dated subfolder first (`old-build-2026-09-14` holds the build from before the puppet
  fix). The friend needs the same `AnimalCrossing.exe`.
- **Windows Firewall** auto-created *Block* rules for `server/build/acnet_server.exe` the
  first time it listened (network profile is Public), so `ctest` times out on this PC.
  The copy in the game folder has the user's *Allow* rules, so run the suite as
  `bash server/test/e2e.sh /d/Downloads/ACPC/acnet_server.exe server/build/acnet_cli.exe`.
  Do not edit firewall rules; that is the user's call.
- **Kill your test servers.** A server started in the background from MSYS bash is not
  always reached by `kill`; check `Get-Process acnet_server` (their command lines show
  the `--data` path) before assuming the port is free, and never kill one whose `--data`
  is `serverdata` — that is the user's, started by the launcher.
- **Build outputs** (`pc/build32`, `server/build`, `*.exe`) are git-ignored.

## Testing the launcher headlessly (Linux)

This caught a real crash before it reached the user. Needs `wine32:i386`, `xvfb`,
`x11-apps`, `imagemagick`, `xdotool`:

```bash
export WINEPREFIX=/tmp/wineprefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
xvfb-run -a -s "-screen 0 1024x768x24" bash -c '
  /usr/lib/wine/wine ./AnimalCrossingOnline.exe &
  sleep 7; import -window root shot.png
  xdotool mousemove X Y click 1          # client coords + window origin (3,22)
  sleep 1; import -window root shot2.png
  kill %1'
```

Run a real `acnet_server` alongside it and the waiting room can be driven end to end.

## The user's setup

- Windows, game folder `D:\Downloads\ACPC`, USA disc image in `rom/`.
- Verizon Fios router, UDP 7777 forwarded to their PC; genuine public IPv4.
- They host; a friend joins. The friend was given `acnet_server.exe` too — harmless as long
  as she only ever clicks Join.
- ENet is IPv4-only, so the "IPv6 Pinholes" page on their router is irrelevant.
- Since 2026-09-14 Claude runs on their PC directly, so builds land in the game folder
  and `aclog.txt` can be read in place — see "Working on the user's PC" above.
