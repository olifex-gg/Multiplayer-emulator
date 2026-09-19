# Hosting the town on a rented server

Today the town only exists while someone runs `acnet_server.exe` on their own PC. A
rented Linux machine (a "VPS", about 5 US dollars a month) keeps it up all the time, so
anyone can play whenever they like and nobody has to be "the host".

The server never runs the game and never needs the disc image. It is a small program
that holds the town file and relays what each game does.

## What to rent

| Thing | What to pick | Why |
|---|---|---|
| Provider | Hetzner, DigitalOcean, Vultr or Linode | Any is fine; Hetzner is the cheapest |
| Plan | The smallest: 1 CPU, 1 GB memory, 10+ GB disk | The server uses almost nothing |
| Image | Ubuntu 24.04 LTS | What the setup script expects |
| Region | Nearest to whoever plays most | Lower delay in town |
| Login | SSH key if offered, otherwise a long password | Keeps strangers out |

The town file is about 650 KB, so disk space is never the limit.

## Setting it up

1. Create the machine. Write down its public IP address.
2. Open the provider's console (a "Launch console" or "Web terminal" button) and log in
   as `root`, or connect with PuTTY or Windows Terminal (`ssh root@THE-IP`).
3. Download and run the setup script, with your own invite code and time zone:

   ```
   curl -fsSL https://raw.githubusercontent.com/olifex-gg/Multiplayer-emulator/claude/multiplayer-animal-crossing-emulator-3kmegf/server/deploy/vps-setup.sh -o vps-setup.sh
   sudo bash vps-setup.sh --code 3hwybg --tz America/New_York
   ```

   If the repository is private, the download step will not work. Copy the project
   folder up with WinSCP to `/opt/acnet-src` instead, then run the script from there.

   The script installs a compiler, builds the server, installs it as a service that
   starts with the machine, opens UDP port 7777, sets the machine's clock to your time
   zone, and sets up a daily town backup kept for 30 days.

4. Open UDP port 7777 on the provider's own firewall page if it has one.
5. Copy your existing town up to the machine, with WinSCP or `scp`, to
   `/opt/acnet/data/towns/<code>/town.gci`, then run `systemctl restart acnet`.
   Without this the server starts an empty town and the first person to play founds a
   new one.

**The town's clock follows the server's clock**, not each player's
(`server_tz_min` in the welcome message, rule 14 in CLAUDE.md). A machine left on UTC
runs the town's day, its shops and its events on UTC. Set `--tz` to the time zone the
players live in.

## Playing against it

In the launcher everyone picks **Join**, types the machine's IP address in the address
field and the invite code, then Play. Nobody picks Host any more, and no one needs to
leave a PC running.

## Looking after it

| Question | Command on the server |
|---|---|
| Is it running? | `systemctl status acnet` |
| Who is joining and saving? | `journalctl -u acnet -f` |
| Where are the backups? | `ls /opt/acnet/backups` |
| Restore a backup | `systemctl stop acnet`, copy the dated file over `town.gci`, `systemctl start acnet` |
| Update to a new build | rerun `vps-setup.sh` with the same options |

Every player's game must match the server's protocol version, so when the game exe
changes, update the server at the same time.

## What it costs and what it risks

About 5 dollars a month. The invite code is the only thing keeping strangers out, so
treat it like a password; anyone who knows the address and the code can join the town.
Keep the machine's login to yourself, and let the provider's automatic updates run.
