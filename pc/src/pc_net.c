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
#include <sys/stat.h>

#include "protocol.h"
#include <dolphin/os.h> /* OSReport */

/* The card-A GCI the game loads/saves (mirrors PC_GCI_PATH in pc_m_card.c). */
#define NET_CARD_A_DIR "save/card_a"
#define NET_GCI_PATH   NET_CARD_A_DIR "/DobutsunomoriP_MURA.gci"

typedef struct {
    int    configured;         /* a server address was found in settings.ini */
    char   host[128];
    int    port;
    char   invite[ACNET_INVITE_LEN + 1];
    char   name[ACNET_NAME_LEN + 1];
    int    want_slot;          /* 0..3 or ACNET_SLOT_ANY */
} net_config_t;

static net_config_t s_cfg;
static ENetHost*    s_host;
static ENetPeer*    s_peer;
static int          s_active;          /* logged in and town in place */
static int          s_slot = -1;
static int          s_authority_id = -1;
static int          s_self_id = -1;
static int64_t      s_clock_skew_ms;   /* server_ms - local_ms at login */
static uint32_t     s_town_version;
static unsigned     s_puppet_rx;       /* counters for diagnostics until step 2/3 */
static unsigned     s_chat_rx;

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

static int write_town_file(const uint8_t* blob, size_t len) {
    FILE* fp;
    char tmp[256];
#ifdef _WIN32
    _mkdir("save"); _mkdir(NET_CARD_A_DIR);
#else
    mkdir("save", 0755); mkdir(NET_CARD_A_DIR, 0755);
#endif
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
        s_clock_skew_ms = w.server_unix_ms - local_ms();
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
        if (d.present && blob_len == ACNET_TOWN_SIZE) {
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
    case ACNET_MSG_AUTHORITY: {
        acnet_authority_t a;
        if (payload_len != sizeof(a)) return -1;
        memcpy(&a, payload, sizeof(a));
        s_authority_id = a.client_id;
        return ACNET_MSG_AUTHORITY;
    }
    case ACNET_MSG_PEER_JOINED:
    case ACNET_MSG_PEER_LEFT:
        return hdr->type;
    default:
        return hdr->type;
    }
}

static int pump_until(uint8_t want, uint32_t timeout_ms) {
    uint32_t deadline = enet_time_get() + timeout_ms;
    if (!s_host) return -1;
    for (;;) {
        ENetEvent ev;
        uint32_t now = enet_time_get();
        int rc;
        if ((int32_t)(deadline - now) <= 0) return 0;
        rc = enet_host_service(s_host, &ev, deadline - now);
        if (rc < 0) return -1;
        if (rc == 0) continue;
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
                        s_puppet_rx++; /* step 2 will apply these */
                        t = hdr.type;
                    } else if (hdr.type == ACNET_MSG_CHAT) {
                        s_chat_rx++;   /* step 4 will surface these */
                        t = hdr.type;
                    } else {
                        t = handle_control(&hdr, payload, payload_len, payload + payload_len, blob_len);
                    }
                }
            }
            enet_packet_destroy(ev.packet);
            if (t == ACNET_MSG_REJECT) return t;
            if (want == 0 || t == (int)want) return t;
        }
    }
}

/* --------------------------------------------------------------- public */

int pc_net_enabled(void) { return s_active; }
int pc_net_assigned_slot(void) { return s_slot; }

int pc_net_init(void) {
    ENetAddress addr;
    ENetEvent ev;
    acnet_hello_t hello;
    int rc;

    read_config();
    if (!s_cfg.configured) return 0;

    if (enet_initialize() != 0) {
        OSReport("[net] enet_initialize failed; starting in single-player\n");
        return 0;
    }
    s_host = enet_host_create(NULL, 1, ACNET_CHANNELS, 0, 0);
    if (!s_host) {
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
    s_peer = enet_host_connect(s_host, &addr, ACNET_CHANNELS, 0);
    if (!s_peer) return 0;
    rc = enet_host_service(s_host, &ev, 5000);
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
    return 1;
}

void pc_net_service(void) {
    if (!s_active) return;
    (void)pump_until(0, 0); /* non-blocking drain */
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
    enet_host_flush(s_host);
    (void)pump_until(ACNET_MSG_TOWN_ACK, 10000);
}

void pc_net_shutdown(void) {
    if (s_peer) {
        if (s_active) pc_net_on_saved(ACNET_UPLOAD_LEAVE);
        enet_peer_disconnect(s_peer, 0);
        if (s_host) {
            ENetEvent ev;
            uint32_t end = enet_time_get() + 1000;
            while ((int32_t)(end - enet_time_get()) > 0 && enet_host_service(s_host, &ev, 100) >= 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
                if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
            }
        }
        s_peer = NULL;
    }
    if (s_host) {
        enet_host_destroy(s_host);
        s_host = NULL;
        enet_deinitialize();
    }
    s_active = 0;
}
