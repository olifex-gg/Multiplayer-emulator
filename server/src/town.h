/* town.h - town vault: the canonical save for one shared town on disk.
 *
 * A town is stored exactly as the game writes it: a Dolphin-compatible GCI
 * file (64-byte CARDDir header + 0xA2000 payload, eight residents). The
 * server never interprets game state beyond the fixed offsets in
 * net/protocol.h, which is enough to splice each resident's own blocks and
 * fix the save checksum.
 *
 * A town written by the four-resident builds (0x72000 payload) is served as
 * it is: the game converts it on load and uploads the eight-resident layout
 * on its next save, which replaces the stored file. Until then the server
 * reads the old offsets for slots 0-3 and has no blocks for slots 4-7.
 */
#ifndef ACNET_TOWN_H
#define ACNET_TOWN_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "protocol.h"

#define TOWN_DIR_MAX 512

typedef struct {
    char invite[ACNET_INVITE_LEN + 1];
    char dir[TOWN_DIR_MAX];
    uint8_t* data;               /* data_len bytes, or NULL when no town yet */
    size_t data_len;             /* ACNET_TOWN_SIZE, or ACNET_LEGACY_TOWN_SIZE for an old town */
    uint32_t version;            /* bumped on every accepted upload, persisted */
    char slot_owner[ACNET_MAX_PLAYERS][ACNET_NAME_LEN + 1]; /* "" = unassigned */
    uint8_t slot_uploaded[ACNET_MAX_PLAYERS];               /* owner uploaded at least once */
    time_t  slot_last_seen[ACNET_MAX_PLAYERS];              /* last login or logout, 0 = unknown */
    uint8_t* empty_blocks;   /* what a never-used character block + house block look like in
                              * this town (ACNET_RESIDENT_BLOB_SIZE, from a slot nobody has
                              * used); needed to blank a seat. NULL until one has been seen. */
} town_t;

/* Open (or create the directory for) the town keyed by invite code under
 * data_root. Loads town.gci, residents.txt, town.version if present.
 * Returns 0 on success, -1 on error. */
int town_open(town_t* t, const char* data_root, const char* invite);

/* 1 when a town directory for this invite code already exists under data_root.
 * The lobby status path uses this so a status request never creates a town. */
int town_exists_on_disk(const char* data_root, const char* invite);
void town_close(town_t* t);

/* Resolve a resident name to a slot. An existing name gets its slot back.
 * A new name gets want_slot if free (or the lowest free slot when want_slot
 * is ACNET_SLOT_ANY).
 *
 * The roster: when all eight seats are owned and the town can blank one
 * (town_seats_available), the seat of a resident who is away is freed for
 * the newcomer -- first choice someone who never saved (nothing to keep),
 * else the one seen longest ago. The evicted resident's saved character and
 * house are kept in roster/<name>.bin and come back the next time they log
 * in, into whatever seat is freed for them then; the seat is filled with the
 * newcomer's own roster blocks if they have any, else blanked with the
 * empty-block template. `online(ctx, name)` says whether a name is connected
 * right now (never evicted). `evicted` (ACNET_NAME_LEN + 1 bytes) receives
 * the name moved out, or "".
 * Returns the slot, or -1 when the town is full and nobody can be moved. */
typedef int (*town_online_fn)(void* ctx, const char* name);
int town_assign_slot(town_t* t, const char* name, int want_slot, town_online_fn online, void* ctx, char* evicted);

/* 1 when a seat could be freed for a newcomer: an eight-resident town whose
 * empty-block template has been captured. */
int town_seats_available(const town_t* t);

/* Which Save_t.homes[] block holds slot's house, read from the stored town's
 * house_arrangement (a fifth resident's house is the free one they picked,
 * not block 4). The slot itself for a four-resident file or no town. */
int town_house_of_slot(const town_t* t, int slot);

/* Note that the resident in `slot` is here now (login) or has just left. */
void town_touch_resident(town_t* t, int slot);

/* 1 when the stored town has a character in save block i (its PersonalID
 * is not the empty 0xFFFF pair). 0 with no town yet. */
int town_block_has_character(const town_t* t, int i);

/* Move resident `name` from slot `from` to slot `to` because that is the
 * save block their character lives in. Whoever held `to` without ever
 * saving takes `from` instead (their name is copied to *displaced, "" if
 * nobody). Refused (returns 0) when `to` is another resident's saved
 * character. Returns 1 on success, including the no-op from == to. */
int town_move_resident(town_t* t, const char* name, int from, int to, char* displaced);

/* Sanity check on an uploaded blob: exact size and GCI magic. 1 = ok. */
int town_validate_blob(const uint8_t* blob, size_t len);
/* The same for a file read back from disk, which may still be in the
 * four-resident layout. */
int town_validate_blob_any(const uint8_t* blob, size_t len);
/* 1 when the stored town is in the four-resident layout. */
int town_is_legacy(const town_t* t);

/* Accept an upload from the resident in uploader_slot. Blocks owned by other
 * residents who have already uploaded are copied back in from the current
 * canonical town, the save checksum is recomputed, the backup copy is
 * mirrored, and the result is written atomically. Returns the new version,
 * or 0 on failure. */
uint32_t town_apply_upload(town_t* t, int uploader_slot, const uint8_t* blob);

/* Write one field-item cell into the stored town (big-endian). Returns 1 if
 * written, 0 when there is no town yet or the indices are out of range. The
 * caller persists later with town_flush(). */
int town_set_land_cell(town_t* t, int fx, int fz, int utx, int utz, uint16_t item);

/* A resident pushed their own two blocks (Private_c then mHm_hs_c, big-endian,
 * ACNET_RESIDENT_BLOB_SIZE bytes) without saving. `house` is the homes[]
 * block the house half belongs to (the pusher's arrangement entry; 0xFF =
 * whatever the stored town says), and the stored arrangement is updated to
 * match. Writes them into the stored town, marks the resident as having
 * uploaded (so other residents' uploads can no longer overwrite the blocks)
 * and writes the town to disk. The version is not bumped. Returns 1 if
 * stored, 0 when there is no town yet (the caller may still relay the
 * blocks) or on a bad slot. */
int town_set_resident_blocks(town_t* t, int slot, int house, const uint8_t* blob);

/* Recompute the checksum, mirror main -> backup and write the town to disk.
 * Used after live land edits; does not bump the version or rotate backups. */
int town_flush(town_t* t);

/* Big-endian 16-bit two's complement checksum as used by the game's save
 * check. Computed over the whole Save_t with the checksum field zeroed. */
uint16_t town_checksum_be(const uint8_t* data, size_t size);

/* Recompute and store the checksum of the main Save_t inside a payload and
 * mirror main -> backup. Exposed for tools and tests. */
void town_fix_payload(uint8_t* payload);

#endif /* ACNET_TOWN_H */
