#!/usr/bin/env python3
"""Manufacture a second resident for a test town, for playtesting puppets
without a second person.

Copies the character (Private_c) in save block SRC of a town blob into block
DST, and the HOUSE that player SRC lives in over the house player DST lives in
(houses are assigned through Save_t.house_arrangement, two bits per player, and
are NOT indexed by save block: in the user's town player 0 lives in house 1),
then renames it and changes its gender, face type and shirt, and writes the
result as a new town blob. An earlier version copied homes[SRC] over homes[DST]
by block index, which wiped the house the OTHER player lived in; after that
every gyroid in the test town said "This house, sadly, is empty." The copied
house keeps SRC's owner id, so the gyroid treats it as SRC's; good enough for a
stand-in. Upload it with
`acnet_cli --name NAME --upload OUT --reason save` while logged in as the
resident who owns block DST: the server keeps only that block from the
upload (every other saved resident's blocks are protected), so nothing else
in the blob matters. The checksum is fixed anyway, so the file is a valid
save on its own too.

  make_resident.py TOWN_IN OUT SRC DST NAME GENDER FACE CLOTH

GENDER is boy or girl, FACE 0..7 (the game's face types), CLOTH the shirt
index (built-in designs are 0..255). Offsets come from net/protocol.h and
include/m_private.h: PersonalID_c at 0 (player_name[8], land_name[8],
player_id, land_id), gender at 0x14, face at 0x15, cloth.idx at 0x1088.
"""
import sys

from mktown import C, fix, home_range, private_range

GENDER_OFF = 0x14
ARRANGEMENT_OFF = 0x02068A  # Save_t.house_arrangement, relative to Save_t
FACE_OFF = 0x15
CLOTH_OFF = 0x1088
ID_OFF = C["ACNET_PRIVATE_ID_OFFSET"]


def main(argv):
    if len(argv) != 9:
        print(__doc__)
        return 2
    town_in, out, src, dst, name, gender, face, cloth = argv[1:9]
    src, dst, face, cloth = int(src), int(dst), int(face), int(cloth)
    if gender not in ("boy", "girl"):
        print("GENDER must be boy or girl")
        return 2
    blob = bytearray(open(town_in, "rb").read())
    if len(blob) != C["TOWN_SIZE"]:
        print("not a town blob: %d bytes" % len(blob))
        return 1

    a, b = private_range(src)
    p, q = private_range(dst)
    blob[p:q] = blob[a:b]
    # Houses go by Save_t.house_arrangement, not by save block.
    arr = blob[C["MAIN_ABS"] + ARRANGEMENT_OFF]
    src_house = (arr >> (src * 2)) & 3
    dst_house = (arr >> (dst * 2)) & 3
    if src_house != dst_house:
        a, b = home_range(src_house)
        p2, q2 = home_range(dst_house)
        blob[p2:q2] = blob[a:b]
    print("player %d lives in house %d, player %d in house %d" % (src, src_house, dst, dst_house))

    blob[p:p + 8] = name.encode("ascii").ljust(8, b" ")[:8]   # player_name, space padded
    pid = ((blob[p + ID_OFF] << 8) | blob[p + ID_OFF + 1]) ^ 0x0001  # a distinct player_id
    blob[p + ID_OFF] = pid >> 8
    blob[p + ID_OFF + 1] = pid & 0xFF
    blob[p + GENDER_OFF] = 1 if gender == "girl" else 0
    blob[p + FACE_OFF] = face
    blob[p + CLOTH_OFF] = (cloth >> 8) & 0xFF
    blob[p + CLOTH_OFF + 1] = cloth & 0xFF
    fix(blob)  # valid checksum and backup copy, so the blob also works as a save file
    open(out, "wb").write(blob)
    print("wrote %s: block %d = %s, %s, face %d, shirt %d, player_id 0x%04x" % (out, dst, name, gender, face, cloth, pid))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
