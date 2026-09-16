#include "m_house.h"

#include "m_common_data.h"
#include "m_home.h"
#include "m_private.h"
#include "m_field_info.h"
#include "m_field_make.h"

/* Multiplayer fork: the second house acre is whichever other acre the town
 * has of the player-house type (the first is fixed at block (3,2)). */
extern int mHS_house_origin(int house_no, f32* ox, f32* oz) {
    int bx = mHS_FIRST_HOUSE_ACRE_BX, bz = mHS_FIRST_HOUSE_ACRE_BZ;
    int ok = mHS_house_acre_block(mHS_HOUSE_ACRE(house_no & mHS_HOUSE_MASK), &bx, &bz);

    if (!ok) {
        bx = mHS_FIRST_HOUSE_ACRE_BX;
        bz = mHS_FIRST_HOUSE_ACRE_BZ;
    }
    *ox = (f32)bx * (f32)(UT_X_NUM * mFI_UNIT_BASE_SIZE);
    *oz = (f32)bz * (f32)(UT_Z_NUM * mFI_UNIT_BASE_SIZE);
    return ok;
}

extern int mHS_acre_has_free_house(int acre) {
    int i;

    for (i = 0; i < mHS_HOUSES_PER_ACRE; i++) {
        int house = acre * mHS_HOUSES_PER_ACRE + i;
        if (house < mHS_HOUSE_NUM && mPr_NullCheckPersonalID(&Save_Get(homes[house]).ownerID) == TRUE) {
            return TRUE;
        }
    }
    return FALSE;
}

extern int mHS_house_acre_of_block(int bx, int bz) {
    int abx, abz;

    if (bx == mHS_FIRST_HOUSE_ACRE_BX && bz == mHS_FIRST_HOUSE_ACRE_BZ) {
        return 0;
    }
    if (mHS_house_acre_block(1, &abx, &abz) && abx == bx && abz == bz) {
        return 1;
    }
    return -1;
}

extern int mHS_house_acre_block(int acre, int* bx_out, int* bz_out) {
    int bx, bz;

    if (acre == 0) {
        *bx_out = mHS_FIRST_HOUSE_ACRE_BX;
        *bz_out = mHS_FIRST_HOUSE_ACRE_BZ;
        return TRUE;
    }
    if (acre != 1) {
        return FALSE;
    }
    for (bz = 1; bz <= FG_BLOCK_Z_NUM; bz++) {
        for (bx = 1; bx <= FG_BLOCK_X_NUM; bx++) {
            if (bx == mHS_FIRST_HOUSE_ACRE_BX && bz == mHS_FIRST_HOUSE_ACRE_BZ) {
                continue;
            }
            if (mFI_BkNum2BlockType(bx, bz) == mFM_BLOCK_TYPE_PLAYER_HOUSE) {
                *bx_out = bx;
                *bz_out = bz;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* Multiplayer fork: one byte per player holding the index of the house
 * they own, a permutation of 0..PLAYER_NUM-1 (the original packed two bits
 * per player into a byte, which only reaches four). */

/**
 * @brief Initializes the `house_arrangement` array to its default state:
 * player i owns house i.
 **/
extern void mHS_house_init() {
    int i;

    for (i = 0; i < PLAYER_NUM; i++) {
        Save_Set(house_arrangement[i], (u8)i);
    }
}

/**
 * @brief Retrieves the house index for a given player index.
 **/
extern int mHS_get_arrange_idx(int player_no) {
    if (player_no < 0 || player_no >= PLAYER_NUM) {
        return 0;
    }

    return Save_Get(house_arrangement[player_no]) & mHS_HOUSE_MASK;
}

/**
 * @brief Returns the index of the player who owns the house index from `house_arrangement`.
 *
 * @return The owning player index for the house, PLAYER_NUM when nobody does.
 **/
extern int mHS_get_pl_no(int house_no) {
    int i;

    for (i = 0; i < PLAYER_NUM; i++) {
        if ((Save_Get(house_arrangement[i]) & mHS_HOUSE_MASK) == house_no) {
            break;
        }
    }

    return i;
}

/**
 * @brief Returns the index of the player who owns a house by checking house owner IDs.
 *
 * @return The owning player index for the house.
 **/
extern int mHS_get_pl_no_detail(int home_no) {
    mHm_hs_c* home = Save_Get(homes + (home_no & mHS_HOUSE_MASK));

    if (mPr_NullCheckPersonalID(&home->ownerID) == FALSE) {
        int i;
        for (i = 0; i < PLAYER_NUM; i++) {
            if (mPr_CheckCmpPersonalID(&home->ownerID, Save_GetPointer(private_data[i].player_ID))) {
                return i;
            }
        }
    }

    return -1;
}

/* TODO: this probably had some macro.. maybe refactor? */
/**
 * @brief Sets the house arrangement value for `player_no` to `house_no`.
 *
 * This function also swaps houses with whichever player is set in house_arrangement
 * with the house currently assigned to `player_no`.
 *
 * @return TRUE when successfully set, FALSE otherwise.
 **/
extern int mHS_set_use(int player_no, int house_no) {
    int now_player_no;
    int res = FALSE;

    if (player_no >= 0 && player_no < PLAYER_NUM && house_no >= 0 && house_no < mHS_HOUSE_NUM) {
        now_player_no = mHS_get_pl_no(house_no);
        if (mPr_NullCheckPersonalID(Save_GetPointer(homes[house_no].ownerID)) == TRUE) {
            if (now_player_no != player_no) {
                /* Swap: whoever held our chosen house takes the house we held. */
                u8 old_house = Save_Get(house_arrangement[player_no]);

                if (now_player_no < PLAYER_NUM) {
                    Save_Set(house_arrangement[now_player_no], old_house);
                }
                Save_Set(house_arrangement[player_no], (u8)house_no);
            }

            mHm_InitHomeInfo(Save_Get(homes + house_no), player_no);
            res = TRUE;
        }
    }

    return res;
}
