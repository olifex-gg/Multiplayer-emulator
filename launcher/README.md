# Animal Crossing Online launcher

A small Windows app so players can host or join without a command prompt or
editing any config files. It writes the game's `settings.ini` `[Network]`
block, starts the local town server when hosting, and launches the game.

## For players (what to hand your friends)

Put these together in one folder:

```
AnimalCrossingOnline.exe   <- this launcher (double-click this)
AnimalCrossing.exe         <- the game
acnet_server.exe           <- only needed on the machine that HOSTS
shaders/                   <- ships with the game
rom/                       <- put your own Animal Crossing disc image here
```

Double-click **AnimalCrossingOnline.exe**, then:

- **Host a town:** pick "Host", type your name, keep (or change) the invite
  code, click Play. The launcher shows the address and code to give friends.
- **Join a town:** pick "Join", type your name, paste the address and code the
  host gave you, click Play.

The launcher remembers your last name/address/code. Each player still needs
their own copy of the disc image in `rom/`; no game data is shared over the
network, only your character and the town.

## Hosting notes

Hosting on your own PC works out of the box on a home/LAN network. For friends
on the wider internet you either forward UDP port 7777 on your router, or run
`acnet_server` on an always-on machine (a cheap VPS) and everyone uses Join.
The launcher's Host button runs the same server locally either way.

## Build

`launcher/build-windows.sh` (MSYS2 native, or MinGW cross with
`CC=i686-w64-mingw32-gcc`). Depends only on standard Windows libraries.
