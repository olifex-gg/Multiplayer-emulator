#ifndef M_HOUSE_H
#define M_HOUSE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * House arrangement: Save_t.house_arrangement[player_no] holds the index of
 * the house that player owns (multiplayer fork; the original packed two
 * bits per player into one byte, which only reaches four). Houses 0-3 are
 * the first house acre, 4-7 the second.
 **/

enum {
  mHS_HOUSE0, /* Top-left house, first house acre */
  mHS_HOUSE1, /* Top-right house */
  mHS_HOUSE2, /* Bottom-left house */
  mHS_HOUSE3, /* Bottom-right house */
  mHS_HOUSE4, /* Top-left house, second house acre */
  mHS_HOUSE5,
  mHS_HOUSE6,
  mHS_HOUSE7,

  mHS_HOUSE_NUM
};

#define mHS_HOUSE_MASK (mHS_HOUSE_NUM - 1) /* a house index is always 0..7 */
#define mHS_HOUSES_PER_ACRE 4
#define mHS_HOUSE_ACRE(house_no) ((house_no) / mHS_HOUSES_PER_ACRE) /* 0: the first house acre, 1: the second */

extern void mHS_house_init();
extern int mHS_get_arrange_idx(int player_no);
extern int mHS_get_pl_no(int house_no);
extern int mHS_get_pl_no_detail(int home_no);
extern int mHS_set_use(int player_no, int house_no);
/* The acre (block coordinates) holding houses acre*4 .. acre*4+3: the first
 * house acre is always block (3,2); the second is the other acre of type
 * mFM_BLOCK_TYPE_PLAYER_HOUSE, or absent (returns FALSE). */
extern int mHS_house_acre_block(int acre, int* bx_out, int* bz_out);
#define mHS_FIRST_HOUSE_ACRE_BX 3
#define mHS_FIRST_HOUSE_ACRE_BZ 2
/* The world origin (north-west corner) of the acre house_no stands in; the
 * house tables in the actors are offsets from it. FALSE when that acre does
 * not exist (then the first acre's origin is returned). */
extern int mHS_house_origin(int house_no, f32* ox, f32* oz);
/* Which house acre block (bx, bz) is: 0, 1, or -1 for neither. */
extern int mHS_house_acre_of_block(int bx, int bz);

#ifdef __cplusplus
}
#endif

#endif
