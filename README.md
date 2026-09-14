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

## Recommended architecture

Fork or contribute to ACGC-PC-Port. Everything below lives in the game's own C code.

**Session model.** One player hosts their town. Others join and arrive via the train, exactly
as a slot-B visitor would. Each visitor plays as their own character, using their own save.
When they leave, their save keeps their inventory changes and the host's town keeps what
they did to it, reusing the existing visit write-back logic.

**Networked memory card B.** On join, the host sends its town data (the same bytes that would
live on card B). The visitor's client loads it through the existing visit code path. From
then on every client runs a full local simulation of the same town.

**Puppet actor.** A new actor type that reuses the player's model, animation, and draw code
but is driven by network state instead of input. Synced per player at 20 to 30 Hz:
position, facing, animation index and frame, held item or tool, clothing and face, emote,
and speech bubble text. Puppets are only drawn when in the same room or field cell.

**Host-authoritative world events.** Anything that mutates the town is sent as an event and
validated by the host: item pickup and drop, tree shake, rock hit, dug or filled hole, bells,
furniture placed, gate state. Villager NPC positions and dialogue state stream from host to
visitors so everyone sees the same town. The host's clock is the world clock.

**Transport.** UDP with a small reliable layer (ENet or similar) for direct connect, plus an
optional relay for players behind NAT. Chat can reuse the game's existing letter-writing
text entry.

## Phases

1. **Foundation.** Build the PC port from source. Get the slot-B visit flow fully working in
   the port (the port's README lists town visiting as missing, and a "black screen after
   returning from a slot B visit" bug was fixed upstream in May 2026, so it is partly there).
   This is the base everything else stands on and is a contribution upstream wants.
2. **Transport and lobby.** Host, join, send town data, visitor arrives by train.
3. **Puppets.** Second visible character, synced movement and animation.
4. **World event sync.** Items, trees, holes, bells, gate. Host validation.
5. **Chat, villager sync, time sync.**
6. **Three and four player sessions, relay server, polish.**

## Constraints

- Only the USA v1.0 disc (`GAFE01`, revision 0) is supported by the decomp and the port.
- No game assets are ever committed. Players supply their own disc image.
- Licensing: the decompiled code is CC0, the PC port layer is MIT. Our additions should be MIT.

## Sources

- ac-decomp: https://github.com/ACreTeam/ac-decomp
- ACGC-PC-Port: https://github.com/flyngmt/ACGC-PC-Port
- AC-Netplay: https://github.com/KadeStanford/AC-Netplay
- ModLoader64: https://github.com/hylian-modding/ModLoader64
- Dolphin Memory Engine (Python): https://pypi.org/project/dolphin-memory-engine/
