#!/usr/bin/env python3
"""Make, edit, and check synthetic town blobs for the server tests.

The layout constants are read from net/protocol.h so the test cannot drift
from the code. A synthetic town has recognisable fill bytes in each resident's
private and house blocks, a valid save checksum, and a mirrored backup copy.

  mktown.py make OUT [--fill 0xA0]   private[i] = fill+i, home[i] = fill+0x10+i
  mktown.py set FILE SLOT BYTE       refill one resident's blocks, refix checksum
  mktown.py check FILE               validate; prints slot fill bytes; exit 1 if bad
  mktown.py slot FILE SLOT           print the private fill byte of SLOT (or 'mixed')
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(HERE, "..", "..", "net", "protocol.h")


def consts():
    text = open(PROTO).read()
    c = {}
    for m in re.finditer(r"#define\s+(ACNET_[A-Z_0-9]+)\s+(0x[0-9A-Fa-f]+|\d+)\s", text):
        c[m.group(1)] = int(m.group(2), 0)
    c["TOWN_SIZE"] = c["ACNET_GCI_HEADER_SIZE"] + c["ACNET_GCI_PAYLOAD_SIZE"]
    c["MAIN_ABS"] = c["ACNET_GCI_HEADER_SIZE"] + c["ACNET_SAVE_MAIN_OFFSET"]
    c["BACK_ABS"] = c["ACNET_GCI_HEADER_SIZE"] + c["ACNET_SAVE_BACK_OFFSET"]
    return c


C = consts()


def private_range(i):
    off = C["MAIN_ABS"] + C["ACNET_PRIVATE_ARRAY_OFFSET"] + i * C["ACNET_PRIVATE_SIZE"]
    return off, off + C["ACNET_PRIVATE_SIZE"]


def home_range(i):
    off = C["MAIN_ABS"] + C["ACNET_HOME_ARRAY_OFFSET"] + i * C["ACNET_HOME_SIZE"]
    return off, off + C["ACNET_HOME_SIZE"]


def checksum_be(data):
    total = 0
    for k in range(0, len(data) - 1, 2):
        total += (data[k] << 8) | data[k + 1]
    return (-total) & 0xFFFF


def fix(blob):
    m = C["MAIN_ABS"]
    n = C["ACNET_SAVE_T_SIZE"]
    ck = m + C["ACNET_SAVE_CHECKSUM_OFFSET"]
    blob[ck] = 0
    blob[ck + 1] = 0
    s = checksum_be(blob[m:m + n])
    blob[ck] = s >> 8
    blob[ck + 1] = s & 0xFF
    blob[C["BACK_ABS"]:C["BACK_ABS"] + C["ACNET_SAVE_ALIGNED_SIZE"]] = blob[m:m + C["ACNET_SAVE_ALIGNED_SIZE"]]


def make(out, fill):
    blob = bytearray(C["TOWN_SIZE"])
    blob[0:6] = b"GAFE01"
    blob[8:8 + 19] = b"DobutsunomoriP_MURA"
    # land_info sits right after the private array; give it a non-zero id
    land = C["MAIN_ABS"] + C["ACNET_PRIVATE_ARRAY_OFFSET"] + C["ACNET_MAX_PLAYERS"] * C["ACNET_PRIVATE_SIZE"]
    blob[land:land + 8] = b"Testtown"
    for i in range(C["ACNET_MAX_PLAYERS"]):
        a, b = private_range(i)
        blob[a:b] = bytes([(fill + i) & 0xFF]) * (b - a)
        a, b = home_range(i)
        blob[a:b] = bytes([(fill + 0x10 + i) & 0xFF]) * (b - a)
    fix(blob)
    open(out, "wb").write(blob)


def fill_byte(blob, rng):
    a, b = rng
    seg = blob[a:b]
    return seg[0] if seg.count(seg[0:1]) == len(seg) else None


def check(path):
    blob = bytearray(open(path, "rb").read())
    ok = True
    if len(blob) != C["TOWN_SIZE"]:
        print("BAD size %d != %d" % (len(blob), C["TOWN_SIZE"]))
        ok = False
    if blob[0:3] != b"GAF":
        print("BAD magic")
        ok = False
    m = C["MAIN_ABS"]
    n = C["ACNET_SAVE_T_SIZE"]
    total = 0
    for k in range(m, m + n - 1, 2):
        total += (blob[k] << 8) | blob[k + 1]
    if total & 0xFFFF:
        print("BAD checksum (sum=0x%04X)" % (total & 0xFFFF))
        ok = False
    if blob[C["BACK_ABS"]:C["BACK_ABS"] + C["ACNET_SAVE_ALIGNED_SIZE"]] != blob[m:m + C["ACNET_SAVE_ALIGNED_SIZE"]]:
        print("BAD backup copy differs from main")
        ok = False
    parts = []
    for i in range(C["ACNET_MAX_PLAYERS"]):
        p = fill_byte(blob, private_range(i))
        h = fill_byte(blob, home_range(i))
        parts.append("slot%d=%s/%s" % (i, "mixed" if p is None else "0x%02X" % p, "mixed" if h is None else "0x%02X" % h))
    print(("OK " if ok else "INVALID ") + " ".join(parts))
    return ok


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    if cmd == "make":
        fill = 0xA0
        if "--fill" in argv:
            fill = int(argv[argv.index("--fill") + 1], 0)
        make(path, fill)
        return 0
    if cmd == "set":
        slot, byte = int(argv[3]), int(argv[4], 0)
        blob = bytearray(open(path, "rb").read())
        for rng in (private_range(slot), home_range(slot)):
            a, b = rng
            blob[a:b] = bytes([byte & 0xFF]) * (b - a)
        fix(blob)
        open(path, "wb").write(blob)
        return 0
    if cmd == "check":
        return 0 if check(path) else 1
    if cmd == "slot":
        blob = open(path, "rb").read()
        p = fill_byte(blob, private_range(int(argv[3])))
        print("mixed" if p is None else "0x%02X" % p)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
