# Multiplayer Animal Crossing — project instructions

Fork of the ACGC-PC-Port native PC port of the Animal Crossing (GameCube, USA `GAFE01`)
decompilation, adding real-time online multiplayer: up to eight people play as the eight
residents of **one shared town**, at the same time, over the internet. (The original game
had four; `PLAYER_NUM` is 8 here and the save layout changed with it -- see rule 16.)

Read `docs/MULTIPLAYER.md` first — it is the design and decision record. Read
`docs/HANDOFF.md` for the current state of play, what is broken right now, and the
history behind decisions that look odd.

Work happens on branch `claude/multiplayer-animal-crossing-emulator-3kmegf`.

## Who this is for

The person you are working with is not a programmer and does not use a command prompt.
They play this with friends on Windows. That shapes everything:

- **Deliver working Windows binaries**, not instructions to build. Send the `.exe`.
- **Explain in plain language.** Name the mechanism, don't just say "fixed".
- **They are the only playtester.** Nothing here can be verified by running the real game
  in CI, so diagnostics that survive on their machine matter more than local certainty.
  Every build writes `aclog.txt` (game) and `launcher.log` (launcher) next to the exe,
  flushed per line, with a Windows crash handler recording the faulting module + offset.
  Ask for those files rather than guessing.
- **Never ask them for the game disc image**, and never add game assets to this repo. The
  decompiled source is the game's code; the ROM only holds assets. No work here needs it.

## Layout

| Path | What |
|---|---|
| `src/`, `include/` | the decompiled game (CC0). Engine + gameplay. |
| `pc/` | the PC port layer (MIT): SDL2, OpenGL 3.3, save/GCI handling. |
| `pc/src/pc_net.c` | multiplayer client: login, town download/upload, player state, land relay. |
| `src/game/m_puppet.c_inc` | in-game side: other residents, live resident sync, land diffing. Included at the end of `src/game/m_player.c`. |
| `net/protocol.h` | the wire protocol AND the save-layout offsets the server relies on. Single source of truth shared by client and server. |
| `server/` | the town server (C + ENet). Never runs the game. `ctest` runs an end-to-end suite. |
| `launcher/launcher.c` | Win32 Host/Join launcher + waiting room. All visuals drawn with GDI. |

## Building

**The port is 32-bit only** — JSystem casts pointers to `u32`. A 64-bit build is rejected
by CMake on purpose.

Native on Windows, from an **MSYS2 MINGW32** shell (not MINGW64):

```bash
pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja mingw-w64-i686-make mingw-w64-i686-SDL2 python diffutils
cmake -S pc -B pc/build32 -G Ninja && cmake --build pc/build32 -j
cmake -S server -B server/build -G Ninja && cmake --build server/build -j
bash launcher/build-windows.sh            # -> AnimalCrossingOnline.exe
```

(`python` and `diffutils` are only for the server test suite.) On Windows the game links
SDL2 statically (`AC_SDL2_STATIC`, default on), so `AnimalCrossing.exe` is one file.

**On the user's own PC** (sessions run there since Sept 2026), MSYS2 lives at `D:\msys64`
and is not on PATH. From PowerShell, run any of the above as
`$env:MSYSTEM="MINGW32"; & D:\msys64\usr\bin\bash.exe -lc "cd /c/Users/owenq/Documents/GitHub/Multiplayer-emulator && <command>"`.
The game folder is `D:\Downloads\ACPC`; copy new exes there (keeping a copy of the old
ones in a dated `old-build-...` subfolder first) and read `aclog.txt` there directly.
Anything installed for this project goes on `D:` and is removed when the user says the
project is done.

The game needs `shaders/` and `rom/<disc image>` beside the exe at runtime.

Linux (for compile-checking only — a 32-bit toolchain and `libsdl2-dev:i386`):

```bash
cmake -S pc -B pc/build32 -DCMAKE_TOOLCHAIN_FILE=$PWD/pc/cmake/Toolchain-linux32.cmake -G Ninja
```

Cross-compiling Windows binaries from Linux uses
`pc/cmake/Toolchain-mingw32-cross.cmake` and needs an i686 SDL2 development package;
pass `-DSDL2_FOUND=TRUE -DSDL2_INCLUDE_DIRS=... -DSDL2_LIBRARIES=...` since CMake's
finder cannot see it when cross-compiling.

Always compile-verify **both** Windows and Linux before shipping: the Linux build is fast
and catches most mistakes, but only the Windows build is what the user runs.

## Testing

- `ctest --test-dir server/build` — the server end-to-end suite (`server/test/e2e.sh`).
  It covers login, slot assignment, founding a town, the per-resident splice, checksum
  repair, the eight-resident limit and the roster, chat, authority migration, restart
  persistence, the lobby status query, live resident sync and the land relay. **Add a case for every new
  protocol message.** `server/tools/acnet_cli.c` is the scriptable client it drives.
  On the user's PC, Windows Firewall silently *blocks* a server exe it has not seen before
  (it auto-created Block rules for `server/build/acnet_server.exe`), so `ctest` times out
  there. Run the suite against the copy the user already allowed instead:
  `bash server/test/e2e.sh /d/Downloads/ACPC/acnet_server.exe server/build/acnet_cli.exe`.
  Do not change firewall rules yourself; that is the user's security setting.
- The launcher can be smoke-tested headlessly under Wine with a virtual display — see
  `docs/HANDOFF.md`. Do this before sending a launcher build; it has already caught a
  crash that would otherwise have reached the user.
- **On the user's PC the game can be run and driven.** `docs/HANDOFF.md` ("Playtesting
  without a friend") has the recipe: a scratch copy of the game folder, a test server on
  another port, `pc/tools/gamekeys.ps1` to post keystrokes and take screenshots, and
  `acnet_cli --state ... --state-every` to stand in as a second resident. Use it before
  telling the user something works. In a cloud session (no GPU, no disc image) reason
  from the source and lean on `aclog.txt` from the user.

## Hard-won rules

These each cost a debugging cycle. Do not regress them.

1. **Build with SSE math** (`-msse2 -mfpmath=sse` in `pc/CMakeLists.txt`). The game relies
   on float→`s16` conversions wrapping modulo 2^16 like the GameCube's `fctiwz`. i686 GCC
   defaults to x87, whose 16-bit `fistp` turns out-of-range values into `0x8000` = exactly
   180°, which made characters flip upside down for a frame. Do not "simplify" these flags.
2. **`pc_net_service()` must actually call `enet_host_service`.** A zero timeout is a valid
   non-blocking poll and is the only moment ENet transmits queued packets. An early return
   there silently breaks both directions while login and saving still work.
3. **Actor parts are types.** Gameplay code identifies actors by `actor->part` and blindly
   casts every actor in a part's list to that part's struct (e.g.
   `aNPC_greeting_area_check` casts every `ACTOR_PART_NPC` entry to `NPC_ACTOR*`). Never
   put a foreign-shaped actor in a part that another subsystem scans.
4. **Never overwrite a player's town without a copy.** Downloading the shared town replaces
   `save/card_a/DobutsunomoriP_MURA.gci`; `pc_net.c` keeps a one-time `.before-online`
   copy first. Keep that guarantee.
5. **A resident's own blocks are theirs.** The server splices every other resident's
   `Private_c` and `mHm_hs_c` back in on upload, so no client can overwrite another's
   character or house. Any new sync path must preserve that.
6. **Offsets live in `net/protocol.h`** and are verified against the real structs with
   `offsetof`. If you change anything about the save layout, re-derive them; do not trust
   the comments.
7. **A resident's slot IS the save block their character lives in.** The server hands out
   slots by login name, the game stores characters by `Save_t.private_data` index, and the
   two drifted apart in the user's real town (Owen listed as resident 1 with his character
   in block 0). Every protection, relay and puppet lookup keys on that index, so the game
   tells the server its `player_no` as soon as it is in town (`ACNET_MSG_CLAIM_SLOT`) and
   the server moves the resident. Never add a path that assumes login order.
8. **Edit files with a Python script written by the Write tool, not a bash heredoc.** On
   this machine a heredoc turned `'\0'` into a literal NUL byte inside a C source file
   (the compiler accepted it, silently). The repo mixes CRLF and LF files, so the script
   must preserve each file's line endings.
9. **The live field table is not the land.** While a town is loaded the game swaps
   structure cells (houses, the dump, the shrine, mailboxes) for runtime placeholders
   (`DUMMY_START` 0xF000 up to the reserved ids at 0xFE00, plus 0xFFFF) and restores the
   real ids when it saves. The land relay must never send or apply them and the server must
   never store them (`ACNET_LAND_ITEM_IS_RUNTIME`).
   Relaying them once wiped the dump marker out of a stored town, and the next game to load
   it crashed inside `mAGrw_SetItemDump` on an uninitialised position.
10. **Only the world authority runs the town's clockwork.** The oldest connected game rolls
    the weather and runs the daily land renewal; it sends its weather and its
    `all_grow_renew_time` to the others. Any second game doing either on its own gives two
    residents two different towns (and grows everything twice).
11. **A villager has one owner, and the followers drive it through its own acts.** Owner =
    the resident talking to it, else the lowest client id among games with its acre loaded;
    every game computes that from the same streamed positions. A following copy skips its
    schedule/think step and is given `aNPC_ACT_WALK` `TO_POINT` / `aNPC_ACT_WAIT` requests
    (priority 3) toward the streamed spot, so it animates and dodges obstacles like any
    villager. Never write a villager's position every frame except to correct a copy that
    is hundreds of units off -- the walk act, the BG check and the talk system all assume
    they own the position.
12. **The keyboard is polled, not just evented.** `PADRead` reads `SDL_GetKeyboardState`
    every frame, so a key an overlay consumed as an event (the Enter that sends a chat
    line) is still "down" when the game next polls, and Enter is Start: the inventory
    opened after every chat message until `pc_chat_blocks_pad()` kept the pad blocked
    until that key's key-up. Any new overlay that takes keystrokes needs the same.
13. **A child actor must not outlive its parent.** The puppet's umbrella is a tool actor
    born as the puppet's child; its draw dereferences `parent_actor`, and deleting the
    parent only unlinks the child. `Puppet_actor_dt` deletes the umbrella first. The same
    applies to any other child a puppet ever grows.
14. **The town has one clock and one look per resident, both pushed, not saved.** The
    host's wall clock (via the server's `server_tz_min`) is added to the hardware time
    in `lbRTC_GetHardTime`, and a resident's own character and house blocks are pushed
    (`ACNET_MSG_RESIDENT_PUSH`) the moment they change. The server marks a pushed
    resident as saved so their blocks are protected from everyone else's uploads; do not
    weaken that, and never push someone else's blocks.
15. **The player's move proc stops while the game is paused** (pockets open, any menu, a
    letter), and that is where the per-frame sync runs. Everything we *send* therefore
    lives in `Puppet_stream_local`, which `Puppet_net_idle` (end of `play_main`) calls on
    any frame the move proc skipped; otherwise a friend reading a letter vanished from
    everyone's town after three seconds. Incoming changes (land, blocks) simply queue
    until the pause ends, which is fine. Anything new that must keep flowing while paused
    goes in the stream function, not the update function.
16. **Eight residents, and the save layout that comes with them.** `PLAYER_NUM` is 8, so
    `Save_t` is 0x37620 bytes, the town file 0xA2040, and every per-player field grew:
    `house_arrangement` is a byte per player, the saved-event flags are 64 bits, the
    lighthouse's players are a byte each, K.K.'s flags are 32 bits, museum donors run 1-8
    with 9 for "left town", signboard ids carry three bits of player. `net/protocol.h`
    holds the new offsets and `m_puppet.c_inc` checks every one against the structs at
    compile time, so a layout change fails the build instead of the server. Old town files
    (0x72040 bytes) are converted on load by `pc/src/pc_save_convert.c` from the layout
    recorded in `pc_save_legacy_layout.h` (generated from the four-player headers with the
    real compiler; never hand-edit it), the four-resident file is kept beside the new one
    as `.before-eight`, and the server serves such a town as it is until the first save
    replaces it. Anything that changes `Save_t` again needs the same treatment: record the
    layout before and after, convert, assert.
17. **A resident's house is their arrangement entry, not their slot.** `homes[i]` belongs
    to whoever's `house_arrangement` entry says `i`; a fifth resident picks a free house in
    the second acre, so theirs is not `homes[4]`. Every path that moves the house half of a
    resident's data (push, relay, the server's splice, the roster) carries or looks up that
    index (`ACNET_HOUSE_ARRANGEMENT_OFFSET`). Pushing `homes[slot]` lost Eve's house.
