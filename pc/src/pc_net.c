/* pc_net.c - multiplayer client. See pc_net.h for the step-1 scope.
 *
 * The design deliberately reuses the port's existing, tested GCI save code
 * instead of touching it: on login the downloaded town is written to
 * save/card_a/ so pc_save_check_and_load() loads it exactly as a local save,
 * and on save the same file is read back and uploaded. The network replaces
 * the memory card, nothing more, for step 1.
 */
#include "pc_net.h"

#include <enet/enet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h> /* _mkdir */
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "protocol.h"
#include <dolphin/os.h> /* OSReport */

/* The card-A GCI the game loads/saves (mirrors PC_GCI_PATH in pc_m_card.c). */
#define NET_CARD_A_DIR "save/card_a"
#define NET_GCI_PATH   NET_CARD_A_DIR "/DobutsunomoriP_MURA.gci"
/* One-time, never-overwritten copy of whatever town was on this PC before it
 * first joined a server. Downloading a town replaces the local save, so a
 * single-player town would otherwise be gone the first time its owner joins
 * somebody else's world. */
#define NET_GCI_PRISTINE NET_GCI_PATH ".before-online"

typedef struct {
    int    configured;         /* a server address was found in settings.ini */
    char   host[128];
    int    port;
    char   invite[ACNET_INVITE_LEN + 1];
    char   name[ACNET_NAME_LEN + 1];
    int    want_slot;          /* 0..3 or ACNET_SLOT_ANY */
    int    puppets;            /* draw other residents (settings: show_other_players) */
} net_config_t;

static net_config_t s_cfg;
static ENetHost*    s_client_host;
static ENetPeer*    s_peer;
static int          s_active;          /* logged in and town in place */
static int          s_slot = -1;
static int          s_claimed_no = -1;  /* the save block we last told the server about */
/* Weather: what the authority last sent, and what arrived for a non-authority. */
static int      s_weather_sent_type = -1, s_weather_sent_intensity = -1;
static uint8_t  s_weather_sent_grow[ACNET_GROW_TIME_SIZE];
static uint32_t s_weather_sent_ms;
static int      s_weather_in_pending, s_weather_in_type, s_weather_in_intensity;
static uint8_t  s_weather_in_grow[ACNET_GROW_TIME_SIZE];
static int          s_authority_id = -1;
static int          s_self_id = -1;
static int64_t      s_clock_skew_ms;   /* the host's wall clock minus ours, in ms (see pc_net_server_clock_skew_ms) */
static int          s_clock_skew_known;
static char         s_peer_name[ACNET_MAX_PLAYERS][ACNET_NAME_LEN + 1]; /* login names by resident slot */
static uint32_t     s_push_last_ms;    /* resident push: when we last sent our blocks */
static uint32_t     s_push_hash;       /* ...and what they hashed to */
static int          s_push_hash_known;
static unsigned     s_push_tx;
static uint32_t     s_town_version;
static int          s_town_present;     /* the server had a town when we logged in */
static unsigned     s_puppet_rx;       /* diagnostic counters */
static unsigned     s_puppet_tx;
static uint32_t     s_last_state_ms;
static unsigned     s_chat_rx;

/* --- Remote residents (step 2) -----------------------------------------
 * One entry per resident slot (0..3). The game side samples the local
 * player every frame via pc_net_send_player_state(), and reads the other
 * residents via pc_net_get_remotes() to drive puppet actors. We keep the
 * two most recent states per slot so the game can interpolate; recv_ms lets
 * it despawn a puppet that has gone silent. This layer never touches game
 * structs, so it is independent of the engine integration. */
typedef struct {
    int                  active;      /* a state has been received recently */
    int                  client_id;
    uint32_t             recv_ms;     /* enet_time of the latest state */
    acnet_player_state_t cur;         /* most recent */
    acnet_player_state_t prev;        /* one before, for interpolation */
} remote_resident_t;

static remote_resident_t s_remote[ACNET_MAX_PLAYERS];

/* Live resident sync: another resident's freshly saved blocks, held until
 * the game takes them (between its own reads of the save). */
typedef struct {
    int      pending;
    int      house;   /* Save_t.homes[] block of the house half (0xFF: use the arrangement) */
    uint32_t version;
    uint8_t  bytes[ACNET_RESIDENT_BLOB_SIZE]; /* Private_c then mHm_hs_c, big-endian */
} resident_update_t;
static resident_update_t s_resident_update[ACNET_MAX_PLAYERS];

/* Land relay: cells from other residents, queued until the game applies
 * them. A ring so a burst (someone's whole daily weed sprout) survives. */
#define LAND_IN_CAP 2048
static acnet_land_cell_t s_land_in[LAND_IN_CAP];
static unsigned s_land_in_head, s_land_in_count, s_land_dropped;
static unsigned s_land_tx, s_land_rx;

static void land_in_push(const acnet_land_cell_t* c) {
    if (s_land_in_count == LAND_IN_CAP) {
        s_land_in_head = (s_land_in_head + 1) % LAND_IN_CAP; /* drop oldest */
        s_land_in_count--;
        s_land_dropped++;
    }
    s_land_in[(s_land_in_head + s_land_in_count) % LAND_IN_CAP] = *c;
    s_land_in_count++;
}

/* Pending chat, drained by the game once per frame. A small ring, so two
 * lines arriving in the same frame both get through. */
typedef struct {
    uint8_t slot;
    uint8_t len;
    char text[ACNET_CHAT_LEN + 1];
} chat_msg_t;
#define CHAT_IN_CAP 8
static chat_msg_t s_chat_in[CHAT_IN_CAP];
static unsigned   s_chat_in_head, s_chat_in_count;

/* A remote resident is considered gone if silent this long. */
#define REMOTE_TIMEOUT_MS 3000

/* --- Wall clock ----------------------------------------------------------
 * Unix time in ms, and this PC's local-time offset, computed the same way
 * the server does it, so the two can be compared. */
static int64_t wall_unix_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static int local_tz_min(void) {
    time_t now = time(NULL);
    struct tm g = *gmtime(&now);
    time_t gt;
    g.tm_isdst = -1;
    gt = mktime(&g);
    return (int)((long)difftime(now, gt) / 60);
}

/* The server just told us its clock: remember how far the host's local
 * wall clock is from ours. The game's clock follows the host's (lb_rtc.c). */
static void clock_learn(int64_t server_unix_ms, int server_tz_min) {
    int64_t host_local = server_unix_ms + (int64_t)server_tz_min * 60000;
    int64_t our_local = wall_unix_ms() + (int64_t)local_tz_min() * 60000;
    int64_t skew = host_local - our_local;
    if (!s_clock_skew_known || skew - s_clock_skew_ms > 2000 || s_clock_skew_ms - skew > 2000) {
        OSReport("[net] town clock: the host's clock is %lld s %s ours (host UTC%+d min, we are UTC%+d min); the game follows the host\n",
                 (long long)(skew < 0 ? -skew : skew) / 1000, skew < 0 ? "behind" : "ahead of", server_tz_min,
                 local_tz_min());
    }
    s_clock_skew_ms = skew;
    s_clock_skew_known = 1;
}

static void peer_name_set(int slot, const char* name16) {
    if (slot < 0 || slot >= ACNET_MAX_PLAYERS) return;
    memcpy(s_peer_name[slot], name16, ACNET_NAME_LEN);
    s_peer_name[slot][ACNET_NAME_LEN] = '\0';
}

/* Villagers streamed by their owners, keyed by npc_id. */
#define NPC_TABLE_SIZE 64
typedef struct {
    int                active;
    uint8_t            client_id;
    uint32_t           recv_ms;
    acnet_npc_state_t  st;
} remote_npc_t;
static remote_npc_t s_npc[NPC_TABLE_SIZE];
static unsigned     s_npc_tx, s_npc_rx;

static void npc_apply_states(uint8_t client_id, const uint8_t* data, int count) {
    uint32_t now = enet_time_get();
    int i, k;
    for (k = 0; k < count; k++) {
        acnet_npc_state_t st;
        remote_npc_t* slot = NULL;
        memcpy(&st, data + (size_t)k * sizeof(st), sizeof(st));
        for (i = 0; i < NPC_TABLE_SIZE; i++) {
            if (s_npc[i].active && s_npc[i].st.npc_id == st.npc_id) { slot = &s_npc[i]; break; }
        }
        if (!slot) {
            for (i = 0; i < NPC_TABLE_SIZE; i++) {
                if (!s_npc[i].active) { slot = &s_npc[i]; break; }
            }
        }
        if (!slot) continue;
        slot->active = 1;
        slot->client_id = client_id;
        slot->recv_ms = now;
        slot->st = st;
        s_npc_rx++;
    }
}

static void remote_apply_state(const acnet_player_state_t* s) {
    remote_resident_t* r;
    if (s->slot >= ACNET_MAX_PLAYERS) return;
    if ((int)s->slot == s_slot) return; /* never puppet ourselves */
    r = &s_remote[s->slot];
    /* Drop out-of-order packets (unreliable channel can reorder). */
    if (r->active && (int16_t)(s->seq - r->cur.seq) <= 0) return;
    r->prev = r->active ? r->cur : *s;
    r->cur = *s;
    r->client_id = s->client_id;
    r->recv_ms = enet_time_get();
    r->active = 1;
}

static void remote_expire(void) {
    uint32_t now = enet_time_get();
    int i;
    for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
        if (s_remote[i].active && now - s_remote[i].recv_ms > REMOTE_TIMEOUT_MS) {
            s_remote[i].active = 0;
        }
    }
    for (i = 0; i < NPC_TABLE_SIZE; i++) {
        if (s_npc[i].active && now - s_npc[i].recv_ms > REMOTE_TIMEOUT_MS) {
            s_npc[i].active = 0;
        }
    }
}

static void remote_drop_slot(int slot) {
    if (slot >= 0 && slot < ACNET_MAX_PLAYERS) s_remote[slot].active = 0;
}

/* ------------------------------------------------------------- settings */

static char* trim(char* s) {
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

static void read_config(void) {
    FILE* f = fopen("settings.ini", "r");
    char line[256];
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.port = ACNET_DEFAULT_PORT;
    s_cfg.want_slot = ACNET_SLOT_ANY;
    s_cfg.puppets = 1;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char* p = trim(line);
        char* eq;
        char* key;
        char* val;
        if (*p == '#' || *p == ';' || *p == '[' || *p == '\0') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        key = trim(p);
        val = trim(eq + 1);
        if (strcmp(key, "server") == 0) {
            snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", val);
        } else if (strcmp(key, "server_port") == 0) {
            int v = atoi(val);
            if (v > 0 && v <= 65535) s_cfg.port = v;
        } else if (strcmp(key, "invite") == 0) {
            snprintf(s_cfg.invite, sizeof(s_cfg.invite), "%s", val);
        } else if (strcmp(key, "player_name") == 0) {
            snprintf(s_cfg.name, sizeof(s_cfg.name), "%s", val);
        } else if (strcmp(key, "show_other_players") == 0) {
            s_cfg.puppets = atoi(val) != 0;
        } else if (strcmp(key, "resident_slot") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < ACNET_MAX_PLAYERS) s_cfg.want_slot = v;
        }
    }
    fclose(f);
    s_cfg.configured = s_cfg.host[0] && s_cfg.invite[0] && s_cfg.name[0];
}

/* ------------------------------------------------------------- transport */

static int send_msg(uint8_t channel, uint8_t type, const void* payload, size_t payload_len,
                    const void* blob, size_t blob_len, int reliable) {
    ENetPacket* pkt;
    acnet_hdr_t hdr;
    if (!s_peer || payload_len > 0xFFFF) return -1;
    pkt = enet_packet_create(NULL, sizeof(hdr) + payload_len + blob_len,
                             reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!pkt) return -1;
    hdr.type = type;
    hdr.version = ACNET_PROTOCOL_VERSION;
    hdr.payload_len = (uint16_t)payload_len;
    memcpy(pkt->data, &hdr, sizeof(hdr));
    if (payload_len) memcpy(pkt->data + sizeof(hdr), payload, payload_len);
    if (blob_len) memcpy(pkt->data + sizeof(hdr) + payload_len, blob, blob_len);
    if (enet_peer_send(s_peer, channel, pkt) < 0) {
        enet_packet_destroy(pkt);
        return -1;
    }
    return 0;
}

static int64_t local_ms(void) {
    return (int64_t)enet_time_get();
}

/* Wait for one packet of the given type (0 = any), servicing the host.
 * Returns the type, -1 on disconnect, 0 on timeout. On TOWN_DATA with a
 * present blob, writes the blob to NET_GCI_PATH. */
static int pump_until(uint8_t want, uint32_t timeout_ms);

/* Copy the current local town aside once, the first time we are about to
 * replace it with a downloaded one. Best effort: never blocks the download. */
static void backup_local_town_once(void) {
    FILE* src;
    FILE* dst;
    char buf[8192];
    size_t n;

    dst = fopen(NET_GCI_PRISTINE, "rb");
    if (dst) { fclose(dst); return; }    /* already saved a pristine copy */
    src = fopen(NET_GCI_PATH, "rb");
    if (!src) return;                     /* nothing here yet; nothing to lose */
    dst = fopen(NET_GCI_PRISTINE, "wb");
    if (!dst) { fclose(src); return; }
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) break;
    }
    fclose(src);
    fclose(dst);
    OSReport("[net] kept a copy of your previous town at %s\n", NET_GCI_PRISTINE);
}

static int write_town_file(const uint8_t* blob, size_t len) {
    FILE* fp;
    char tmp[256];
#ifdef _WIN32
    _mkdir("save"); _mkdir(NET_CARD_A_DIR);
#else
    mkdir("save", 0755); mkdir(NET_CARD_A_DIR, 0755);
#endif
    backup_local_town_once();
    snprintf(tmp, sizeof(tmp), "%s.netdl", NET_GCI_PATH);
    fp = fopen(tmp, "wb");
    if (!fp) return -1;
    if (fwrite(blob, 1, len, fp) != len) { fclose(fp); remove(tmp); return -1; }
    fflush(fp);
    fclose(fp);
    remove(NET_GCI_PATH);
    if (rename(tmp, NET_GCI_PATH) != 0) { remove(tmp); return -1; }
    return 0;
}

static int handle_control(const acnet_hdr_t* hdr, const uint8_t* payload, size_t payload_len,
                          const uint8_t* blob, size_t blob_len) {
    switch (hdr->type) {
    case ACNET_MSG_WELCOME: {
        acnet_welcome_t w;
        if (payload_len != sizeof(w)) return -1;
        memcpy(&w, payload, sizeof(w));
        s_self_id = w.client_id;
        s_slot = w.slot;
        s_authority_id = w.authority_client_id;
        s_town_version = w.town_version;
        clock_learn(w.server_unix_ms, w.server_tz_min);
        memset(s_peer_name, 0, sizeof(s_peer_name));
        {
            int i;
            for (i = 0; i < w.peer_count && i < ACNET_MAX_PLAYERS; i++) {
                peer_name_set(w.peers[i].slot, w.peers[i].name);
            }
        }
        if (s_slot >= 0 && s_slot < ACNET_MAX_PLAYERS) {
            snprintf(s_peer_name[s_slot], sizeof(s_peer_name[s_slot]), "%s", s_cfg.name);
        }
        OSReport("[net] logged in: resident slot %d, town %s (v%u), authority client %d, %u peer(s)\n",
                 w.slot, w.town_present ? "present" : "absent", w.town_version, w.authority_client_id,
                 w.peer_count);
        return ACNET_MSG_WELCOME;
    }
    case ACNET_MSG_REJECT: {
        acnet_reject_t r;
        if (payload_len != sizeof(r)) return -1;
        memcpy(&r, payload, sizeof(r));
        r.text[sizeof(r.text) - 1] = '\0';
        OSReport("[net] server rejected login (reason %u): %s\n", r.reason, r.text);
        return ACNET_MSG_REJECT;
    }
    case ACNET_MSG_TOWN_DATA: {
        acnet_town_data_t d;
        if (payload_len != sizeof(d)) return -1;
        memcpy(&d, payload, sizeof(d));
        s_town_version = d.town_version;
        /* A town stored by the four-resident builds is accepted too; the
         * loader converts it, and the next save uploads the new layout. */
        s_town_present = d.present && (blob_len == ACNET_TOWN_SIZE || blob_len == ACNET_LEGACY_TOWN_SIZE);
        if (s_town_present) {
            if (write_town_file(blob, blob_len) == 0) {
                OSReport("[net] downloaded town v%u into %s\n", d.town_version, NET_GCI_PATH);
            } else {
                OSReport("[net] FAILED to write downloaded town to disk\n");
            }
        } else {
            OSReport("[net] server has no town yet; this client will found it on first save\n");
        }
        return ACNET_MSG_TOWN_DATA;
    }
    case ACNET_MSG_TOWN_ACK: {
        acnet_town_ack_t a;
        if (payload_len != sizeof(a)) return -1;
        memcpy(&a, payload, sizeof(a));
        if (a.status == ACNET_ACK_OK) {
            s_town_version = a.town_version;
            OSReport("[net] town uploaded, now v%u\n", a.town_version);
            if (a.by_slot < ACNET_MAX_PLAYERS && (int)a.by_slot != s_slot) {
                OSReport("[net] server has us as resident %u (was %d)\n", a.by_slot, s_slot);
                s_slot = a.by_slot;
                remote_drop_slot(s_slot);
            }
        } else {
            OSReport("[net] town upload refused (status %u)\n", a.status);
        }
        return ACNET_MSG_TOWN_ACK;
    }
    case ACNET_MSG_TOWN_VERSION: {
        acnet_town_ack_t a;
        if (payload_len != sizeof(a)) return -1;
        memcpy(&a, payload, sizeof(a));
        s_town_version = a.town_version;
        OSReport("[net] another resident (slot %u) saved; town is now v%u (will refresh on next visit home)\n",
                 a.by_slot, a.town_version);
        return ACNET_MSG_TOWN_VERSION;
    }
    case ACNET_MSG_LAND_CELLS: {
        acnet_land_hdr_t h;
        int i;
        if (payload_len < sizeof(h)) return -1;
        memcpy(&h, payload, sizeof(h));
        if (h.count == 0 || h.count > ACNET_LAND_MAX_CELLS ||
            payload_len != sizeof(h) + (size_t)h.count * sizeof(acnet_land_cell_t)) return -1;
        for (i = 0; i < h.count; i++) {
            acnet_land_cell_t c;
            memcpy(&c, payload + sizeof(h) + i * sizeof(c), sizeof(c));
            land_in_push(&c);
            s_land_rx++;
        }
        return ACNET_MSG_LAND_CELLS;
    }
    case ACNET_MSG_RESIDENT_DATA: {
        acnet_resident_data_t r;
        if (payload_len != sizeof(r)) return -1;
        memcpy(&r, payload, sizeof(r));
        if (r.slot < ACNET_MAX_PLAYERS && (int)r.slot != s_slot && blob_len == ACNET_RESIDENT_BLOB_SIZE) {
            memcpy(s_resident_update[r.slot].bytes, blob, ACNET_RESIDENT_BLOB_SIZE);
            s_resident_update[r.slot].version = r.town_version;
            s_resident_update[r.slot].house = r.house;
            s_resident_update[r.slot].pending = 1;
            OSReport("[net] resident %u sent their character and house (house %u, town v%u); taking them into this town\n",
                     r.slot, r.house, r.town_version);
        }
        return ACNET_MSG_RESIDENT_DATA;
    }
    case ACNET_MSG_WEATHER: {
        acnet_weather_t w;
        if (payload_len != sizeof(w)) return -1;
        memcpy(&w, payload, sizeof(w));
        s_weather_in_type = w.type;
        s_weather_in_intensity = w.intensity;
        memcpy(s_weather_in_grow, w.grow_renew_time, sizeof(s_weather_in_grow));
        s_weather_in_pending = 1;
        return ACNET_MSG_WEATHER;
    }
    case ACNET_MSG_SLOT: {
        acnet_slot_t r;
        if (payload_len != sizeof(r)) return -1;
        memcpy(&r, payload, sizeof(r));
        if (r.slot < ACNET_MAX_PLAYERS) {
            if ((int)r.slot != s_slot) remote_drop_slot(r.slot); /* never puppet ourselves */
            s_slot = r.slot;
        }
        if (r.slot < ACNET_MAX_PLAYERS && (int)r.slot != s_slot) {
            /* Our name moves with us; whoever was in the new slot swapped into our old one. */
            char old_name[ACNET_NAME_LEN + 1];
            memcpy(old_name, s_peer_name[r.slot], sizeof(old_name));
            snprintf(s_peer_name[r.slot], sizeof(s_peer_name[r.slot]), "%s", s_cfg.name);
            if (s_slot >= 0 && s_slot < ACNET_MAX_PLAYERS) memcpy(s_peer_name[s_slot], old_name, sizeof(old_name));
        }
        if (r.reason == ACNET_SLOT_ACCEPTED) {
            OSReport("[net] server agrees: we are resident %u\n", r.slot);
        } else if (r.reason == ACNET_SLOT_REFUSED) {
            OSReport("[net] server REFUSED our move: save block %d belongs to another resident who has saved; "
                     "we stay resident %u. Their character and house may be shown in place of ours.\n",
                     s_claimed_no, r.slot);
        } else {
            OSReport("[net] server moved us to resident %u to make room for another resident's character\n",
                     r.slot);
            s_claimed_no = -1; /* re-check against our block next frame */
        }
        return ACNET_MSG_SLOT;
    }
    case ACNET_MSG_AUTHORITY: {
        acnet_authority_t a;
        if (payload_len != sizeof(a)) return -1;
        memcpy(&a, payload, sizeof(a));
        s_authority_id = a.client_id;
        return ACNET_MSG_AUTHORITY;
    }
    case ACNET_MSG_PEER_JOINED: {
        acnet_peer_t p;
        if (payload_len == sizeof(p)) {
            memcpy(&p, payload, sizeof(p));
            peer_name_set(p.slot, p.name);
        }
        return hdr->type;
    }
    case ACNET_MSG_PEER_LEFT: {
        acnet_peer_t p;
        if (payload_len == sizeof(p)) {
            memcpy(&p, payload, sizeof(p));
            remote_drop_slot(p.slot); /* remove their puppet immediately */
            if (p.slot < ACNET_MAX_PLAYERS && (int)p.slot != s_slot) s_peer_name[p.slot][0] = '\0';
        }
        return hdr->type;
    }
    case ACNET_MSG_PONG: {
        acnet_pong_t p;
        if (payload_len == sizeof(p)) {
            memcpy(&p, payload, sizeof(p));
            clock_learn(p.server_unix_ms, p.server_tz_min);
        }
        return hdr->type;
    }
    default:
        return hdr->type;
    }
}

/* Service the connection. want != 0: block up to timeout_ms for a packet of
 * that type. want == 0 with timeout 0: the per-frame call -- deliver every
 * packet already waiting and send everything queued, then return at once.
 * That second mode must actually reach enet_host_service(): a timeout of
 * zero is a valid non-blocking poll, and it is also the only moment ENet
 * transmits what we queued, so skipping it silently stalls both directions. */
static int pump_until(uint8_t want, uint32_t timeout_ms) {
    uint32_t deadline = enet_time_get() + timeout_ms;
    if (!s_client_host) return -1;
    for (;;) {
        ENetEvent ev;
        uint32_t now = enet_time_get();
        int32_t left = (int32_t)(deadline - now);
        int rc;
        if (left < 0) left = 0;
        rc = enet_host_service(s_client_host, &ev, (uint32_t)left);
        if (rc < 0) return -1;
        if (rc == 0) {
            if (left == 0) return 0; /* nothing more waiting (or timed out) */
            continue;
        }
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            s_active = 0;
            return -1;
        }
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            acnet_hdr_t hdr;
            int t = -2;
            if (ev.packet->dataLength >= sizeof(hdr)) {
                size_t payload_len, blob_len;
                const uint8_t* payload;
                memcpy(&hdr, ev.packet->data, sizeof(hdr));
                payload = ev.packet->data + sizeof(hdr);
                payload_len = hdr.payload_len;
                if (sizeof(hdr) + payload_len <= ev.packet->dataLength) {
                    blob_len = ev.packet->dataLength - sizeof(hdr) - payload_len;
                    if (hdr.type == ACNET_MSG_PLAYER_STATE) {
                        if (payload_len == sizeof(acnet_player_state_t)) {
                            acnet_player_state_t st;
                            memcpy(&st, payload, sizeof(st));
                            remote_apply_state(&st);
                            s_puppet_rx++;
                        }
                        t = hdr.type;
                    } else if (hdr.type == ACNET_MSG_NPC_STATE) {
                        acnet_npc_hdr_t h;
                        if (payload_len >= sizeof(h)) {
                            memcpy(&h, payload, sizeof(h));
                            if (h.count > 0 && h.count <= ACNET_NPC_MAX &&
                                payload_len == sizeof(h) + (size_t)h.count * sizeof(acnet_npc_state_t)) {
                                npc_apply_states(h.client_id, payload + sizeof(h), h.count);
                            }
                        }
                        t = hdr.type;
                    } else if (hdr.type == ACNET_MSG_CHAT) {
                        if (payload_len == sizeof(acnet_chat_t)) {
                            acnet_chat_t m;
                            memcpy(&m, payload, sizeof(m));
                            chat_msg_t* slot;
                            if (m.len > ACNET_CHAT_LEN) m.len = ACNET_CHAT_LEN;
                            if (s_chat_in_count == CHAT_IN_CAP) { /* drop the oldest */
                                s_chat_in_head = (s_chat_in_head + 1) % CHAT_IN_CAP;
                                s_chat_in_count--;
                            }
                            slot = &s_chat_in[(s_chat_in_head + s_chat_in_count) % CHAT_IN_CAP];
                            slot->slot = m.slot;
                            slot->len = m.len;
                            memcpy(slot->text, m.text, m.len);
                            slot->text[m.len] = '\0';
                            s_chat_in_count++;
                            s_chat_rx++;
                        }
                        t = hdr.type;
                    } else {
                        t = handle_control(&hdr, payload, payload_len, payload + payload_len, blob_len);
                    }
                }
            }
            enet_packet_destroy(ev.packet);
            if (t == ACNET_MSG_REJECT) return t;
            if (want != 0 && t == (int)want) return t;
            /* want == 0: keep draining; the rc == 0 branch above ends it. */
        }
    }
}

/* --------------------------------------------------------------- public */

int pc_net_enabled(void) { return s_active; }
int pc_net_assigned_slot(void) { return s_slot; }


void pc_net_set_player_no(int player_no) {
    acnet_claim_slot_t q;
    if (!s_active || player_no < 0 || player_no >= ACNET_MAX_PLAYERS) return;
    if (player_no == s_slot) {
        s_claimed_no = player_no;
        return;
    }
    if (player_no == s_claimed_no) return; /* asked already: waiting, or it was refused */
    s_claimed_no = player_no;
    memset(&q, 0, sizeof(q));
    q.player_no = (uint8_t)player_no;
    send_msg(ACNET_CH_CONTROL, ACNET_MSG_CLAIM_SLOT, &q, sizeof(q), NULL, 0, 1);
    OSReport("[net] our character lives in save block %d but the server has us as resident %d; asking to move\n",
             player_no, s_slot);
}

int pc_net_init(void) {
    ENetAddress addr;
    ENetEvent ev;
    acnet_hello_t hello;
    int rc;

    read_config();
    if (!s_cfg.configured) {
        OSReport("[net] no [Network] block in settings.ini; single-player\n");
        return 0;
    }
    OSReport("[net] init: server=%s port=%d invite=%s name=%s show_other_players=%d\n", s_cfg.host,
             s_cfg.port, s_cfg.invite, s_cfg.name, s_cfg.puppets);

    if (enet_initialize() != 0) {
        OSReport("[net] enet_initialize failed; starting in single-player\n");
        return 0;
    }
    s_client_host = enet_host_create(NULL, 1, ACNET_CHANNELS, 0, 0);
    if (!s_client_host) {
        OSReport("[net] could not create client host; single-player\n");
        return 0;
    }
    if (enet_address_set_host(&addr, s_cfg.host) != 0) {
        OSReport("[net] cannot resolve server '%s'; single-player\n", s_cfg.host);
        return 0;
    }
    addr.port = (enet_uint16)s_cfg.port;
    OSReport("[net] connecting to %s:%d, town '%s', as '%s'\n", s_cfg.host, s_cfg.port, s_cfg.invite,
             s_cfg.name);
    s_peer = enet_host_connect(s_client_host, &addr, ACNET_CHANNELS, 0);
    if (!s_peer) return 0;
    rc = enet_host_service(s_client_host, &ev, 5000);
    if (rc <= 0 || ev.type != ENET_EVENT_TYPE_CONNECT) {
        OSReport("[net] could not reach server; starting in single-player\n");
        enet_peer_reset(s_peer);
        s_peer = NULL;
        return 0;
    }

    memset(&hello, 0, sizeof(hello));
    snprintf(hello.invite, sizeof(hello.invite), "%s", s_cfg.invite);
    snprintf(hello.name, sizeof(hello.name), "%s", s_cfg.name);
    hello.client_build = 1;
    hello.want_slot = (uint8_t)s_cfg.want_slot;
    send_msg(ACNET_CH_CONTROL, ACNET_MSG_HELLO, &hello, sizeof(hello), NULL, 0, 1);

    if (pump_until(ACNET_MSG_WELCOME, 5000) != ACNET_MSG_WELCOME) {
        OSReport("[net] login failed; starting in single-player\n");
        enet_peer_disconnect_now(s_peer, 0);
        s_peer = NULL;
        return 0;
    }
    /* Ask for the town and wait for it (or the "no town yet" reply). */
    send_msg(ACNET_CH_CONTROL, ACNET_MSG_TOWN_REQUEST, NULL, 0, NULL, 0, 1);
    if (pump_until(ACNET_MSG_TOWN_DATA, 15000) != ACNET_MSG_TOWN_DATA) {
        OSReport("[net] never received town data; disconnecting to single-player\n");
        enet_peer_disconnect_now(s_peer, 0);
        s_peer = NULL;
        return 0;
    }
    s_active = 1;

    if (!s_town_present) {
        /* The server has no town yet. If this PC already has one, it becomes
         * the shared town right now, before the game even starts, so a host's
         * existing town is on the server before any friend enters. Waiting
         * for the host's first in-game save left a window in which a joiner
         * would start a brand-new town and found the server with that. */
        FILE* f = fopen(NET_GCI_PATH, "rb");
        if (f) {
            fclose(f);
            OSReport("[net] server has no town yet; uploading this PC's town to found it\n");
            pc_net_on_saved(ACNET_UPLOAD_NEW_TOWN);
        } else {
            OSReport("[net] server has no town yet and this PC has none either; "
                     "this client will found it on first save\n");
        }
    }
    return 1;
}

void pc_net_service(void) {
    static uint32_t last_report;
    uint32_t now;
    if (!s_active) return;
    (void)pump_until(0, 0); /* deliver waiting packets, transmit queued ones */
    remote_expire();
    now = enet_time_get();
    if (now - last_report >= 5000) {
        int i, n = 0;
        for (i = 0; i < ACNET_MAX_PLAYERS; i++) n += s_remote[i].active ? 1 : 0;
        OSReport("[net] state sent=%u received=%u residents-visible=%d land-cells sent=%u received=%u%s villagers sent=%u received=%u blocks-pushed=%u rtt=%ums\n",
                 s_puppet_tx, s_puppet_rx, n, s_land_tx, s_land_rx, s_land_dropped ? " (some dropped!)" : "",
                 s_npc_tx, s_npc_rx, s_push_tx, s_peer ? s_peer->roundTripTime : 0u);
        last_report = now;
    }
    {
        /* A ping a minute keeps the town clock lined up with the host's. */
        static uint32_t last_ping;
        if (now - last_ping >= 60000) {
            acnet_ping_t p;
            p.nonce = now;
            send_msg(ACNET_CH_CONTROL, ACNET_MSG_PING, &p, sizeof(p), NULL, 0, 1);
            last_ping = now;
        }
    }
}

/* --- Player-state stream (step 2) --------------------------------------- */

void pc_net_send_player_state(const acnet_player_state_t* state) {
    static uint16_t seq;
    acnet_player_state_t s;
    uint32_t now;
    if (!s_active) return;
    /* The game calls this every frame; 30 Hz is plenty for a puppet and
     * keeps the unreliable channel from queueing faster than it drains. */
    now = enet_time_get();
    if (now - s_last_state_ms < 33) return;
    s_last_state_ms = now;
    s_puppet_tx++;
    s = *state;
    s.client_id = (uint8_t)s_self_id;
    s.slot = (uint8_t)s_slot;
    s.seq = ++seq;
    /* Unreliable, sequenced: newest state wins, drops are fine. */
    send_msg(ACNET_CH_STATE, ACNET_MSG_PLAYER_STATE, &s, sizeof(s), NULL, 0, 0);
}

int pc_net_local_slot(void) { return s_active ? s_slot : -1; }

int pc_net_self_id(void) { return s_active ? s_self_id : -1; }

void pc_net_send_npc_states(const acnet_npc_state_t* states, int count) {
    uint8_t buf[sizeof(acnet_npc_hdr_t) + ACNET_NPC_MAX * sizeof(acnet_npc_state_t)];
    acnet_npc_hdr_t h;
    if (!s_active || count <= 0) return;
    if (count > ACNET_NPC_MAX) count = ACNET_NPC_MAX;
    memset(&h, 0, sizeof(h));
    h.client_id = (uint8_t)s_self_id;
    h.count = (uint8_t)count;
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), states, (size_t)count * sizeof(*states));
    send_msg(ACNET_CH_STATE, ACNET_MSG_NPC_STATE, buf, sizeof(h) + (size_t)count * sizeof(*states), NULL, 0, 0);
    s_npc_tx += (unsigned)count;
}

int pc_net_get_npc_state(unsigned npc_id, acnet_npc_state_t* out, int* from_client) {
    int i;
    if (!s_active) return 0;
    for (i = 0; i < NPC_TABLE_SIZE; i++) {
        if (s_npc[i].active && s_npc[i].st.npc_id == npc_id) {
            if (out) *out = s_npc[i].st;
            if (from_client) *from_client = s_npc[i].client_id;
            return 1;
        }
    }
    return 0;
}

/* ----- weather ----- */

int pc_net_is_authority(void) { return s_active && s_authority_id == s_self_id; }

int pc_net_world_is_remote(void) { return s_active && s_authority_id != s_self_id; }

void pc_net_send_weather(int type, int intensity, const void* grow_renew_time, size_t grow_renew_size) {
    acnet_weather_t w;
    uint32_t now;
    if (!pc_net_is_authority()) return;
    if (grow_renew_size > ACNET_GROW_TIME_SIZE) grow_renew_size = ACNET_GROW_TIME_SIZE;
    memset(&w, 0, sizeof(w));
    memcpy(w.grow_renew_time, grow_renew_time, grow_renew_size);
    now = enet_time_get();
    if (type == s_weather_sent_type && intensity == s_weather_sent_intensity &&
        memcmp(w.grow_renew_time, s_weather_sent_grow, sizeof(w.grow_renew_time)) == 0 &&
        now - s_weather_sent_ms < 5000) return;
    s_weather_sent_type = type;
    s_weather_sent_intensity = intensity;
    memcpy(s_weather_sent_grow, w.grow_renew_time, sizeof(s_weather_sent_grow));
    s_weather_sent_ms = now;
    w.type = (int16_t)type;
    w.intensity = (int16_t)intensity;
    send_msg(ACNET_CH_CONTROL, ACNET_MSG_WEATHER, &w, sizeof(w), NULL, 0, 1);
}

int pc_net_take_weather(int* type, int* intensity, void* grow_renew_time_out, size_t grow_renew_size) {
    if (!s_active || !s_weather_in_pending) return 0;
    s_weather_in_pending = 0;
    *type = s_weather_in_type;
    *intensity = s_weather_in_intensity;
    if (grow_renew_size > ACNET_GROW_TIME_SIZE) grow_renew_size = ACNET_GROW_TIME_SIZE;
    memcpy(grow_renew_time_out, s_weather_in_grow, grow_renew_size);
    return 1;
}

int pc_net_puppets_enabled(void) { return s_active && s_cfg.puppets; }

void pc_net_send_land_cells(const acnet_land_cell_t* cells, int count) {
    uint8_t buf[sizeof(acnet_land_hdr_t) + ACNET_LAND_MAX_CELLS * sizeof(acnet_land_cell_t)];
    if (!s_active) return;
    while (count > 0) {
        int n = count > ACNET_LAND_MAX_CELLS ? ACNET_LAND_MAX_CELLS : count;
        acnet_land_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.count = (uint8_t)n;
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), cells, (size_t)n * sizeof(acnet_land_cell_t));
        send_msg(ACNET_CH_CONTROL, ACNET_MSG_LAND_CELLS, buf, sizeof(h) + (size_t)n * sizeof(acnet_land_cell_t),
                 NULL, 0, 1);
        s_land_tx += (unsigned)n;
        cells += n;
        count -= n;
    }
}

int pc_net_take_land_cells(acnet_land_cell_t* out, int max) {
    int n = 0;
    if (!s_active) return 0;
    while (n < max && s_land_in_count > 0) {
        out[n++] = s_land_in[s_land_in_head];
        s_land_in_head = (s_land_in_head + 1) % LAND_IN_CAP;
        s_land_in_count--;
    }
    return n;
}

int pc_net_resident_update_house(int slot) {
    if (!s_active || slot < 0 || slot >= ACNET_MAX_PLAYERS || slot == s_slot) return -1;
    if (!s_resident_update[slot].pending) return -1;
    return s_resident_update[slot].house;
}

int pc_net_take_resident_update(int slot, void* private_out, size_t private_len, void* home_out,
                                size_t home_len) {
    resident_update_t* u;
    if (!s_active || slot < 0 || slot >= ACNET_MAX_PLAYERS || slot == s_slot) return 0;
    u = &s_resident_update[slot];
    if (!u->pending) return 0;
    if (private_len != ACNET_PRIVATE_SIZE || home_len != ACNET_HOME_SIZE) {
        OSReport("[net] resident block size mismatch (%u/%u vs %u/%u); live sync disabled for slot %d\n",
                 (unsigned)private_len, (unsigned)home_len, (unsigned)ACNET_PRIVATE_SIZE,
                 (unsigned)ACNET_HOME_SIZE, slot);
        u->pending = 0;
        return 0;
    }
    memcpy(private_out, u->bytes, ACNET_PRIVATE_SIZE);
    memcpy(home_out, u->bytes + ACNET_PRIVATE_SIZE, ACNET_HOME_SIZE);
    u->pending = 0;
    return 1;
}

int pc_net_get_remote_state(int slot, acnet_player_state_t* out) {
    if (!s_active || slot < 0 || slot >= ACNET_MAX_PLAYERS || !s_remote[slot].active) return 0;
    if (out) *out = s_remote[slot].cur;
    return 1;
}

int pc_net_remote_count(void) {
    int i, n = 0;
    if (!s_active) return 0;
    for (i = 0; i < ACNET_MAX_PLAYERS; i++) if (s_remote[i].active) n++;
    return n;
}

/* --- Chat (step 4) ------------------------------------------------------- */

void pc_net_send_chat(const char* text) {
    acnet_chat_t m;
    size_t n;
    if (!s_active || !text) return;
    n = strlen(text);
    if (n > ACNET_CHAT_LEN) n = ACNET_CHAT_LEN;
    memset(&m, 0, sizeof(m));
    m.slot = (uint8_t)s_slot;
    m.len = (uint8_t)n;
    memcpy(m.text, text, n);
    send_msg(ACNET_CH_CHAT, ACNET_MSG_CHAT, &m, sizeof(m), NULL, 0, 1);
}

int pc_net_poll_chat(int* out_slot, char* out_text, int out_size) {
    chat_msg_t* m;
    if (!s_active || s_chat_in_count == 0) return 0;
    m = &s_chat_in[s_chat_in_head];
    if (out_slot) *out_slot = m->slot;
    if (out_text && out_size > 0) {
        int n = m->len < (out_size - 1) ? m->len : (out_size - 1);
        memcpy(out_text, m->text, n);
        out_text[n] = '\0';
    }
    s_chat_in_head = (s_chat_in_head + 1) % CHAT_IN_CAP;
    s_chat_in_count--;
    return 1;
}

/* --- Clock (step 4) ------------------------------------------------------ */

long long pc_net_server_clock_skew_ms(void) { return (s_active && s_clock_skew_known) ? (long long)s_clock_skew_ms : 0; }

const char* pc_net_peer_name(int slot) {
    if (!s_active || slot < 0 || slot >= ACNET_MAX_PLAYERS) return "";
    return s_peer_name[slot];
}

/* --- Resident push ------------------------------------------------------- */

static uint32_t fnv1a(const uint8_t* p, size_t n, uint32_t h) {
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

int pc_net_push_own_blocks(const void* private_be, size_t private_len, const void* home_be, size_t home_len,
                           int house, int force) {
    uint32_t now, h;
    uint8_t blob[ACNET_RESIDENT_BLOB_SIZE];
    acnet_resident_push_t q;
    if (!s_active || s_slot < 0 || private_len != ACNET_PRIVATE_SIZE || home_len != ACNET_HOME_SIZE) return 0;
    now = enet_time_get();
    if (s_push_hash_known && now - s_push_last_ms < 5000) return 0; /* at most one every 5 s */
    h = fnv1a((const uint8_t*)home_be, home_len, fnv1a((const uint8_t*)private_be, private_len, 2166136261u));
    if (s_push_hash_known && h == s_push_hash && !force) return 0;
    memcpy(blob, private_be, ACNET_PRIVATE_SIZE);
    memcpy(blob + ACNET_PRIVATE_SIZE, home_be, ACNET_HOME_SIZE);
    memset(&q, 0, sizeof(q));
    q.slot = (uint8_t)s_slot;
    q.house = (uint8_t)house;
    send_msg(ACNET_CH_CONTROL, ACNET_MSG_RESIDENT_PUSH, &q, sizeof(q), blob, sizeof(blob), 1);
    s_push_last_ms = now;
    s_push_hash = h;
    s_push_hash_known = 1;
    s_push_tx++;
    return 1;
}

static uint8_t* read_town_file(size_t* out_len) {
    FILE* fp = fopen(NET_GCI_PATH, "rb");
    long n;
    uint8_t* buf;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n != (long)ACNET_TOWN_SIZE) { fclose(fp); return NULL; }
    buf = (uint8_t*)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    *out_len = (size_t)n;
    return buf;
}

void pc_net_on_saved(int reason) {
    size_t len = 0;
    uint8_t* blob;
    acnet_town_upload_t u;
    if (!s_active) return;
    blob = read_town_file(&len);
    if (!blob) {
        OSReport("[net] on_saved: could not read %s to upload\n", NET_GCI_PATH);
        return;
    }
    memset(&u, 0, sizeof(u));
    u.reason = (uint8_t)reason;
    send_msg(ACNET_CH_CONTROL, ACNET_MSG_TOWN_UPLOAD, &u, sizeof(u), blob, len, 1);
    free(blob);
    enet_host_flush(s_client_host);
    (void)pump_until(ACNET_MSG_TOWN_ACK, 10000);
}

void pc_net_shutdown(void) {
    if (s_peer) {
        if (s_active) pc_net_on_saved(ACNET_UPLOAD_LEAVE);
        enet_peer_disconnect(s_peer, 0);
        if (s_client_host) {
            ENetEvent ev;
            uint32_t end = enet_time_get() + 1000;
            while ((int32_t)(end - enet_time_get()) > 0 && enet_host_service(s_client_host, &ev, 100) >= 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
                if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
            }
        }
        s_peer = NULL;
    }
    if (s_client_host) {
        enet_host_destroy(s_client_host);
        s_client_host = NULL;
        enet_deinitialize();
    }
    s_active = 0;
}
