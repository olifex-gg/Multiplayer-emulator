#!/usr/bin/env bash
# End-to-end test: starts a server on a random port and drives it with the
# CLI client. Covers login and slot assignment, founding a town, the
# per-resident splice on upload, checksum repair, the eight-resident limit,
# chat relay, world-authority migration, a town still stored in the
# four-resident layout, and the roster (a ninth resident takes the seat of
# someone who is away; what they saved comes back when they return).
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
printf '%s\nlegacytown\nseatstown\n' "$INVITE" > "$TMP/data/invites.txt"
# A town as the four-resident builds stored it: resident 0 has a character,
# blocks 1-3 are empty. Served as it is, and replaced by the first save.
mkdir -p "$TMP/data/towns/legacytown"
python3 - "$TMP/data/towns/legacytown/town.gci" "$HERE" <<'PYEOF'
import sys
sys.path.insert(0, sys.argv[2])
from mktown import C
main = C["ACNET_GCI_HEADER_SIZE"] + C["ACNET_LEGACY_SAVE_MAIN_OFFSET"]
blob = bytearray(C["ACNET_GCI_HEADER_SIZE"] + C["ACNET_LEGACY_GCI_PAYLOAD_SIZE"])
blob[0:6] = b"GAFE01"
blob[8:8 + 19] = b"DobutsunomoriP_MURA"
for i in range(C["ACNET_LEGACY_MAX_PLAYERS"]):
    a = main + C["ACNET_LEGACY_PRIVATE_ARRAY_OFFSET"] + i * C["ACNET_PRIVATE_SIZE"]
    blob[a:a + C["ACNET_PRIVATE_SIZE"]] = (b"\x50" if i == 0 else b"\xff") * C["ACNET_PRIVATE_SIZE"]
open(sys.argv[1], "wb").write(blob)
PYEOF
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
grep -q "^OK slot0=0xA0/0xB0 slot1=0xBB/0xBB slot2=0xCC/0xCC slot3=0xA3/0xB3 slot4=0xA4/0xB4 slot5=0xA5/0xB5 slot6=0xA6/0xB6 slot7=0xA7/0xB7" "$TMP/check.txt" || fail "splice result wrong"

echo "9. a malformed upload is rejected and does not bump the version"
head -c 1000 "$TMP/townA.gci" > "$TMP/short.gci"
out=$(cli --name alice --upload "$TMP/short.gci")
grep -q "ACK status=1 version=2" <<<"$out" || fail "short upload: $out"

echo "10. a resident keeps their slot across logins; the ninth name is refused"
out=$(cli --name bob); grep -q "slot=1" <<<"$out" || fail "bob slot changed: $out"
out=$(cli --name dave); grep -q "slot=3" <<<"$out" || fail "dave slot: $out"
out=$(cli --name erin); grep -q "slot=4" <<<"$out" || fail "erin slot: $out"
out=$(cli --name frank); grep -q "slot=5" <<<"$out" || fail "frank slot: $out"
out=$(cli --name gina); grep -q "slot=6" <<<"$out" || fail "gina slot: $out"
out=$(cli --name hank); grep -q "slot=7" <<<"$out" || fail "hank slot: $out"
out=$(cli --name ivan || true); grep -q "REJECT reason=3" <<<"$out" || fail "expected TOWN_FULL: $out"

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
out=$(cli --name ivan || true)
grep -q "REJECT reason=3" <<<"$out" || fail "town should be full after eight residents: $out"

echo "14. a resident's save is pushed live to the others (character + house blocks)"
cli --name alice --wait 4 > "$TMP/alice_live.txt" &
LPID=$!
sleep 0.7
out=$(cli --name bob --upload "$TMP/townB.gci")
grep -q "ACK status=0 version=3" <<<"$out" || fail "bob live upload: $out"
wait "$LPID" || true
grep -q "RESIDENT slot=1 house=1 version=3 bytes=19184" "$TMP/alice_live.txt" || fail "alice got no live resident data: $(cat "$TMP/alice_live.txt")"

echo "15. a changed land cell is relayed live and written into the stored town"
cli --name alice --wait 3 > "$TMP/alice_land.txt" &
LAPID=$!
sleep 0.7
out=$(cli --name bob --land 2,3,4,5,4660)
wait "$LAPID" || true
grep -q "LAND count=1 cell=2,3,4,5,4660" "$TMP/alice_land.txt" || fail "alice got no land cell: $(cat "$TMP/alice_land.txt")"
sleep 6   # server flushes live land edits to disk after 5 s
python3 - "$TMP/data/towns/$INVITE/town.gci" "$HERE" <<'PYEOF' || fail "land cell not in stored town"
import sys
sys.path.insert(0, sys.argv[2])
from mktown import C
FG_ABS = C["MAIN_ABS"] + C["ACNET_FG_OFFSET"]
idx = ((3 * 5 + 2) * 16 + 5) * 16 + 4
b = open(sys.argv[1], 'rb').read()
v = (b[FG_ABS + idx * 2] << 8) | b[FG_ABS + idx * 2 + 1]
print("stored cell =", v)
sys.exit(0 if v == 4660 else 1)
PYEOF
$MK check "$TMP/data/towns/$INVITE/town.gci" | grep -q "^OK" || fail "stored town checksum broken after land flush"

echo "16. a player-state packet reaches the others with its animation state intact"
cli --name alice --wait 3 > "$TMP/alice_state.txt" &
SAPID=$!
sleep 0.7
cli --name bob --state 1,2,3 --anim 5 --wait 1
wait "$SAPID" || true
grep -q "STATE .* area=7 pos=1.0,2.0,3.0 anim=5/5 part=0 frame=1.0/1.0 speed=0.00 flags=0" "$TMP/alice_state.txt" || fail "alice got no usable state: $(cat "$TMP/alice_state.txt")"

echo "17. a resident's slot follows the save block their character lives in"
# bob is resident 1; carol holds 2 but has never saved, so bob's claim swaps them.
cli --name alice --wait 3 > "$TMP/alice_slot.txt" &
CLPID=$!
sleep 0.7
out=$(cli --name bob --claim 2)
grep -q "SLOT slot=2 reason=1" <<<"$out" || fail "bob's claim: $out"
out=$(cli --status)
grep -q "^SLOT 2 bob" <<<"$out" || fail "bob should now own slot 2: $out"
grep -q "^SLOT 1 carol" <<<"$out" || fail "carol should have taken slot 1: $out"
wait "$CLPID" || true
grep -q "LEFT client_id=[0-9]* slot=1 name=bob" "$TMP/alice_slot.txt" || fail "alice saw no LEFT for bob's old slot: $(cat "$TMP/alice_slot.txt")"
grep -q "JOIN client_id=[0-9]* slot=2 name=bob" "$TMP/alice_slot.txt" || fail "alice saw no JOIN for bob's new slot: $(cat "$TMP/alice_slot.txt")"
# alice has saved into block 0, so nobody can take it.
out=$(cli --name bob --claim 0)
grep -q "SLOT slot=2 reason=2" <<<"$out" || fail "a saved character's block must not be taken: $out"
# The next login keeps the corrected slot, and bob's uploads now protect block 2.
out=$(cli --name bob); grep -q "slot=2" <<<"$out" || fail "bob's slot did not persist: $out"
$MK check "$TMP/data/towns/$INVITE/town.gci" | grep -q "^OK" || fail "town broken after slot move"

echo "18. the world authority's weather reaches the others; anyone else's is dropped"
cli --name alice --weather 1,2 --wait 3 > "$TMP/alice_wx.txt" &   # alone, so alice is the authority
WXPID=$!
sleep 0.7
out=$(cli --name bob --wait 2)
grep -q "WEATHER type=1 intensity=2" <<<"$out" || fail "bob did not get the authority's weather: $out"
wait "$WXPID" || true
cli --name alice --wait 3 > "$TMP/alice_wx2.txt" &
WXPID=$!
sleep 0.7
cli --name bob --weather 3,1 --wait 1 > /dev/null
wait "$WXPID" || true
! grep -q "WEATHER" "$TMP/alice_wx2.txt" || fail "a non-authority's weather was relayed: $(cat "$TMP/alice_wx2.txt")"

echo "19. a runtime placeholder is never stored as land, nor passed on"
cli --name alice --wait 3 > "$TMP/alice_rt.txt" &
RTPID=$!
sleep 0.7
cli --name bob --land 2,3,4,5,61717 --wait 1 > /dev/null     # 0xF115 = DUMMY_DUMP
wait "$RTPID" || true
! grep -q "LAND" "$TMP/alice_rt.txt" || fail "a placeholder cell was relayed: $(cat "$TMP/alice_rt.txt")"
sleep 6
python3 - "$TMP/data/towns/$INVITE/town.gci" "$HERE" <<'PYEOF' || fail "placeholder cell reached the stored town"
import sys
sys.path.insert(0, sys.argv[2])
from mktown import C
FG_ABS = C["MAIN_ABS"] + C["ACNET_FG_OFFSET"]
idx = ((3 * 5 + 2) * 16 + 5) * 16 + 4
b = open(sys.argv[1], 'rb').read()
v = (b[FG_ABS + idx * 2] << 8) | b[FG_ABS + idx * 2 + 1]
print("stored cell =", v)
sys.exit(0 if v == 4660 else 1)
PYEOF

echo "20. a resident whose game died silently can log in again once it has gone quiet"
out=$(cli --name bob --vanish)
grep -q "VANISHED" <<<"$out" || fail "vanish: $out"
out=$(cli --name bob || true)
grep -q "REJECT reason=4" <<<"$out" || fail "a fresh silent session must still count as connected: $out"
sleep 6
out=$(cli --name bob)
grep -q "WELCOME client_id=[0-9]* slot=2" <<<"$out" || fail "bob could not replace his stale session: $out"

echo "21. a villager's state reaches the others, stamped with who sent it"
cli --name alice --wait 3 > "$TMP/alice_npc.txt" &
NPPID=$!
sleep 0.7
cli --name bob --npc 4660,100,200 --wait 1 > "$TMP/bob_npc.txt"
wait "$NPPID" || true
bob_id=$(grep -o "WELCOME client_id=[0-9]*" "$TMP/bob_npc.txt" | grep -o "[0-9]*$")
grep -q "NPC client_id=$bob_id npc=4660 pos=100.0,0.0,200.0 angle=0 walking=0 act=255 flags=0" "$TMP/alice_npc.txt" || fail "alice got no villager state: $(cat "$TMP/alice_npc.txt")"

echo "22. a villager's act and flags travel with it"
cli --name alice --wait 3 > "$TMP/alice_npc2.txt" &
NPPID=$!
sleep 0.7
cli --name bob --npc 4661,10,20,9,3 --wait 1 > /dev/null
wait "$NPPID" || true
grep -q "NPC client_id=[0-9]* npc=4661 pos=10.0,0.0,20.0 angle=0 walking=0 act=9 flags=3" "$TMP/alice_npc2.txt" || fail "act/flags lost: $(cat "$TMP/alice_npc2.txt")"

echo "23. a resident can push their own blocks without saving; they are stored, protected and relayed"
# bob is resident 2. Push blocks filled with 0xEE out of a crafted town; the
# rest of that town must not matter.
cp "$TMP/dl_bob.gci" "$TMP/townP.gci"
$MK set "$TMP/townP.gci" 2 0xEE
$MK set "$TMP/townP.gci" 0 0x11      # tampering with alice's block in the pushed file changes nothing
cli --name alice --wait 3 > "$TMP/alice_push.txt" &
PUPID=$!
sleep 0.7
out=$(cli --name bob --push "$TMP/townP.gci" --wait 1)
grep -q "PUSHED slot=2" <<<"$out" || fail "push: $out"
wait "$PUPID" || true
grep -q "RESIDENT slot=2 house=2 version=[0-9]* bytes=$((0x2440 + 0x26B0))" "$TMP/alice_push.txt" || fail "alice did not get bob's pushed blocks: $(cat "$TMP/alice_push.txt")"
sleep 1
$MK check "$TMP/data/towns/$INVITE/town.gci" > "$TMP/check_push.txt"
grep -q "^OK .*slot2=0xEE/0xEE" "$TMP/check_push.txt" || fail "pushed blocks not stored: $(cat "$TMP/check_push.txt")"
grep -q "slot0=0xA0/0xB0" "$TMP/check_push.txt" || fail "a push must only touch the pusher's blocks: $(cat "$TMP/check_push.txt")"
grep -q "^2 1 bob" "$TMP/data/towns/$INVITE/residents.txt" || fail "bob should count as saved after a push: $(cat "$TMP/data/towns/$INVITE/residents.txt")"
# From now on nobody else's upload can overwrite block 2.
cli --name alice --download "$TMP/townQ.gci" > /dev/null
$MK set "$TMP/townQ.gci" 2 0x22
sleep 2
out=$(cli --name alice --upload "$TMP/townQ.gci")
grep -q "ACK status=0" <<<"$out" || fail "alice's upload after the push: $out"
$MK check "$TMP/data/towns/$INVITE/town.gci" | grep -q "slot2=0xEE/0xEE" || fail "an upload overwrote pushed blocks"
# A push for somebody else's slot is ignored (the CLI can only push its own, so check the server log stays clean).
! grep -q "ignoring blocks" "$TMP/server.log" || fail "unexpected foreign push"

echo "24. the welcome carries the server's clock and time zone"
out=$(cli --name carol --ping)
grep -q "WELCOME .* server_ms=[0-9]* tz=-\?[0-9]*" <<<"$out" || fail "no clock in welcome: $out"
grep -q "PONG nonce=12648430 server_ms=[0-9]* tz=-\?[0-9]*" <<<"$out" || fail "no clock in pong: $out"
tz=$(grep -o "WELCOME .* tz=-\?[0-9]*" <<<"$out" | grep -o "[-0-9]*$")
want=$(python3 -c "import time; print(-(time.altzone if time.localtime().tm_isdst > 0 else time.timezone) // 60)")
[[ "$tz" == "$want" ]] || fail "server time zone $tz, this machine says $want"

echo "25. a town stored by the four-resident builds is served as it is, its slots read at the old offsets, and replaced by the first save"
lcli() { "$CLI" --server 127.0.0.1 --port "$PORT" --invite legacytown --quiet "$@"; }
grep -q "four-resident town file" "$TMP/server.log" || true
out=$(lcli --name zed --download "$TMP/dl_legacy.gci")
grep -q "WELCOME client_id=[0-9]* slot=1 .*town_present=1" <<<"$out" || fail "zed should get slot 1, the first empty block of the old town: $out"
cmp "$TMP/dl_legacy.gci" "$TMP/data/towns/legacytown/town.gci" || fail "the old town must be served byte for byte"
[[ $(stat -c %s "$TMP/dl_legacy.gci") -eq $((64 + 0x72000)) ]] || fail "old town download has the wrong size"
$MK make "$TMP/townL.gci" --fill 0x60
out=$(lcli --name zed --upload "$TMP/townL.gci")
grep -q "ACK status=0 version=1" <<<"$out" || fail "zed's upload of the new layout: $out"
[[ $(stat -c %s "$TMP/data/towns/legacytown/town.gci") -eq $((64 + 0xA2000)) ]] || fail "the stored town was not replaced by the eight-resident layout"
$MK check "$TMP/data/towns/legacytown/town.gci" | grep -q "^OK slot0=0x60/0x70" || fail "upgraded town wrong"
grep -q "now stored in the eight-resident layout" "$TMP/server.log" || fail "server did not report the upgrade"

echo "26. a ninth resident takes the seat of whoever is away and was seen longest ago; the roster gives it back"
scli() { "$CLI" --server 127.0.0.1 --port "$PORT" --invite seatstown --quiet "$@"; }
SD="$TMP/data/towns/seatstown"
$MK make "$TMP/townS.gci" --fill 0xA0 --residents 1
out=$(scli --name p1 --upload "$TMP/townS.gci" --reason new)
grep -q "ACK status=0 version=1" <<<"$out" || fail "p1 founds seatstown: $out"
[[ -f "$SD/empty.bin" ]] || fail "the server did not keep an empty-block template"
[[ $(stat -c %s "$SD/empty.bin") -eq $((0x2440 + 0x26B0)) ]] || fail "template size"
for n in 2 3 4 5 6 7 8; do
  out=$(scli --name p$n)
  grep -q "WELCOME client_id=[0-9]* slot=$((n-1)) " <<<"$out" || fail "p$n should get slot $((n-1)): $out"
done
# Everyone saves (a push counts), so a newcomer must evict a saved resident:
# the one seen longest ago, which is p1, the founder.
for n in 2 3 4 5 6 7 8; do
  scli --name p$n --download "$TMP/townS_$n.gci" > /dev/null
  $MK set "$TMP/townS_$n.gci" $((n-1)) $((0xC0 + n))
  out=$(scli --name p$n --push "$TMP/townS_$n.gci" --wait 1)
  grep -q "PUSHED slot=$((n-1)) house=$((n-1))" <<<"$out" || fail "p$n push: $out"
done
sleep 1.2
out=$(scli --name p9)
grep -q "WELCOME client_id=[0-9]* slot=0 " <<<"$out" || fail "p9 should take p1's seat: $out"
grep -q "p1 moved out of seat 0 (their character and house are kept in the roster) so p9 can move in" "$TMP/server.log" || fail "server did not report the move: $(tail -n 5 "$TMP/server.log")"
[[ -f "$SD/roster/p1.bin" ]] || fail "p1's character and house were not kept"
[[ $(stat -c %s "$SD/roster/p1.bin") -eq $((0x2440 + 0x26B0)) ]] || fail "roster file size"
$MK check "$SD/town.gci" > "$TMP/check_seat.txt"
grep -q "slot0=0xFF/0xFF" "$TMP/check_seat.txt" || fail "seat 0 was not blanked for p9: $(cat "$TMP/check_seat.txt")"
grep -q "slot1=0xC2/0xC2" "$TMP/check_seat.txt" || fail "p2's pushed blocks must stay: $(cat "$TMP/check_seat.txt")"
grep -q "^0 0 p9 [0-9]" "$SD/residents.txt" || fail "residents.txt after p9: $(cat "$SD/residents.txt")"
# p1 comes back: p9 never saved, so p9 (nothing to lose) gives up seat 0
# before any saved resident would, and p1's own character and house come
# back from the roster into it.
sleep 1.2
out=$(scli --name p1)
grep -q "WELCOME client_id=[0-9]* slot=0 " <<<"$out" || fail "p1 should be seated where p9 was: $out"
grep -q "p9 moved out of seat 0 (they never saved) so p1 can move in; their own character and house are back from the roster" "$TMP/server.log" || fail "server did not report p1's return: $(tail -n 5 "$TMP/server.log")"
$MK check "$SD/town.gci" | grep -q "slot0=0xA0/0xB0" || fail "p1's saved character and house did not come back: $($MK check "$SD/town.gci")"
! [[ -f "$SD/roster/p9.bin" ]] || fail "p9 never saved, so nothing should be kept for them"
grep -q "^0 1 p1 [0-9]" "$SD/residents.txt" || fail "residents.txt after p1's return: $(cat "$SD/residents.txt")"
# The main town is full of characters and has no empty block to copy, so its
# ninth login is still refused (step 10) rather than evicting anyone.
! [[ -f "$TMP/data/towns/$INVITE/empty.bin" ]] || fail "the full town must not have a template"

echo "ALL PASSED"
