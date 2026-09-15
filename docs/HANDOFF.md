# Handoff — state of play

Written at the end of the first long build-and-playtest session and updated since, so a
fresh session can pick up without re-deriving anything. `CLAUDE.md` has the standing
rules; this file has the situation. `docs/MULTIPLAYER.md` has the design reasoning.

**Last updated 2026-09-15 (evening), the first session on the user's own PC.** Three
things landed today, all confirmed on the real game here (see "Playtesting without a
friend"): the puppet no longer crashes the game; it now stands, walks and animates
exactly as the other resident's game does; and it is dressed as *that* resident (body,
face, shirt) from their own save block. Along the way a design bug in the resident slots
was found in the user's real data and fixed (below). The build is in the user's game
folder; **both players need the new `AnimalCrossing.exe` and `AnimalCrossingOnline.exe`**
(protocol version 2; old copies are refused by the new server).

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

- **Seeing each other.** Crashed the game until 2026-09-14; fixed. On 2026-09-15 the
  puppet gained real animation (the sender's own keyframe state) and its own resident's
  look (model, face, shirt from their save block), both confirmed with a manufactured
  second resident. Not yet seen with two real people.
- **Live resident sync** — when a resident saves, their character and house are pushed to
  everyone else's *running* game. Shipped to the user, never confirmed, because the crash
  below got in the way.
- **The land relay** — field-item cells (trees, flowers, dropped items, holes) diffed each
  frame, relayed, and written into the stored town. Server side is covered by the e2e
  suite. It is in the user's current build but unconfirmed in play.

**Not started:** chat through the game's text entry, the shared town clock, puppet
collision and held items, and more than four residents.

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

**Confirmed 2026-09-15 on this PC** (see "Playtesting without a friend"): `[puppet] ct`,
`resident 0 appeared`, `[puppet] first draw (resident 0)`, 30+ s alive with
`residents-visible=1`, `resident 0 left; removing puppet`, and a clean re-creation when
she came back. No crash. Both players still need the new `AnimalCrossing.exe` — the crash
was in whichever game *draws* the other person.

**Animation and look (2026-09-15).** The state packet now carries the sender's
`animation0_idx`, `animation1_idx`, `part_table_idx`, both current frames and the speed;
the puppet's move proc starts the same animations (with a short morph on change) and runs
`cKF_SkeletonInfo_R_combine_play` every frame, snapping to the sender's frame only when
more than two frames adrift, so 30 Hz samples do not stutter a 60 fps puppet. Eyes and
mouth follow the animation's texture tables or blink normally. The look comes from the
resident's own `Private_c` (`Puppet_look_refresh` in `m_puppet.c_inc`, helpers
`mPlib_*_for` in `m_player_lib.c`): boy/girl skeleton by gender, the 14 eye/mouth
textures + face palette by face/sunburn/bee-sting, shirt texture + palette by
`cloth.idx` (built-in from the disc, or one of their own designs from the block). Each
puppet keeps its own ~3.6 KB copy, rebuilt when the block changes (live resident sync
clears it) and the puppet is rebuilt if the gender changes. Still missing: collision
(villagers walk through it), held items, hats/accessories drawn by the player's per-joint
callbacks.

**The resident-slot bug (found and fixed 2026-09-15).** The server assigned slots by
login order while the game keeps characters by save-block index, and they had drifted in
the user's real data (`residents.txt`: `0 Alana / 1 Owen`, but Owen's character in block
0). Consequences would have been Alana's puppet dressed as Owen and her saved character
wiped by the splice. Fix: `ACNET_MSG_CLAIM_SLOT` — once in town, the game reports
`Common_Get(player_no)` (gated on the scene: the title demo sets `player_no` to 0), the
server moves the resident to that slot, swapping with a slot-holder who has never saved
and refusing a block that holds someone else's saved character, and announces the move as
PEER_LEFT/PEER_JOINED. Confirmed on the real data: Owen logged in as resident 1, claimed
block 0, the server swapped him with Alana, and `residents.txt` became `0 Owen / 1 Alana`.
e2e check 17 covers it.

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

## Playtesting without a friend (on the user's PC)

The game *can* be run and driven here, so "cannot be verified" no longer applies to the
in-game side. The recipe that confirmed the puppet fix:

1. **Scratch copy of the game folder, `D:\Downloads\ACPC-test`** (exists; rebuild it from
   `D:\Downloads\ACPC` if missing): the exe, `shaders\`, `rom\`, `keybindings.ini`, a copy
   of `save\card_a\DobutsunomoriP_MURA.gci`, a copy of `serverdata\` (so the test server
   knows the same residents: `0 Alana`, `1 Owen`), and `settings.ini` edited to
   `fullscreen = 0`, `window_width/height = 1280/720`, `disable_resetti = 1` (skips the
   Resetti lecture after a crash) and `server_port = 7778`. The real folder is untouched.
2. **Server:** run the *allowed* exe with test data:
   `D:\Downloads\ACPC\acnet_server.exe --port 7778 --data D:\Downloads\ACPC-test\serverdata --verbose`
   (Windows Firewall blocks any other copy; see above).
3. **Game:** `Start-Process D:\Downloads\ACPC-test\AnimalCrossing.exe -WorkingDirectory D:\Downloads\ACPC-test`
   — the working directory is where it looks for `save/`, `rom/` and writes `aclog.txt`.
4. **Drive it with `pc/tools/gamekeys.ps1`.** `SetForegroundWindow` is refused from a
   background script, so use the `ptap`/`phold` commands, which post `WM_KEYDOWN/UP` to the
   game window and work without focus: `gamekeys.ps1 ptap Return` (Start),
   `ptap Space 80 2 1200` (A twice), `phold D 700` (walk right), `shot out.png`
   (screenshot of the window; `move 0 0 1000 620` first to get it clear of other windows).
   Title → Return; Egbert's dialogue → Space repeatedly (about eight); the name menu offers
   "Owen / I'm new" with the cursor on Owen; then "getting Nookton ready" → Space → the
   field, standing outside the house. Screenshot after every step: the dialogue is timed
   and a menu choice made blind is how you end up playing the wrong resident.
5. **Fake resident:** read Owen's position from a listener, then stand Alana next to him:
   `acnet_cli --server 127.0.0.1 --port 7778 --invite 3hwybg --name Alana --wait 4`
   prints `STATE ... pos=x,y,z`; then
   `acnet_cli ... --name Alana --state x+60,y,z+20 --state-every 200 --wait 150`
   keeps her there (the game forgets a resident after 3 s of silence; `--state-every`
   exists for this; `--anim N` picks the pose, frame 1 held). Watch `aclog.txt` for the
   `[puppet]` lines and take a `shot`. Killing the CLI is the "friend left" test; running
   it again is "friend came back".
6. **Give her a character** so the look path is exercised (a slot with no character falls
   back to the local player's textures): with the fake Alana *offline* (the server refuses
   an upload from a name that is connected),
   `python3 server/test/make_resident.py <serverdata>/towns/3hwybg/town.gci alana.gci 0 1 Alana girl 3 50`
   copies Owen's block 0 into block 1 as a girl with face 3 and shirt 50, then
   `acnet_cli ... --name Alana --upload alana.gci --reason save`. Owen's running game logs
   `resident 1 saved ... taking their character`, and the next fake Alana spawns with
   `[puppet] resident 1 looks like: girl, face 3, shirt 50` and `building girl body`.
   Run `make_resident.py` with MSYS2's python and give it the script path in `/c/...`
   form; its data arguments can be Windows paths.
7. Kill the game, the server and any `acnet_cli` afterwards (`Get-Process` by name; the
   server's command line shows the `--data` path so you never kill the user's own).
8. Editing source from this session: write the edit as a Python file with the Write tool
   and run it with `D:/msys64/usr/bin/python3.exe /c/...`. A bash heredoc mangled a
   `'\0'` into a real NUL byte once, and the files mix CRLF and LF.

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
