#ifndef M_PERSONAL_ID_H
#define M_PERSONAL_ID_H

#include "types.h"

#include "m_land_h.h"
#include "m_actor_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Multiplayer fork: eight residents in one town (the original had four).
 * Eight is the ceiling: several save fields keep one bit per player in a
 * byte. Everything per-resident is sized from this. */
#define PLAYER_NUM 8
#define FOREIGNER_NUM 1
#define TOTAL_PLAYER_NUM (PLAYER_NUM + FOREIGNER_NUM)

#define PLAYER_NAME_LEN 8

/* sizeof(PersonalID_c) == 0x14 */
typedef struct personal_id_s {
  /* 0x00 */ u8 player_name[PLAYER_NAME_LEN];
  /* 0x08 */ u8 land_name[LAND_NAME_SIZE];
  /* 0x10 */ u16 player_id;
  /* 0x12 */ u16 land_id;
} PersonalID_c;

#ifdef __cplusplus
}
#endif

#endif
