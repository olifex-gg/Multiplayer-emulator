/* pc_save_convert.h - loading town files from the four-resident builds
 * (multiplayer fork). Since the town grew to eight residents the save
 * layout changed; a file written by the old builds is converted on the way
 * in, once, and written back in the new layout on the next save. */
#ifndef PC_SAVE_CONVERT_H
#define PC_SAVE_CONVERT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The old town file: a 64-byte CARDDir header and 0x72000 bytes of payload
 * (others block 0x26000, then two 0x26000 copies of the save). */
#define PC_SAVE_LEGACY_GCI_PAYLOAD_SIZE 0x72000
#define PC_SAVE_LEGACY_GCI_FILE_SIZE (64 + PC_SAVE_LEGACY_GCI_PAYLOAD_SIZE)

/* Convert a legacy payload (big-endian bytes as on disk) into an
 * eight-resident payload of mCD_LAND_SAVE_SIZE bytes. `out` is caller
 * allocated and is zeroed here. `hdr` is the 64-byte CARDDir header, whose
 * block count is rewritten in place. Returns TRUE on success; on failure
 * `out` is unusable and the reason is in the log. */
int pc_save_convert_legacy_payload(const u8* in, u8* out, u8* hdr);

/* After a converted town has been byte-swapped into common_data: put the
 * four new resident slots, their houses and the per-player tables into the
 * state a fresh town gives them (the byte converter can only zero them). */
void pc_save_convert_fixup_loaded(void);

/* The part of that which needs the ROM (the new houses' gyroid messages),
 * run on the first play frame after a conversion; a no-op otherwise. */
void pc_save_convert_late_fixup(void);

/* Right before the outdoor field is first built: a town without a second
 * house acre gets one (it needs the disc's field data, so not at load time,
 * and it must come before the field so a neighbouring acre is safe). */
void pc_save_convert_pre_field(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_SAVE_CONVERT_H */
