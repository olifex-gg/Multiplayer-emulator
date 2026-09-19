#!/usr/bin/env bash
# vps-setup.sh - put the town server on a fresh Ubuntu machine and keep it running.
#
# Run it as root on the VPS, once:
#
#   bash vps-setup.sh --code 3hwybg --tz America/New_York \
#                     --source https://github.com/olifex-gg/Multiplayer-emulator.git
#
# What it does: installs a compiler, builds acnet_server, installs it to
# /opt/acnet, runs it as a service that starts with the machine, opens the
# game's UDP port, sets the machine's clock to your time zone (the town's
# clock follows the server's), and keeps 30 days of daily town backups.
#
# Afterwards, put your town file at /opt/acnet/data/towns/<code>/town.gci and
# restart:  systemctl restart acnet
#
# Nothing here touches your PC or anyone's save; it only sets up the server.
set -euo pipefail

CODE=""
TZ_WANT=""
SOURCE_URL="https://github.com/olifex-gg/Multiplayer-emulator.git"
BRANCH="claude/multiplayer-animal-crossing-emulator-3kmegf"
PORT=7777
PREFIX=/opt/acnet
SRC_DIR=/opt/acnet-src

die() { echo "error: $*" >&2; exit 1; }
say() { echo; echo "== $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --code)   CODE="${2:-}"; shift 2 ;;
        --tz)     TZ_WANT="${2:-}"; shift 2 ;;
        --source) SOURCE_URL="${2:-}"; shift 2 ;;
        --branch) BRANCH="${2:-}"; shift 2 ;;
        --port)   PORT="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) die "unknown option $1" ;;
    esac
done

[ "$(id -u)" = "0" ] || die "run this as root (sudo bash vps-setup.sh ...)"
[ -n "$CODE" ] || die "--code is required (the invite code your friends type, e.g. 3hwybg)"
case "$CODE" in
    *[!a-z0-9]*) die "--code must be lowercase letters and digits only" ;;
esac

say "installing the tools needed to build the server"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake git ca-certificates >/dev/null

if [ -n "$TZ_WANT" ]; then
    say "setting the machine's time zone to $TZ_WANT (the town's clock follows it)"
    timedatectl set-timezone "$TZ_WANT"
fi

say "getting the source"
if [ -d "$SRC_DIR/.git" ]; then
    git -C "$SRC_DIR" fetch --depth 1 origin "$BRANCH"
    git -C "$SRC_DIR" checkout -q FETCH_HEAD
else
    rm -rf "$SRC_DIR"
    git clone --depth 1 --branch "$BRANCH" "$SOURCE_URL" "$SRC_DIR" ||
        die "could not download the source. If the repository is private, upload the
     project folder to $SRC_DIR yourself (drag it over with WinSCP) and run this again."
fi

say "building the server"
cmake -S "$SRC_DIR/server" -B "$SRC_DIR/server/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$SRC_DIR/server/build" -j"$(nproc)" >/dev/null
[ -x "$SRC_DIR/server/build/acnet_server" ] || die "the build did not produce acnet_server"

say "installing to $PREFIX"
id -u acnet >/dev/null 2>&1 || useradd --system --home "$PREFIX" --shell /usr/sbin/nologin acnet
install -d -o acnet -g acnet "$PREFIX" "$PREFIX/data" "$PREFIX/data/towns" "$PREFIX/backups"
install -m 755 "$SRC_DIR/server/build/acnet_server" "$PREFIX/acnet_server"
install -m 755 "$SRC_DIR/server/build/acnet_cli" "$PREFIX/acnet_cli" 2>/dev/null || true
if ! grep -qx "$CODE" "$PREFIX/data/invites.txt" 2>/dev/null; then
    echo "$CODE" >> "$PREFIX/data/invites.txt"
fi
install -d -o acnet -g acnet "$PREFIX/data/towns/$CODE"
chown -R acnet:acnet "$PREFIX"

say "installing the service"
cat > /etc/systemd/system/acnet.service <<SERVICE
[Unit]
Description=Animal Crossing town server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=acnet
WorkingDirectory=$PREFIX
ExecStart=$PREFIX/acnet_server --port $PORT --data $PREFIX/data
Restart=always
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true
ReadWritePaths=$PREFIX

[Install]
WantedBy=multi-user.target
SERVICE
systemctl daemon-reload
systemctl enable --now acnet >/dev/null

say "keeping 30 days of town backups (one a day, at 4am)"
cat > /usr/local/bin/acnet-backup <<BACKUP
#!/bin/sh
# Copy every town file to $PREFIX/backups, dated; keep the last 30 days.
set -e
stamp=\$(date +%Y-%m-%d)
for town in $PREFIX/data/towns/*/town.gci; do
    [ -f "\$town" ] || continue
    code=\$(basename "\$(dirname "\$town")")
    cp -p "\$town" "$PREFIX/backups/\$code-\$stamp.gci"
done
find $PREFIX/backups -name '*.gci' -mtime +30 -delete
BACKUP
chmod 755 /usr/local/bin/acnet-backup
cat > /etc/systemd/system/acnet-backup.service <<SERVICE
[Unit]
Description=Back up the town files
[Service]
Type=oneshot
User=acnet
ExecStart=/usr/local/bin/acnet-backup
SERVICE
cat > /etc/systemd/system/acnet-backup.timer <<TIMER
[Unit]
Description=Daily town backup
[Timer]
OnCalendar=*-*-* 04:00:00
Persistent=true
[Install]
WantedBy=timers.target
TIMER
systemctl daemon-reload
systemctl enable --now acnet-backup.timer >/dev/null

if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
    say "opening UDP port $PORT in the machine's firewall"
    ufw allow "$PORT/udp" >/dev/null
else
    echo
    echo "note: no active ufw firewall here. If your hosting provider has its own"
    echo "      firewall page, allow UDP port $PORT there."
fi

ip=$(hostname -I 2>/dev/null | awk '{print $1}')
say "done"
cat <<DONE

The server is running and will start again by itself after a reboot.

  address for your friends : ${ip:-<this machine's public IP>}
  invite code              : $CODE
  port                     : $PORT (UDP)

Next: copy your existing town file to
  $PREFIX/data/towns/$CODE/town.gci
then run:  systemctl restart acnet

Useful later:
  systemctl status acnet      is it running?
  journalctl -u acnet -f      watch who joins and saves
  ls $PREFIX/backups          the daily backups
DONE
