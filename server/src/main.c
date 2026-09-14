/* main.c - Animal Crossing shared-town server.
 *
 * What it does: holds the canonical town save for each invite code, hands it
 * to residents when they log in, accepts their uploads (splicing so nobody
 * can overwrite another resident's character), relays player state and
 * chat between the residents of a town, elects a "world authority" client,
 * and serves the town clock.
 *
 * What it does not do: run the game. See docs/MULTIPLAYER.md.
 */
#include <enet/enet.h>

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "protocol.h"
#include "town.h"

#define MAX_TOWNS   16
#define MAX_INVITES 64
#define UPLOAD_MIN_INTERVAL_MS 2000

typedef struct {
    int      in_use;
    int      id;              /* 1..255, unique among connected clients */
    int      logged_in;
    int      room;            /* index into g_rooms, -1 when not logged in */
    int      slot;
    char     name[ACNET_NAME_LEN + 1];
    uint64_t login_seq;
    uint32_t last_upload_ms;
    ENetPeer* peer;
} client_t;

typedef struct {
    int    in_use;
    town_t town;
    int    authority_id;      /* client id, 0 = none */
} room_t;

static client_t g_clients[ACNET_MAX_CLIENTS];
static room_t   g_rooms[MAX_TOWNS];
static char     g_invites[MAX_INVITES][ACNET_INVITE_LEN + 1];
static int      g_invite_count;
static char     g_data_root[512] = "data";
static int      g_verbose;
static uint64_t g_login_seq;
static volatile sig_atomic_t g_stop;

/* ---------------------------------------------------------------- logging */

static void logf_(const char* fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    va_list ap;
#ifdef _WIN32
    localtime_s(&tmv, &now); /* note: Windows argument order is (tm*, time_t*) */
#else
    localtime_r(&now, &tmv);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    printf("%s ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static int64_t unix_ms(void) {
#ifdef _WIN32
    /* FILETIME is 100ns ticks since 1601-01-01; shift epoch to 1970. */
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

/* --------------------------------------------------------------- invites */

static void load_invites(void) {
    char path[600];
    FILE* fp;
    snprintf(path, sizeof(path), "%s/invites.txt", g_data_root);
    fp = fopen(path, "r");
    if (!fp) {
        /* First run: create one random invite code so the server is usable
         * immediately. The operator can edit invites.txt afterwards. */
        static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
        char code[13];
        FILE* rnd = fopen("/dev/urandom", "rb");
        int i;
        for (i = 0; i < 12; i++) {
            unsigned char b = 0;
            if (!rnd || fread(&b, 1, 1, rnd) != 1) b = (unsigned char)(rand() ^ (i * 31));
            code[i] = alphabet[b % (sizeof(alphabet) - 1)];
        }
        code[12] = '\0';
        if (rnd) fclose(rnd);
        fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "%s\n", code);
            fclose(fp);
        }
        snprintf(g_invites[0], sizeof(g_invites[0]), "%s", code);
        g_invite_count = 1;
        logf_("no invites.txt found; created one invite code: %s", code);
        return;
    }
    while (g_invite_count < MAX_INVITES) {
        char line[128];
        char* p;
        if (!fgets(line, sizeof(line), fp)) break;
        p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        {
            char* q = p;
            while (*q && !isspace((unsigned char)*q)) q++;
            *q = '\0';
        }
        if (strlen(p) == 0 || strlen(p) > ACNET_INVITE_LEN) continue;
        snprintf(g_invites[g_invite_count++], ACNET_INVITE_LEN + 1, "%s", p);
    }
    fclose(fp);
    logf_("loaded %d invite code(s) from %s", g_invite_count, path);
}

static int invite_allowed(const char* code) {
    int i;
    for (i = 0; i < g_invite_count; i++) {
        if (strcmp(g_invites[i], code) == 0) return 1;
    }
    return 0;
}

static int invite_chars_ok(const char* s) {
    for (; *s; s++) {
        if (!(isalnum((unsigned char)*s) || *s == '-' || *s == '_')) return 0;
    }
    return 1;
}

static int name_ok(const char* s) {
    size_t n = strlen(s);
    if (n == 0 || n > ACNET_NAME_LEN - 1) return 0;
    for (; *s; s++) {
        if (!(isalnum((unsigned char)*s) || *s == '-' || *s == '_')) return 0;
    }
    return 1;
}

/* ----------------------------------------------------------------- rooms */

static int room_find_or_open(const char* invite) {
    int i, free_idx = -1;
    for (i = 0; i < MAX_TOWNS; i++) {
        if (g_rooms[i].in_use && strcmp(g_rooms[i].town.invite, invite) == 0) return i;
        if (!g_rooms[i].in_use && free_idx < 0) free_idx = i;
    }
    if (free_idx < 0) return -1;
    if (town_open(&g_rooms[free_idx].town, g_data_root, invite) != 0) return -1;
    g_rooms[free_idx].in_use = 1;
    g_rooms[free_idx].authority_id = 0;
    logf_("[%s] town opened: %s, version %u", invite,
          g_rooms[free_idx].town.data ? "save present" : "no save yet", g_rooms[free_idx].town.version);
    return free_idx;
}

static client_t* client_by_id(int id) {
    int i;
    for (i = 0; i < ACNET_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].id == id) return &g_clients[i];
    }
    return NULL;
}

static int room_logged_in_count(int room) {
    int i, n = 0;
    for (i = 0; i < ACNET_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].logged_in && g_clients[i].room == room) n++;
    }
    return n;
}

/* ------------------------------------------------------------- packets */

static int send_msg(ENetPeer* peer, uint8_t channel, uint8_t type, const void* payload,
                    size_t payload_len, const void* blob, size_t blob_len, int reliable) {
    ENetPacket* pkt;
    acnet_hdr_t hdr;
    size_t total = sizeof(hdr) + payload_len + blob_len;
    if (!peer || payload_len > 0xFFFF) return -1;
    pkt = enet_packet_create(NULL, total, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!pkt) return -1;
    hdr.type = type;
    hdr.version = ACNET_PROTOCOL_VERSION;
    hdr.payload_len = (uint16_t)payload_len;
    memcpy(pkt->data, &hdr, sizeof(hdr));
    if (payload_len) memcpy(pkt->data + sizeof(hdr), payload, payload_len);
    if (blob_len) memcpy(pkt->data + sizeof(hdr) + payload_len, blob, blob_len);
    if (enet_peer_send(peer, channel, pkt) < 0) {
        enet_packet_destroy(pkt);
        return -1;
    }
    return 0;
}

static void room_broadcast(int room, const client_t* except, uint8_t channel, uint8_t type,
                           const void* payload, size_t payload_len, int reliable) {
    int i;
    for (i = 0; i < ACNET_MAX_CLIENTS; i++) {
        client_t* c = &g_clients[i];
        if (!c->in_use || !c->logged_in || c->room != room || c == except) continue;
        send_msg(c->peer, channel, type, payload, payload_len, NULL, 0, reliable);
    }
}

static void send_reject(ENetPeer* peer, uint8_t reason, const char* text) {
    acnet_reject_t r;
    memset(&r, 0, sizeof(r));
    r.reason = reason;
    snprintf(r.text, sizeof(r.text), "%s", text);
    send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_REJECT, &r, sizeof(r), NULL, 0, 1);
    enet_peer_disconnect_later(peer, 0);
}

static void room_elect_authority(int room) {
    int i, best = -1;
    uint64_t best_seq = 0;
    for (i = 0; i < ACNET_MAX_CLIENTS; i++) {
        client_t* c = &g_clients[i];
        if (!c->in_use || !c->logged_in || c->room != room) continue;
        if (best < 0 || c->login_seq < best_seq) {
            best = i;
            best_seq = c->login_seq;
        }
    }
    {
        int new_id = best >= 0 ? g_clients[best].id : 0;
        if (new_id != g_rooms[room].authority_id) {
            acnet_authority_t a;
            g_rooms[room].authority_id = new_id;
            a.client_id = (uint8_t)new_id;
            room_broadcast(room, NULL, ACNET_CH_CONTROL, ACNET_MSG_AUTHORITY, &a, sizeof(a), 1);
            logf_("[%s] world authority is now client %d (%s)", g_rooms[room].town.invite, new_id,
                  best >= 0 ? g_clients[best].name : "nobody");
        }
    }
}

/* ------------------------------------------------------------ handlers */

static void handle_hello(client_t* c, const uint8_t* payload, size_t len) {
    acnet_hello_t h;
    char invite[ACNET_INVITE_LEN + 1];
    char name[ACNET_NAME_LEN + 1];
    int room, slot, i;
    acnet_welcome_t w;
    acnet_peer_t joined;

    if (c->logged_in) return;
    if (len != sizeof(h)) {
        send_reject(c->peer, ACNET_REJECT_BAD_VERSION, "malformed hello");
        return;
    }
    memcpy(&h, payload, sizeof(h));
    memcpy(invite, h.invite, ACNET_INVITE_LEN);
    invite[ACNET_INVITE_LEN] = '\0';
    memcpy(name, h.name, ACNET_NAME_LEN);
    name[ACNET_NAME_LEN] = '\0';

    if (!invite_chars_ok(invite) || !invite_allowed(invite)) {
        logf_("client %d: bad invite code", c->id);
        send_reject(c->peer, ACNET_REJECT_BAD_INVITE, "unknown invite code");
        return;
    }
    if (!name_ok(name)) {
        send_reject(c->peer, ACNET_REJECT_BAD_NAME, "name must be 1-15 letters, digits, - or _");
        return;
    }
    room = room_find_or_open(invite);
    if (room < 0) {
        send_reject(c->peer, ACNET_REJECT_BUSY, "server has no room for another town");
        return;
    }
    for (i = 0; i < ACNET_MAX_CLIENTS; i++) {
        client_t* o = &g_clients[i];
        if (o->in_use && o->logged_in && o->room == room && strcmp(o->name, name) == 0) {
            send_reject(c->peer, ACNET_REJECT_NAME_IN_USE, "that resident is already connected");
            return;
        }
    }
    slot = town_assign_slot(&g_rooms[room].town, name, h.want_slot == ACNET_SLOT_ANY ? ACNET_SLOT_ANY : (int)h.want_slot);
    if (slot < 0) {
        if (h.want_slot != ACNET_SLOT_ANY) {
            send_reject(c->peer, ACNET_REJECT_SLOT_TAKEN, "that resident slot belongs to someone else");
        } else {
            send_reject(c->peer, ACNET_REJECT_TOWN_FULL, "this town already has four residents");
        }
        return;
    }

    c->logged_in = 1;
    c->room = room;
    c->slot = slot;
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->login_seq = ++g_login_seq;

    memset(&w, 0, sizeof(w));
    w.client_id = (uint8_t)c->id;
    w.slot = (uint8_t)slot;
    w.town_present = g_rooms[room].town.data ? 1 : 0;
    w.town_version = g_rooms[room].town.version;
    w.server_unix_ms = unix_ms();
    for (i = 0; i < ACNET_MAX_CLIENTS && w.peer_count < ACNET_MAX_PLAYERS; i++) {
        client_t* o = &g_clients[i];
        if (!o->in_use || !o->logged_in || o->room != room || o == c) continue;
        w.peers[w.peer_count].client_id = (uint8_t)o->id;
        w.peers[w.peer_count].slot = (uint8_t)o->slot;
        memcpy(w.peers[w.peer_count].name, o->name, ACNET_NAME_LEN);
        w.peer_count++;
    }
    if (g_rooms[room].authority_id == 0) g_rooms[room].authority_id = c->id;
    w.authority_client_id = (uint8_t)g_rooms[room].authority_id;
    send_msg(c->peer, ACNET_CH_CONTROL, ACNET_MSG_WELCOME, &w, sizeof(w), NULL, 0, 1);

    memset(&joined, 0, sizeof(joined));
    joined.client_id = (uint8_t)c->id;
    joined.slot = (uint8_t)slot;
    memcpy(joined.name, c->name, ACNET_NAME_LEN);
    room_broadcast(room, c, ACNET_CH_CONTROL, ACNET_MSG_PEER_JOINED, &joined, sizeof(joined), 1);
    room_elect_authority(room);
    logf_("[%s] %s logged in as resident %d (client %d), %d online", invite, name, slot, c->id,
          room_logged_in_count(room));
}

static void handle_town_request(client_t* c) {
    acnet_town_data_t d;
    town_t* t;
    if (!c->logged_in) return;
    t = &g_rooms[c->room].town;
    memset(&d, 0, sizeof(d));
    d.town_version = t->version;
    d.present = t->data ? 1 : 0;
    send_msg(c->peer, ACNET_CH_CONTROL, ACNET_MSG_TOWN_DATA, &d, sizeof(d), t->data,
             t->data ? ACNET_TOWN_SIZE : 0, 1);
    if (g_verbose) logf_("[%s] sent town (present=%d, v%u) to %s", t->invite, d.present, t->version, c->name);
}

static void handle_town_upload(client_t* c, const uint8_t* payload, size_t payload_len,
                               const uint8_t* blob, size_t blob_len) {
    acnet_town_upload_t u;
    acnet_town_ack_t ack;
    uint32_t now = enet_time_get();
    uint32_t v;
    memset(&ack, 0, sizeof(ack));
    if (!c->logged_in) return;
    ack.town_version = g_rooms[c->room].town.version;
    if (payload_len != sizeof(u) || blob_len != ACNET_TOWN_SIZE) {
        ack.status = ACNET_ACK_REJECTED;
        send_msg(c->peer, ACNET_CH_CONTROL, ACNET_MSG_TOWN_ACK, &ack, sizeof(ack), NULL, 0, 1);
        logf_("[%s] rejected malformed upload from %s (payload %zu, blob %zu)",
              g_rooms[c->room].town.invite, c->name, payload_len, blob_len);
        return;
    }
    memcpy(&u, payload, sizeof(u));
    if (c->last_upload_ms && now - c->last_upload_ms < UPLOAD_MIN_INTERVAL_MS && u.reason != ACNET_UPLOAD_LEAVE) {
        ack.status = ACNET_ACK_RATE_LIMIT;
        send_msg(c->peer, ACNET_CH_CONTROL, ACNET_MSG_TOWN_ACK, &ack, sizeof(ack), NULL, 0, 1);
        return;
    }
    c->last_upload_ms = now;
    v = town_apply_upload(&g_rooms[c->room].town, c->slot, blob);
    if (v == 0) {
        ack.status = town_validate_blob(blob, blob_len) ? ACNET_ACK_IO_ERROR : ACNET_ACK_REJECTED;
        send_msg(c->peer, ACNET_CH_CONTROL, ACNET_MSG_TOWN_ACK, &ack, sizeof(ack), NULL, 0, 1);
        logf_("[%s] upload from %s failed (%s)", g_rooms[c->room].town.invite, c->name,
              ack.status == ACNET_ACK_IO_ERROR ? "disk error" : "invalid town blob");
        return;
    }
    ack.town_version = v;
    ack.status = ACNET_ACK_OK;
    ack.by_slot = (uint8_t)c->slot;
    send_msg(c->peer, ACNET_CH_CONTROL, ACNET_MSG_TOWN_ACK, &ack, sizeof(ack), NULL, 0, 1);
    room_broadcast(c->room, c, ACNET_CH_CONTROL, ACNET_MSG_TOWN_VERSION, &ack, sizeof(ack), 1);
    logf_("[%s] town v%u saved by %s (reason %u)", g_rooms[c->room].town.invite, v, c->name, u.reason);
}

static void handle_chat(client_t* c, const uint8_t* payload, size_t len) {
    acnet_chat_t m;
    int i;
    if (!c->logged_in || len != sizeof(m)) return;
    memcpy(&m, payload, sizeof(m));
    if (m.len > ACNET_CHAT_LEN) m.len = ACNET_CHAT_LEN;
    for (i = 0; i < m.len; i++) {
        unsigned char ch = (unsigned char)m.text[i];
        if (ch < 0x20 || ch == 0x7F) m.text[i] = ' ';
    }
    for (i = m.len; i < ACNET_CHAT_LEN; i++) m.text[i] = '\0';
    m.slot = (uint8_t)c->slot;
    room_broadcast(c->room, c, ACNET_CH_CHAT, ACNET_MSG_CHAT, &m, sizeof(m), 1);
    if (g_verbose) logf_("[%s] <%s> %.*s", g_rooms[c->room].town.invite, c->name, (int)m.len, m.text);
}

static void handle_player_state(client_t* c, const uint8_t* payload, size_t len) {
    acnet_player_state_t s;
    if (!c->logged_in || len != sizeof(s)) return;
    memcpy(&s, payload, sizeof(s));
    s.client_id = (uint8_t)c->id;
    s.slot = (uint8_t)c->slot;
    room_broadcast(c->room, c, ACNET_CH_STATE, ACNET_MSG_PLAYER_STATE, &s, sizeof(s), 0);
}

static void handle_ping(client_t* c, const uint8_t* payload, size_t len) {
    acnet_ping_t p;
    acnet_pong_t r;
    if (len != sizeof(p)) return;
    memcpy(&p, payload, sizeof(p));
    r.nonce = p.nonce;
    r.server_unix_ms = unix_ms();
    send_msg(c->peer, ACNET_CH_CONTROL, ACNET_MSG_PONG, &r, sizeof(r), NULL, 0, 1);
}

static void handle_packet(client_t* c, const uint8_t* data, size_t len) {
    acnet_hdr_t hdr;
    const uint8_t* payload;
    size_t payload_len, blob_len;
    if (len < sizeof(hdr)) return;
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.version != ACNET_PROTOCOL_VERSION) {
        if (!c->logged_in) send_reject(c->peer, ACNET_REJECT_BAD_VERSION, "protocol version mismatch");
        return;
    }
    payload = data + sizeof(hdr);
    payload_len = hdr.payload_len;
    if (sizeof(hdr) + payload_len > len) return;
    blob_len = len - sizeof(hdr) - payload_len;

    switch (hdr.type) {
    case ACNET_MSG_HELLO:        handle_hello(c, payload, payload_len); break;
    case ACNET_MSG_TOWN_REQUEST: handle_town_request(c); break;
    case ACNET_MSG_TOWN_UPLOAD:  handle_town_upload(c, payload, payload_len, payload + payload_len, blob_len); break;
    case ACNET_MSG_CHAT:         handle_chat(c, payload, payload_len); break;
    case ACNET_MSG_PLAYER_STATE: handle_player_state(c, payload, payload_len); break;
    case ACNET_MSG_PING:         handle_ping(c, payload, payload_len); break;
    default:
        if (g_verbose) logf_("client %d sent unknown message type %u", c->id, hdr.type);
        break;
    }
}

/* ------------------------------------------------------------ lifecycle */

static client_t* client_alloc(ENetPeer* peer) {
    static int next_id = 1;
    int i, tries;
    for (i = 0; i < ACNET_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) break;
    }
    if (i == ACNET_MAX_CLIENTS) return NULL;
    for (tries = 0; tries < 256; tries++) {
        int id = next_id;
        next_id = next_id >= 255 ? 1 : next_id + 1;
        if (!client_by_id(id)) {
            memset(&g_clients[i], 0, sizeof(g_clients[i]));
            g_clients[i].in_use = 1;
            g_clients[i].id = id;
            g_clients[i].room = -1;
            g_clients[i].peer = peer;
            return &g_clients[i];
        }
    }
    return NULL;
}

static void client_release(client_t* c) {
    if (c->logged_in) {
        acnet_peer_t left;
        int room = c->room;
        memset(&left, 0, sizeof(left));
        left.client_id = (uint8_t)c->id;
        left.slot = (uint8_t)c->slot;
        memcpy(left.name, c->name, ACNET_NAME_LEN);
        c->logged_in = 0;
        room_broadcast(room, c, ACNET_CH_CONTROL, ACNET_MSG_PEER_LEFT, &left, sizeof(left), 1);
        logf_("[%s] %s left, %d online", g_rooms[room].town.invite, c->name, room_logged_in_count(room));
        room_elect_authority(room);
    }
    memset(c, 0, sizeof(*c));
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(const char* argv0) {
    printf("usage: %s [--port N] [--data DIR] [--verbose]\n"
           "  --port N     UDP port to listen on (default %d, env ACNET_PORT)\n"
           "  --data DIR   data directory holding invites.txt and towns/ (default ./data, env ACNET_DATA)\n"
           "  --verbose    log every town transfer and chat line\n",
           argv0, ACNET_DEFAULT_PORT);
}

int main(int argc, char** argv) {
    int port = ACNET_DEFAULT_PORT;
    ENetAddress address;
    ENetHost* host;
    int i;

    /* Seed rand(): the invite-code fallback uses it when /dev/urandom is
     * absent (e.g. on Windows). */
    srand((unsigned)(time(NULL) ^ (uintptr_t)&host));

    if (getenv("ACNET_PORT")) port = atoi(getenv("ACNET_PORT"));
    if (getenv("ACNET_DATA")) snprintf(g_data_root, sizeof(g_data_root), "%s", getenv("ACNET_DATA"));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            snprintf(g_data_root, sizeof(g_data_root), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else {
            usage(argv[0]);
            return strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ? 0 : 2;
        }
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port %d\n", port);
        return 2;
    }
    if (enet_initialize() != 0) {
        fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }
    {
        char towns_dir[600];
        snprintf(towns_dir, sizeof(towns_dir), "%s/towns", g_data_root);
        /* town_open creates nested dirs; make the root now so invites.txt can be written */
        town_t probe;
        if (town_open(&probe, g_data_root, "_probe") != 0) {
            fprintf(stderr, "cannot create data directory under %s\n", g_data_root);
            return 1;
        }
        town_close(&probe);
        (void)towns_dir;
    }
    load_invites();

    address.host = ENET_HOST_ANY;
    address.port = (enet_uint16)port;
    host = enet_host_create(&address, ACNET_MAX_CLIENTS, ACNET_CHANNELS, 0, 0);
    if (!host) {
        fprintf(stderr, "could not bind UDP port %d\n", port);
        return 1;
    }
    signal(SIGINT, on_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_signal);
#endif
    logf_("town server listening on UDP %d, data in %s", port, g_data_root);

    while (!g_stop) {
        ENetEvent ev;
        int rc = enet_host_service(host, &ev, 50);
        if (rc < 0) {
            logf_("enet_host_service error");
            break;
        }
        if (rc == 0) continue;
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            client_t* c = client_alloc(ev.peer);
            char ip[64] = "?";
            enet_address_get_host_ip(&ev.peer->address, ip, sizeof(ip));
            if (!c) {
                logf_("connection from %s refused: server full", ip);
                enet_peer_disconnect(ev.peer, 0);
                break;
            }
            ev.peer->data = c;
            enet_peer_timeout(ev.peer, 0, 5000, 15000);
            if (g_verbose) logf_("client %d connected from %s", c->id, ip);
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            client_t* c = (client_t*)ev.peer->data;
            if (c) handle_packet(c, ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            client_t* c = (client_t*)ev.peer->data;
            if (c) client_release(c);
            ev.peer->data = NULL;
            break;
        }
        default:
            break;
        }
    }

    logf_("shutting down");
    for (i = 0; i < ACNET_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].peer) enet_peer_disconnect(g_clients[i].peer, 0);
    }
    enet_host_flush(host);
    enet_host_destroy(host);
    for (i = 0; i < MAX_TOWNS; i++) {
        if (g_rooms[i].in_use) town_close(&g_rooms[i].town);
    }
    enet_deinitialize();
    return 0;
}
