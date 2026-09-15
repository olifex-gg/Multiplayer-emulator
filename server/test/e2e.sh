#!/usr/bin/env bash
# End-to-end test: starts a server on a random port and drives it with the
# CLI client. Covers login and slot assignment, founding a town, the
# per-resident splice on upload, checksum repair, the four-resident limit,
# chat relay, and world-authority migration.
set -euo pipefail

SERVER=${1:?server binary}
CLI=${2:?cli binary}
HERE=$(cd "$(dirname "$0")" && pwd)
MK="python3 $HERE/mktown.py"
TMP=$(mktemp -d)
PORT=$((20000 + RANDOM % 20000))
INVITE=testinvite

cleanup() {
    if [[ -n "${SPID:-}" ]]; then kill "$SPID" 2>/dev/null || true; wait "$SPID" 2>/dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT

fail() { echo "FAIL: $*"; echo "--- server log ---"; cat "$TMP/server.log" || true; exit 1; }
cli() { "$CLI" --server 127.0.0.1 --port "$PORT" --invite "$INVITE" --quiet "$@"; }

# Wait until the server has actually bound its port before any client talks to
# it. A fixed short sleep is not enough on Windows, where a UDP packet sent to
# a port nobody listens on yet comes back as an ICMP error that aborts the
# client's socket, so the first connect attempt fails outright.
wait_for_server() {
    local i
    for i in $(seq 1 100); do
        kill -0 "$SPID" 2>/dev/null || fail "server did not start"
        grep -q "listening on UDP" "$TMP/server.log" 2>/dev/null && return 0
        sleep 0.1
    done
    fail "server never reported listening"
}

mkdir -p "$TMP/data"
echo "$INVITE" > "$TMP/data/invites.txt"
"$SERVER" --port "$PORT" --data "$TMP/data" --verbose > "$TMP/server.log" 2>&1 &
SPID=$!
wait_for_server

echo "1. wrong invite is rejected"
out=$("$CLI" --server 127.0.0.1 --port "$PORT" --invite nope --name alice --quiet || true)
grep -q "REJECT reason=2" <<<"$out" || fail "expected REJECT bad invite, got: $out"

echo "2. bad name is rejected"
out=$(cli --name 'bad name!' || true)
grep -q "REJECT reason=7" <<<"$out" || fail "expected REJECT bad name, got: $out"

echo "3. first resident logs in to an empty town"
out=$(cli --name alice --download "$TMP/dl_empty.gci")
grep -q "WELCOME client_id=[0-9]* slot=0 .*town_present=0 version=0" <<<"$out" || fail "alice welcome: $out"
grep -q "TOWN present=0" <<<"$out" || fail "expected no town yet: $out"

echo "4. alice founds the town"
$MK make "$TMP/townA.gci" --fill 0xA0
out=$(cli --name alice --upload "$TMP/townA.gci" --reason new)
grep -q "ACK status=0 version=1" <<<"$out" || fail "founding upload: $out"
$MK check "$TMP/data/towns/$INVITE/town.gci" | grep -q "^OK slot0=0xA0/0xB0 slot1=0xA1/0xB1" || fail "stored town wrong"

echo "5. bob gets slot 1 and the same town"
out=$(cli --name bob --download "$TMP/dl_bob.gci")
grep -q "WELCOME client_id=[0-9]* slot=1 .*town_present=1 version=1" <<<"$out" || fail "bob welcome: $out"
cmp "$TMP/dl_bob.gci" "$TMP/townA.gci" || fail "bob's download differs from founding upload"

echo "6. carol claims slot 2 but never uploads"
out=$(cli --name carol)
grep -q "slot=2" <<<"$out" || fail "carol welcome: $out"

echo "7. bob uploads with his own block changed, and alice's and carol's blocks tampered"
cp "$TMP/dl_bob.gci" "$TMP/townB.gci"
$MK set "$TMP/townB.gci" 1 0xBB
$MK set "$TMP/townB.gci" 0 0xFF   # must be rejected: alice has uploaded
$MK set "$TMP/townB.gci" 2 0xCC   # must be accepted: carol has not uploaded yet
out=$(cli --name bob --upload "$TMP/townB.gci")
grep -q "ACK status=0 version=2" <<<"$out" || fail "bob upload: $out"

echo "8. alice downloads: her block survived, bob's changed, carol's taken from bob"
out=$(cli --name alice --download "$TMP/dl_alice2.gci")
grep -q "TOWN present=1 version=2" <<<"$out" || fail "alice download: $out"
$MK check "$TMP/dl_alice2.gci" | tee "$TMP/check.txt"
grep -q "^OK slot0=0xA0/0xB0 slot1=0xBB/0xBB slot2=0xCC/0xCC slot3=0xA3/0xB3" "$TMP/check.txt" || fail "splice result wrong"

echo "9. a malformed upload is rejected and does not bump the version"
head -c 1000 "$TMP/townA.gci" > "$TMP/short.gci"
out=$(cli --name alice --upload "$TMP/short.gci")
grep -q "ACK status=1 version=2" <<<"$out" || fail "short upload: $out"

echo "10. a resident keeps their slot across logins; the fifth name is refused"
out=$(cli --name bob); grep -q "slot=1" <<<"$out" || fail "bob slot changed: $out"
out=$(cli --name dave); grep -q "slot=3" <<<"$out" || fail "dave slot: $out"
out=$(cli --name erin || true); grep -q "REJECT reason=3" <<<"$out" || fail "expected TOWN_FULL: $out"

echo "11. chat relay and world-authority migration"
cli --name alice --wait 3 > "$TMP/alice_wait.txt" &
APID=$!
sleep 0.7
cli --name bob --chat "hello town" --wait 5 > "$TMP/bob_wait.txt" &
BPID=$!
sleep 0.7
out=$(cli --name carol --ping)
grep -q "PONG nonce=12648430" <<<"$out" || fail "ping: $out"
wait "$APID" || true
grep -q "JOIN client_id=[0-9]* slot=1 name=bob" "$TMP/alice_wait.txt" || fail "alice saw no join: $(cat "$TMP/alice_wait.txt")"
grep -q "CHAT slot=1 text=hello town" "$TMP/alice_wait.txt" || fail "alice saw no chat: $(cat "$TMP/alice_wait.txt")"
wait "$BPID" || true
# alice logged in first, so she was authority; when she left, bob must have been promoted
bob_id=$(grep -o "WELCOME client_id=[0-9]*" "$TMP/bob_wait.txt" | grep -o "[0-9]*$")
grep -q "AUTHORITY client_id=$bob_id" "$TMP/bob_wait.txt" || fail "bob was not promoted: $(cat "$TMP/bob_wait.txt")"

echo "12. server persists across restart"
kill "$SPID"; wait "$SPID" 2>/dev/null || true
"$SERVER" --port "$PORT" --data "$TMP/data" > "$TMP/server.log" 2>&1 &
SPID=$!
wait_for_server
out=$(cli --name carol --download "$TMP/dl_after.gci")
grep -q "WELCOME client_id=[0-9]* slot=2 .*town_present=1 version=2" <<<"$out" || fail "after restart: $out"
cmp "$TMP/dl_after.gci" "$TMP/dl_alice2.gci" || fail "town changed across restart"

echo "13. lobby status query (no login, claims no slot, creates no town)"
# Unknown invite code: reported as unknown, and no town directory is created for it.
out=$("$CLI" --server 127.0.0.1 --port "$PORT" --invite zzzznope --status --quiet)
grep -q "STATUS room_known=0" <<<"$out" || fail "unknown invite should be unknown: $out"
[ ! -d "$TMP/data/towns/zzzznope" ] || fail "a status query created a town directory"
# Known town: reports residents, marks who is connected, and does not take a slot.
cli --name dave --wait 4 > "$TMP/dave_wait.txt" &
DPID=$!
sleep 0.7
out=$(cli --status)
grep -q "STATUS room_known=1 town_present=1" <<<"$out" || fail "known town status: $out"
grep -q "^SLOT 3 dave online" <<<"$out" || fail "dave should show online: $out"
grep -q "^SLOT 0 alice away" <<<"$out" || fail "alice should show away: $out"
wait "$DPID" || true
# After dave disconnects he still owns his slot, now away.
out=$(cli --status)
grep -q "^SLOT 3 dave away" <<<"$out" || fail "dave should still own his slot: $out"
# The status queries must not have consumed a resident slot.
out=$(cli --name erin || true)
grep -q "REJECT reason=3" <<<"$out" || fail "town should be full after four residents: $out"

echo "14. a resident's save is pushed live to the others (character + house blocks)"
cli --name alice --wait 4 > "$TMP/alice_live.txt" &
LPID=$!
sleep 0.7
out=$(cli --name bob --upload "$TMP/townB.gci")
grep -q "ACK status=0 version=3" <<<"$out" || fail "bob live upload: $out"
wait "$LPID" || true
grep -q "RESIDENT slot=1 version=3 bytes=19184" "$TMP/alice_live.txt" || fail "alice got no live resident data: $(cat "$TMP/alice_live.txt")"

echo "15. a changed land cell is relayed live and written into the stored town"
cli --name alice --wait 3 > "$TMP/alice_land.txt" &
LAPID=$!
sleep 0.7
out=$(cli --name bob --land 2,3,4,5,4660)
wait "$LAPID" || true
grep -q "LAND count=1 cell=2,3,4,5,4660" "$TMP/alice_land.txt" || fail "alice got no land cell: $(cat "$TMP/alice_land.txt")"
sleep 6   # server flushes live land edits to disk after 5 s
python3 - "$TMP/data/towns/$INVITE/town.gci" <<'PYEOF' || fail "land cell not in stored town"
import sys
FG_ABS = 64 + 0x26000 + 0x137A8
idx = ((3 * 5 + 2) * 16 + 5) * 16 + 4
b = open(sys.argv[1], 'rb').read()
v = (b[FG_ABS + idx * 2] << 8) | b[FG_ABS + idx * 2 + 1]
print("stored cell =", v)
sys.exit(0 if v == 4660 else 1)
PYEOF
$MK check "$TMP/data/towns/$INVITE/town.gci" | grep -q "^OK" || fail "stored town checksum broken after land flush"

echo "ALL PASSED"
