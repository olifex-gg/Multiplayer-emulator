# Handoff — state of play

Written at the end of the first long build-and-playtest session, so a fresh session can
pick up without re-deriving anything. `CLAUDE.md` has the standing rules; this file has
the situation. `docs/MULTIPLAYER.md` has the design reasoning.

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

- **Seeing each other.** This is the open problem. See below.
- **Live resident sync** — when a resident saves, their character and house are pushed to
  everyone else's *running* game. Shipped to the user, never confirmed, because the crash
  below got in the way.
- **The land relay** — field-item cells (trees, flowers, dropped items, holes) diffed each
  frame, relayed, and written into the stored town. Server side is covered by the e2e
  suite. It is in the user's current build but unconfirmed in play.

**Not started:** chat through the game's text entry, the shared town clock, giving each
puppet the right face/outfit, and more than four residents.

## The open bug: puppets crash the game

Symptom: with two people in the town, both games crash the moment either joins.

Root cause is known and proven from the user's `aclog.txt` plus disassembly of the exact
binary they ran:

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
toggle it remembers. Off, everything else still works. The user has been told to switch it
off if it crashes.

**Next step:** a background workflow (`puppet-actor-part-audit`) was auditing every actor
part for places that cast or reinterpret actors by part, plus the option of drawing
puppets without registering an actor at all, with adversarial verification of the
recommendation. If its results are not in this session's context, re-run it — the script
is under `.claude/projects/.../workflows/scripts/`. Do not simply move the puppet to
another part on a hunch; the whole point is that one missed cast is another crash for the
user.

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
- They intend to move to a local Claude Code install so builds can be run and tested in one
  loop instead of exes being sent back and forth.
