/* protocol.h - wire protocol shared by the game client (pc/src/pc_net.c) and
 * the town server (server/). This header is the single source of truth for
 * message ids, sizes, and the save-layout offsets the server relies on.
 *
 * Conventions
 *  - Transport is ENet. Channel 0 carries reliable control traffic, channel 1
 *    carries unreliable sequenced player state, channel 2 carries chat.
 *  - Every packet starts with acnet_hdr_t. All multi-byte fields are little
 *    endian; both endpoints are x86, and structs are packed so that the
 *    32-bit MinGW client and the 64-bit Linux server agree on layout.
 *  - The town blob is a Dolphin-compatible GCI file, exactly as the game
 *    writes it: a 64-byte CARDDir header followed by a 0x72000-byte payload.
 *    Its interior is big-endian GameCube data. The server treats it as opaque
 *    except for the fixed offsets below.
 */
#ifndef ACNET_PROTOCOL_H
#define ACNET_PROTOCOL_H

#include <stdint.h>

#define ACNET_PROTOCOL_VERSION 1
#define ACNET_DEFAULT_PORT     7777

#define ACNET_MAX_PLAYERS 4     /* PLAYER_NUM: resident slots in one town */
#define ACNET_MAX_CLIENTS 8     /* connections, including ones still logging in */
#define ACNET_NAME_LEN    16    /* resident login name, NUL padded */
#define ACNET_INVITE_LEN  32    /* invite code, NUL padded */
#define ACNET_CHAT_LEN    96
#define ACNET_SLOT_ANY    0xFF

/* ENet channels */
#define ACNET_CH_CONTROL 0
#define ACNET_CH_STATE   1
#define ACNET_CH_CHAT    2
#define ACNET_CHANNELS   3

/* --- Town blob layout (from the port's pc_m_card.c and the decomp headers) --- */
#define ACNET_GCI_HEADER_SIZE      64            /* sizeof(CARDDir) */
#define ACNET_GCI_PAYLOAD_SIZE     0x72000       /* mCD_LAND_SAVE_SIZE */
#define ACNET_TOWN_SIZE            (ACNET_GCI_HEADER_SIZE + ACNET_GCI_PAYLOAD_SIZE)
#define ACNET_SAVE_MAIN_OFFSET     0x26000       /* Save_t main copy, payload relative */
#define ACNET_SAVE_BACK_OFFSET     0x4C000       /* Save_t backup copy, payload relative */
#define ACNET_SAVE_ALIGNED_SIZE    0x26000       /* sizeof(Save): sector aligned union */
#define ACNET_SAVE_T_SIZE          0x242A0       /* sizeof(Save_t): checksummed bytes */
#define ACNET_SAVE_CHECKSUM_OFFSET 0x12          /* mFRm_chk_t.checksum inside Save_t */
#define ACNET_PRIVATE_ARRAY_OFFSET 0x20          /* Save_t.private_data[PLAYER_NUM] */
#define ACNET_PRIVATE_SIZE         0x2440        /* sizeof(Private_c) */
#define ACNET_HOME_ARRAY_OFFSET    0x9CE8        /* Save_t.homes[PLAYER_NUM] */
#define ACNET_HOME_SIZE            0x26B0        /* sizeof(mHm_hs_c) */

/* Absolute offsets inside the town blob for resident slot i. */
#define ACNET_SAVE_MAIN_ABS   (ACNET_GCI_HEADER_SIZE + ACNET_SAVE_MAIN_OFFSET)
#define ACNET_PRIVATE_OFFSET(i) (ACNET_SAVE_MAIN_ABS + ACNET_PRIVATE_ARRAY_OFFSET + (i) * ACNET_PRIVATE_SIZE)
#define ACNET_HOME_OFFSET(i)    (ACNET_SAVE_MAIN_ABS + ACNET_HOME_ARRAY_OFFSET + (i) * ACNET_HOME_SIZE)

/* --- Message ids --- */
enum acnet_msg {
    ACNET_MSG_HELLO         = 1,  /* C->S  acnet_hello_t */
    ACNET_MSG_WELCOME       = 2,  /* S->C  acnet_welcome_t */
    ACNET_MSG_REJECT        = 3,  /* S->C  acnet_reject_t */
    ACNET_MSG_TOWN_REQUEST  = 4,  /* C->S  (no payload) */
    ACNET_MSG_TOWN_DATA     = 5,  /* S->C  acnet_town_data_t + ACNET_TOWN_SIZE bytes when present */
    ACNET_MSG_TOWN_UPLOAD   = 6,  /* C->S  acnet_town_upload_t + ACNET_TOWN_SIZE bytes */
    ACNET_MSG_TOWN_ACK      = 7,  /* S->C  acnet_town_ack_t */
    ACNET_MSG_PEER_JOINED   = 8,  /* S->C  acnet_peer_t */
    ACNET_MSG_PEER_LEFT     = 9,  /* S->C  acnet_peer_t */
    ACNET_MSG_AUTHORITY     = 10, /* S->C  acnet_authority_t */
    ACNET_MSG_PING          = 11, /* C->S  acnet_ping_t */
    ACNET_MSG_PONG          = 12, /* S->C  acnet_pong_t */
    ACNET_MSG_CHAT          = 13, /* C->S->C acnet_chat_t */
    ACNET_MSG_PLAYER_STATE  = 14, /* C->S->C acnet_player_state_t (channel 1) */
    ACNET_MSG_TOWN_VERSION  = 15, /* S->C  acnet_town_ack_t: someone else's upload landed */
    ACNET_MSG_STATUS_REQUEST = 16,/* C->S  acnet_status_request_t (pre-login, claims nothing) */
    ACNET_MSG_STATUS_REPLY   = 17,/* S->C  acnet_status_reply_t */
    ACNET_MSG_RESIDENT_DATA  = 18 /* S->C  acnet_resident_data_t + ACNET_RESIDENT_BLOB_SIZE bytes */
};

enum acnet_reject_reason {
    ACNET_REJECT_BAD_VERSION = 1,
    ACNET_REJECT_BAD_INVITE  = 2,
    ACNET_REJECT_TOWN_FULL   = 3,
    ACNET_REJECT_NAME_IN_USE = 4, /* same resident already connected */
    ACNET_REJECT_SLOT_TAKEN  = 5,
    ACNET_REJECT_BUSY        = 6,
    ACNET_REJECT_BAD_NAME    = 7
};

enum acnet_upload_reason {
    ACNET_UPLOAD_SAVE     = 1, /* in-game save */
    ACNET_UPLOAD_LEAVE    = 2, /* client is disconnecting */
    ACNET_UPLOAD_PERIODIC = 3, /* world authority checkpoint */
    ACNET_UPLOAD_NEW_TOWN = 4  /* founding upload into an empty town */
};

enum acnet_ack_status {
    ACNET_ACK_OK          = 0,
    ACNET_ACK_REJECTED    = 1, /* malformed blob */
    ACNET_ACK_RATE_LIMIT  = 2,
    ACNET_ACK_NOT_LOGGED_IN = 3,
    ACNET_ACK_IO_ERROR    = 4
};

#if defined(__GNUC__)
#define ACNET_PACKED __attribute__((packed))
#else
#error "acnet packets require a packed-struct attribute"
#endif

typedef struct ACNET_PACKED {
    uint8_t  type;        /* enum acnet_msg */
    uint8_t  version;     /* ACNET_PROTOCOL_VERSION */
    uint16_t payload_len; /* bytes following this header, excluding trailing blob */
} acnet_hdr_t;

typedef struct ACNET_PACKED {
    char     invite[ACNET_INVITE_LEN];
    char     name[ACNET_NAME_LEN];
    uint32_t client_build;
    uint8_t  want_slot;   /* ACNET_SLOT_ANY or 0..3 */
    uint8_t  reserved[3];
} acnet_hello_t;

typedef struct ACNET_PACKED {
    uint8_t client_id;
    uint8_t slot;
    char    name[ACNET_NAME_LEN];
} acnet_peer_t;

typedef struct ACNET_PACKED {
    uint8_t  client_id;
    uint8_t  slot;
    uint8_t  authority_client_id;
    uint8_t  town_present;
    uint32_t town_version;
    int64_t  server_unix_ms;
    uint8_t  peer_count;
    uint8_t  reserved[3];
    acnet_peer_t peers[ACNET_MAX_PLAYERS];
} acnet_welcome_t;

typedef struct ACNET_PACKED {
    uint8_t reason;       /* enum acnet_reject_reason */
    char    text[63];
} acnet_reject_t;

typedef struct ACNET_PACKED {
    uint32_t town_version;
    uint8_t  present;     /* 1: ACNET_TOWN_SIZE bytes follow */
    uint8_t  reserved[3];
} acnet_town_data_t;

typedef struct ACNET_PACKED {
    uint8_t reason;       /* enum acnet_upload_reason */
    uint8_t reserved[3];
} acnet_town_upload_t;

typedef struct ACNET_PACKED {
    uint32_t town_version;
    uint8_t  status;      /* enum acnet_ack_status */
    uint8_t  by_slot;     /* who uploaded (TOWN_VERSION) */
    uint8_t  reserved[2];
} acnet_town_ack_t;

typedef struct ACNET_PACKED {
    uint8_t client_id;
} acnet_authority_t;

typedef struct ACNET_PACKED {
    uint32_t nonce;
} acnet_ping_t;

typedef struct ACNET_PACKED {
    uint32_t nonce;
    int64_t  server_unix_ms;
} acnet_pong_t;

typedef struct ACNET_PACKED {
    uint8_t slot;
    uint8_t len;
    char    text[ACNET_CHAT_LEN];
} acnet_chat_t;

/* Lobby status (launcher). A status request is answered without logging in,
 * so the launcher can show who is in a town without claiming a resident slot
 * or disturbing the players already connected. It never creates a town. */
typedef struct ACNET_PACKED {
    char invite[ACNET_INVITE_LEN];
} acnet_status_request_t;

typedef struct ACNET_PACKED {
    char    name[ACNET_NAME_LEN]; /* "" = slot free */
    uint8_t online;               /* 1 = that resident is connected right now */
    uint8_t reserved[3];
} acnet_slot_info_t;

typedef struct ACNET_PACKED {
    uint8_t  room_known;   /* 0 = no such town on this server (yet) */
    uint8_t  town_present; /* 1 = a save has been uploaded; 0 = nobody has saved yet */
    uint8_t  reserved[2];
    uint32_t town_version;
    int64_t  server_unix_ms;
    acnet_slot_info_t slots[ACNET_MAX_PLAYERS];
} acnet_status_reply_t;

/* Live resident sync. When a resident's upload lands, the server sends that
 * resident's own two blocks (Private_c then mHm_hs_c, big-endian exactly as
 * they sit in the town blob) to everyone else in the town, so their running
 * games can take the new character and house without a reload. */
#define ACNET_RESIDENT_BLOB_SIZE (ACNET_PRIVATE_SIZE + ACNET_HOME_SIZE)
typedef struct ACNET_PACKED {
    uint8_t  slot;
    uint8_t  reserved[3];
    uint32_t town_version;
} acnet_resident_data_t;

/* Puppet stream (step 2). Sent by each client at 20-30 Hz, relayed to the
 * others. area identifies the field or room so puppets are only drawn when
 * two players share it; the exact encoding is settled in step 2. */
typedef struct ACNET_PACKED {
    uint8_t  client_id;
    uint8_t  slot;
    uint16_t seq;
    uint32_t area;
    float    x, y, z;
    int16_t  angle_y;
    uint16_t anim_index;
    float    anim_frame;
    uint16_t item;
    uint8_t  emote;
    uint8_t  flags;
} acnet_player_state_t;

#define ACNET_MAX_PAYLOAD (sizeof(acnet_hdr_t) + sizeof(acnet_welcome_t))

#endif /* ACNET_PROTOCOL_H */
