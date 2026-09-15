/* town.h - town vault: the canonical save for one shared town on disk.
 *
 * A town is stored exactly as the game writes it: a Dolphin-compatible GCI
 * file (64-byte CARDDir header + 0x72000 payload). The server never
 * interprets game state beyond the fixed offsets in net/protocol.h, which is
 * enough to splice each resident's own blocks and fix the save checksum.
 */
#ifndef ACNET_TOWN_H
#define ACNET_TOWN_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

#define TOWN_DIR_MAX 512

typedef struct {
    char invite[ACNET_INVITE_LEN + 1];
    char dir[TOWN_DIR_MAX];
    uint8_t* data;               /* ACNET_TOWN_SIZE bytes, or NULL when no town yet */
    uint32_t version;            /* bumped on every accepted upload, persisted */
    char slot_owner[ACNET_MAX_PLAYERS][ACNET_NAME_LEN + 1]; /* "" = unassigned */
    uint8_t slot_uploaded[ACNET_MAX_PLAYERS];               /* owner uploaded at least once */
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
 * is ACNET_SLOT_ANY). Returns the slot or -1 when the town is full. */
int town_assign_slot(town_t* t, const char* name, int want_slot);

/* Sanity check on an uploaded blob: exact size and GCI magic. 1 = ok. */
int town_validate_blob(const uint8_t* blob, size_t len);

/* Accept an upload from the resident in uploader_slot. Blocks owned by other
 * residents who have already uploaded are copied back in from the current
 * canonical town, the save checksum is recomputed, the backup copy is
 * mirrored, and the result is written atomically. Returns the new version,
 * or 0 on failure. */
uint32_t town_apply_upload(town_t* t, int uploader_slot, const uint8_t* blob);

/* Big-endian 16-bit two's complement checksum as used by the game's save
 * check. Computed over the whole Save_t with the checksum field zeroed. */
uint16_t town_checksum_be(const uint8_t* data, size_t size);

/* Recompute and store the checksum of the main Save_t inside a payload and
 * mirror main -> backup. Exposed for tools and tests. */
void town_fix_payload(uint8_t* payload);

#endif /* ACNET_TOWN_H */
