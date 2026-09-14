# Town server

The always-on home for one or more shared towns. It stores each town's save,
hands it to residents when they log in, accepts their uploads while protecting
every resident's own character data, relays player state and chat between the
residents of a town, elects a world-authority client, and serves the town
clock. It never runs the game. Design and rationale: `../docs/MULTIPLAYER.md`.

## Run it

Docker, from the repository root:

```sh
docker compose -f server/docker-compose.yml up -d
docker compose -f server/docker-compose.yml logs    # shows the generated invite code
```

Or build natively (Linux, needs cmake and a C compiler):

```sh
cmake -S server -B server/build -G Ninja
cmake --build server/build
server/build/acnet_server --port 7777 --data ./data
```

Open UDP 7777 (or whatever `--port` you chose) on the firewall. Players put the
server address and the invite code in the game's `settings.ini`.

## Data directory

```
data/
  invites.txt            one invite code per line; created with a random code on first run
  towns/<invite>/
    town.gci             the canonical town, a Dolphin-compatible save file
    town.gci.bak1..3     rotated backups of previous versions
    town.version         upload counter
    residents.txt        "<slot> <uploaded> <name>" per resident
```

Because `town.gci` is a normal GCI file, it can be copied into a single-player
`save/card_a/` folder or opened in Dolphin at any time.

## Command-line client

`acnet_cli` speaks the same protocol and is what the end-to-end test drives.
It is also useful for operators:

```sh
acnet_cli --server example.org --invite CODE --name admin --download town.gci
acnet_cli --server example.org --invite CODE --name admin --upload town.gci --reason save
acnet_cli --server example.org --invite CODE --name admin --wait 30     # watch join/leave/chat
```

## Tests

`ctest` in the build directory runs `test/e2e.sh`, which starts a server on a
random port and checks login and slot assignment, founding a town, the
per-resident splice and checksum repair, the four-resident limit, malformed
uploads, chat relay, world-authority migration, and persistence across a
restart. `test/mktown.py` builds and inspects synthetic town files using the
layout constants from `net/protocol.h`.
