/* pc_save_convert.c - four-resident town files into the eight-resident layout
 * (multiplayer fork). See pc_save_convert.h.
 *
 * The old layout comes from pc_save_legacy_layout.h, generated from the
 * headers as they were at PLAYER_NUM 4; the new one is offsetof/sizeof on the
 * live structs. Every Save_t field is copied by name, so a field this file
 * does not know about is a compile error, not a silent gap. Fields whose
 * meaning changed with eight residents are converted by hand:
 *   private_data, homes, mother_mail, keep_house_size   four more entries
 *   house_arrangement      two bits per player in a byte -> a byte per player
 *   event_save_data.flags  32 bits, four per group -> 64 bits, eight per group
 *   animals[].hp_mail      four per villager -> eight
 *   island.animal          the same villager struct
 *   allgrow_ss_pos_info    five positions -> nine
 *   LightHouse             two nibbles of players -> two bytes
 *   museum_display         donor 5 ("a resident who left") -> 9
 *   fg                     the blank signboard id moved (0x0920 -> 0x0940)
 * The others block (mail, designs, diary) keeps its order and offsets; only
 * the diary grows, from four residents' pages to eight.
 */
#ifdef TARGET_PC

#include "pc_save_convert.h"
#include "pc_save_legacy_layout.h"
#include "pc_save_bswap.h"
#include "m_common_data.h"
#include "m_card.h"
#include "m_event.h"
#include "m_island.h"
#include "m_all_grow_ovl.h"
#include "m_museum_display.h"
#include "m_name_table.h"
#include "m_house.h"
#include "m_home.h"
#include "m_private.h"
#include "m_npc.h"
#include "m_cockroach.h"
#include "m_field_make.h"
#include "m_scene_table.h"
#include "dolphin/os.h"
#include <string.h>
#include <stddef.h>

#define CARDDIR_LENGTH_OFFSET 56 /* u16 block count inside the 64-byte CARDDir */

static u16 get_be16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 get_be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void put_be16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put_be64(u8* p, u64 v) {
    int i;
    for (i = 7; i >= 0; i--) { p[i] = (u8)v; v >>= 8; }
}

static int s_conv_ok;

/* A field that is the same size and meaning in both layouts. */
static void copy_plain(u8* dst, const u8* src, size_t new_off, size_t old_off, size_t old_size, size_t new_size,
                       const char* name) {
    if (old_size != new_size) {
        OSReport("[save] convert: field %s is %u bytes now, was %u; refusing\n", name, (unsigned)new_size,
                 (unsigned)old_size);
        s_conv_ok = FALSE;
        return;
    }
    memcpy(dst + new_off, src + old_off, old_size);
}

/* One villager: everything before the per-player mail slots, four of the
 * eight slots, then the tail. */
static void convert_animal(u8* dst, const u8* src) {
    if (offsetof(Animal_c, hp_mail) != LEGACY_ANIMAL_OFF_hp_mail || sizeof(AnmHPMail_c) != LEGACY_SIZEOF_AnmHPMail_c ||
        sizeof(((Animal_c*)0)->unused) != LEGACY_ANIMAL_SIZE_unused) {
        OSReport("[save] convert: the villager struct is not what the converter expects; refusing\n");
        s_conv_ok = FALSE;
        return;
    }
    memcpy(dst, src, LEGACY_ANIMAL_OFF_hp_mail);
    memcpy(dst + offsetof(Animal_c, hp_mail), src + LEGACY_ANIMAL_OFF_hp_mail, LEGACY_ANIMAL_SIZE_hp_mail);
    memcpy(dst + offsetof(Animal_c, unused), src + LEGACY_ANIMAL_OFF_unused, LEGACY_ANIMAL_SIZE_unused);
}

/* The saved-event flags: subtype 0 and 1 stay, the six groups of four
 * per-player flags become groups of eight, the foreigner's gateway flag
 * follows them. */
static u64 remap_event_flags(u32 old) {
    u64 out = 0;
    int s;
    for (s = 0; s < 32; s++) {
        int ns;
        if ((old & ((u32)1 << s)) == 0) continue;
        if (s < 2) {
            ns = s;
        } else if (s < 2 + 6 * LEGACY_PLAYER_NUM) {
            int g = (s - 2) / LEGACY_PLAYER_NUM;
            int p = (s - 2) % LEGACY_PLAYER_NUM;
            ns = 2 + g * PLAYER_NUM + p;
        } else if (s == 2 + 6 * LEGACY_PLAYER_NUM) {
            ns = 2 + 6 * PLAYER_NUM;
        } else {
            continue;
        }
        out |= (u64)1 << ns;
    }
    return out;
}

static int convert_save(u8* dst, const u8* src) {
    size_t i;

    s_conv_ok = TRUE;
    memset(dst, 0, sizeof(Save));

#define X(f) copy_plain(dst, src, offsetof(Save_t, f), LEGACY_OFF_##f, LEGACY_SIZE_##f, sizeof(((Save_t*)0)->f), #f);
    LEGACY_SAVE_PLAIN_FIELDS(X)
#undef X

    /* Four residents, four houses, into the first four of eight. */
    if (sizeof(Private_c) != LEGACY_SIZEOF_Private_c || sizeof(mHm_hs_c) != LEGACY_SIZEOF_mHm_hs_c ||
        sizeof(mPr_mother_mail_info_c) != LEGACY_SIZEOF_mPr_mother_mail_info_c) {
        OSReport("[save] convert: a per-resident struct changed size; refusing\n");
        return FALSE;
    }
    for (i = 0; i < LEGACY_PLAYER_NUM; i++) {
        memcpy(dst + offsetof(Save_t, private_data) + i * sizeof(Private_c),
               src + LEGACY_OFF_private_data + i * sizeof(Private_c), sizeof(Private_c));
        memcpy(dst + offsetof(Save_t, homes) + i * sizeof(mHm_hs_c), src + LEGACY_OFF_homes + i * sizeof(mHm_hs_c),
               sizeof(mHm_hs_c));
        memcpy(dst + offsetof(Save_t, mother_mail) + i * sizeof(mPr_mother_mail_info_c),
               src + LEGACY_OFF_mother_mail + i * sizeof(mPr_mother_mail_info_c), sizeof(mPr_mother_mail_info_c));
        dst[offsetof(Save_t, keep_house_size) + i] = src[LEGACY_OFF_keep_house_size + i];
    }

    /* The house each player owns: two bits each in one byte, now a byte each. */
    {
        u8 old = src[LEGACY_OFF_house_arrangement];
        for (i = 0; i < PLAYER_NUM; i++) {
            dst[offsetof(Save_t, house_arrangement) + i] =
                (i < LEGACY_PLAYER_NUM) ? (u8)((old >> (2 * i)) & 3) : (u8)i;
        }
    }

    /* The field-item table: only the blank signboard's id moved. */
    {
        u8* cell = dst + offsetof(Save_t, fg);
        memcpy(cell, src + LEGACY_OFF_fg, LEGACY_SIZE_fg);
        for (i = 0; i + 1 < LEGACY_SIZE_fg; i += 2) {
            if (get_be16(cell + i) == SIGNBOARD_START + 32) {
                put_be16(cell + i, SIGNBOARD);
            }
        }
    }

    /* Villagers, and the islander. */
    if (LEGACY_SIZE_animals != ANIMAL_NUM_MAX * LEGACY_SIZEOF_Animal_c ||
        offsetof(Island_c, animal) != LEGACY_ISLAND_OFF_animal ||
        sizeof(Island_c) - offsetof(Island_c, deposit) < LEGACY_SIZE_island - LEGACY_ISLAND_OFF_deposit) {
        OSReport("[save] convert: the villager tables are not what the converter expects; refusing\n");
        return FALSE;
    }
    for (i = 0; i < ANIMAL_NUM_MAX; i++) {
        convert_animal(dst + offsetof(Save_t, animals) + i * sizeof(Animal_c),
                       src + LEGACY_OFF_animals + i * LEGACY_SIZEOF_Animal_c);
    }
    memcpy(dst + offsetof(Save_t, island), src + LEGACY_OFF_island, LEGACY_ISLAND_OFF_animal);
    convert_animal(dst + offsetof(Save_t, island) + offsetof(Island_c, animal),
                   src + LEGACY_OFF_island + LEGACY_ISLAND_OFF_animal);
    memcpy(dst + offsetof(Save_t, island) + offsetof(Island_c, deposit), src + LEGACY_OFF_island + LEGACY_ISLAND_OFF_deposit,
           LEGACY_SIZE_island - LEGACY_ISLAND_OFF_deposit);

    /* Saved events: the special event and the weekly union copy (the union
     * grew for nine spoken-to ids; the rest stays zero), the flags spread. */
    if (LEGACY_EVSAVE_SIZE_special != sizeof(mEv_special_c) || LEGACY_EVSAVE_SIZE_weekly > sizeof(mEv_weekly_u) ||
        LEGACY_EVSAVE_SIZE_flags != 4) {
        OSReport("[save] convert: the event save struct is not what the converter expects; refusing\n");
        return FALSE;
    }
    memcpy(dst + offsetof(Save_t, event_save_data) + offsetof(mEv_event_save_c, special),
           src + LEGACY_OFF_event_save_data + LEGACY_EVSAVE_OFF_special, LEGACY_EVSAVE_SIZE_special);
    memcpy(dst + offsetof(Save_t, event_save_data) + offsetof(mEv_event_save_c, weekly),
           src + LEGACY_OFF_event_save_data + LEGACY_EVSAVE_OFF_weekly, LEGACY_EVSAVE_SIZE_weekly);
    put_be64(dst + offsetof(Save_t, event_save_data) + offsetof(mEv_event_save_c, flags),
             remap_event_flags(get_be32(src + LEGACY_OFF_event_save_data + LEGACY_EVSAVE_OFF_flags)));

    /* The shine and stone positions: five entries each become nine. */
    memcpy(dst + offsetof(Save_t, allgrow_ss_pos_info) + offsetof(mAGrw_AllGrow_c, shine_pos),
           src + LEGACY_OFF_allgrow_ss_pos_info + LEGACY_ALLGROW_OFF_shine_pos, LEGACY_ALLGROW_SIZE_shine_pos);
    memcpy(dst + offsetof(Save_t, allgrow_ss_pos_info) + offsetof(mAGrw_AllGrow_c, stone_pos),
           src + LEGACY_OFF_allgrow_ss_pos_info + LEGACY_ALLGROW_OFF_stone_pos, LEGACY_ALLGROW_SIZE_stone_pos);

    /* The lighthouse quest: started players in the low nibble and
     * contributors in the high nibble, now a byte each. */
    {
        const u8* o = src + LEGACY_OFF_LightHouse;
        u8* n = dst + offsetof(Save_t, LightHouse);
        memcpy(n + offsetof(LightHouse_c, renew_time), o + LEGACY_LIGHTHOUSE_OFF_renew_time, LEGACY_LIGHTHOUSE_SIZE_renew_time);
        n[offsetof(LightHouse_c, days_switched_on)] = o[LEGACY_LIGHTHOUSE_OFF_days_switched_on];
        n[offsetof(LightHouse_c, players_quest_started)] = (u8)(o[LEGACY_LIGHTHOUSE_OFF_players_quest_started] & 0x0F);
        n[offsetof(LightHouse_c, players_contributed)] = (u8)(o[LEGACY_LIGHTHOUSE_OFF_players_quest_started] >> 4);
        n[offsetof(LightHouse_c, players_completed)] = o[LEGACY_LIGHTHOUSE_OFF_players_completed];
    }

    /* Museum donors: four bits per item; "a resident who has left" was 5
     * after four residents and is 9 after eight. */
    {
        u8* n = dst + offsetof(Save_t, museum_display);
        memcpy(n, src + LEGACY_OFF_museum_display, LEGACY_SIZE_museum_display);
        for (i = 0; i < LEGACY_SIZE_museum_display; i++) {
            u8 lo = n[i] & 0x0F, hi = n[i] >> 4;
            if (lo == LEGACY_PLAYER_NUM + 1) lo = mMmd_DONATOR_DELETED_PLAYER;
            if (hi == LEGACY_PLAYER_NUM + 1) hi = mMmd_DONATOR_DELETED_PLAYER;
            n[i] = (u8)((hi << 4) | lo);
        }
    }

    /* _241A8: padding, stays zero. */

    if (!s_conv_ok) return FALSE;

    /* The checksum, over the checksummed part of the save as the game writes it. */
    put_be16(dst + offsetof(Save_t, save_check.checksum), 0);
    put_be16(dst + offsetof(Save_t, save_check.checksum), pc_checksum_be(dst, sizeof(Save_t), 0));
    return TRUE;
}

extern int pc_save_convert_legacy_payload(const u8* in, u8* out, u8* hdr) {
    const size_t block_start = ALIGN_NEXT(sizeof(MemcardHeader_c) + 32, 32);
    const size_t mail_size = ALIGN_NEXT(sizeof(mCD_keep_mail_c), 32);
    const size_t orig_size = ALIGN_NEXT(sizeof(mCD_keep_original_c), 32);
    const size_t diary_old = ALIGN_NEXT(LEGACY_SIZEOF_mCD_keep_diary_c, 32);
    const size_t diary_new = ALIGN_NEXT(sizeof(mCD_keep_diary_c), 32);
    const u8* old_save = in + LEGACY_OTHERS_SIZE;
    size_t off_mail, off_orig, off_diary;
    u16 first_landid, save_land_id;
    int gc_order;

    if (sizeof(mCD_keep_mail_c) != LEGACY_SIZEOF_mCD_keep_mail_c ||
        sizeof(mCD_keep_original_c) != LEGACY_SIZEOF_mCD_keep_original_c ||
        block_start + orig_size + mail_size + diary_new > OTHERS_SIZE) {
        OSReport("[save] convert: the others block is not what the converter expects; refusing\n");
        return FALSE;
    }

    memset(out, 0, mCD_LAND_SAVE_SIZE);

    /* Which order the three blocks are in (same detection as the reader). */
    save_land_id = get_be16(old_save + LEGACY_OFF_land_info + offsetof(mLd_land_info_c, id));
    first_landid = get_be16(in + block_start + 2);
    gc_order = (first_landid == save_land_id && save_land_id != 0);
    if (gc_order) {
        off_mail = block_start;
        off_orig = ALIGN_NEXT(off_mail + mail_size, 32);
        off_diary = ALIGN_NEXT(off_orig + orig_size, 32);
    } else {
        off_orig = block_start;
        off_mail = ALIGN_NEXT(off_orig + orig_size, 32);
        off_diary = ALIGN_NEXT(off_mail + mail_size, 32);
    }
    if (off_diary + diary_old > LEGACY_OTHERS_SIZE || off_diary + diary_new > OTHERS_SIZE) {
        OSReport("[save] convert: the diary block does not fit; refusing\n");
        return FALSE;
    }

    /* Comment, banner, icon, then mail and designs unchanged. */
    memcpy(out, in, block_start);
    memcpy(out + off_mail, in + off_mail, mail_size);
    memcpy(out + off_orig, in + off_orig, orig_size);
    /* The diary: the four residents' pages first, four blank ones after. */
    {
        u8* blk = out + off_diary;
        memcpy(blk + 2, in + off_diary + 2, LEGACY_SIZEOF_mCD_keep_diary_c - 2);
        blk[0] = 0;
        blk[1] = 0;
        put_be16(blk, pc_checksum_be(blk, (u32)diary_new, 0));
    }

    /* The save, then its backup copy. */
    if (!convert_save(out + OTHERS_SIZE, old_save)) {
        return FALSE;
    }
    memcpy(out + OTHERS_SIZE + sizeof(Save), out + OTHERS_SIZE, sizeof(Save));

    /* The header: the file is more blocks now. */
    put_be16(hdr + CARDDIR_LENGTH_OFFSET, (u16)(mCD_LAND_SAVE_SIZE / mCD_MEMCARD_SECTORSIZE));

    OSReport("[save] converted a four-resident town file (%s block order) to the eight-resident layout\n",
             gc_order ? "GC" : "PC");
    return TRUE;
}

static int s_late_fixup_pending;

extern void pc_save_convert_fixup_loaded(void) {
    int i;

    for (i = LEGACY_PLAYER_NUM; i < PLAYER_NUM; i++) {
        mPr_ClearPrivateInfo(Save_GetPointer(private_data[i]));
        mHm_ClearHomeInfoNoRom(i); /* the ROM loader's heaps do not exist yet; see the late fixup */
        mPr_ClearMotherMailInfo(Save_GetPointer(mother_mail[i]));
        Save_Set(keep_house_size[i], 0);
        Save_Set(house_arrangement[i], (u8)i);
        mNpc_AllClearHPMailPlayerIdx(i);
    }
    /* Nine "spoken to" ids where there were five: the new ones are nobody. */
    mPr_ClearAnyPersonalID(&Save_Get(event_save_data).weekly.kabu_peddler.spoken_pids[LEGACY_PLAYER_NUM + 1],
                           TOTAL_PLAYER_NUM - (LEGACY_PLAYER_NUM + 1));
    /* K.K. Slider's weekly flags changed shape; he starts the week afresh. */
    mEv_clear_save_area(mEv_EVENT_KK_SLIDER, 0xa);
    s_late_fixup_pending = TRUE;
    OSReport("[save] eight-resident town: slots %d-%d, houses %d-%d cleared for new residents\n", LEGACY_PLAYER_NUM,
             PLAYER_NUM - 1, LEGACY_PLAYER_NUM, mHS_HOUSE_NUM - 1);
}

extern void pc_save_convert_late_fixup(void) {
    int i;

    if (s_late_fixup_pending) {
        s_late_fixup_pending = FALSE;
        for (i = LEGACY_PLAYER_NUM; i < mHS_HOUSE_NUM; i++) {
            mHm_LoadHaniwaDefaultMessage(i);
        }
        OSReport("[save] eight-resident town: the new houses' gyroid messages loaded\n");
    }
}

extern void pc_save_convert_pre_field(void) {
    static int s_acre_checked;

    /* Any town without a second house acre gets one (a converted town, or
     * one saved by the first eight-resident build); a no-op otherwise. Done
     * before the field is built so the acre may be the start acre's
     * neighbour. Once per session, on the first outdoor field. */
    if (!s_acre_checked && Save_Get(scene_no) == SCENE_FG) {
        s_acre_checked = TRUE;
        mFM_MakeSecondHouseAcre();
    }
}

#endif /* TARGET_PC */
