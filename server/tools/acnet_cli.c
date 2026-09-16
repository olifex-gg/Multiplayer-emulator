/* acnet_cli.c - command-line client for the town server.
 *
 * Used by the end-to-end tests and handy for operators: log in, download or
 * upload a town, send chat, and print every event the server sends as one
 * machine-readable line. Exit codes: 0 ok, 1 rejected or failed, 2 usage,
 * 3 timeout.
 */
#include <enet/enet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

static int g_quiet;
static int g_my_slot = -1; /* from WELCOME, for --push */

static int send_msg(ENetPeer* peer, uint8_t channel, uint8_t type, const void* payload,
                    size_t payload_len, const void* blob, size_t blob_len, int reliable) {
    ENetPacket* pkt = enet_packet_create(NULL, sizeof(acnet_hdr_t) + payload_len + blob_len,
                                         reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    acnet_hdr_t hdr;
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

static void print_name(const char* label, const char* name16) {
    char tmp[ACNET_NAME_LEN + 1];
    memcpy(tmp, name16, ACNET_NAME_LEN);
    tmp[ACNET_NAME_LEN] = '\0';
    printf("%s=%s", label, tmp);
}

/* Print an incoming packet. Returns its type, or 0 if malformed. */
static int describe(const ENetPacket* pkt, const char* save_town_to) {
    acnet_hdr_t hdr;
    const uint8_t* payload;
    size_t payload_len, blob_len;
    if (pkt->dataLength < sizeof(hdr)) return 0;
    memcpy(&hdr, pkt->data, sizeof(hdr));
    payload = pkt->data + sizeof(hdr);
    payload_len = hdr.payload_len;
    if (sizeof(hdr) + payload_len > pkt->dataLength) return 0;
    blob_len = pkt->dataLength - sizeof(hdr) - payload_len;

    switch (hdr.type) {
    case ACNET_MSG_STATUS_REPLY: {
        acnet_status_reply_t r;
        int i;
        if (payload_len != sizeof(r)) return 0;
        memcpy(&r, payload, sizeof(r));
        printf("STATUS room_known=%u town_present=%u version=%u\n", r.room_known, r.town_present,
               r.town_version);
        for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
            char nm[ACNET_NAME_LEN + 1];
            memcpy(nm, r.slots[i].name, ACNET_NAME_LEN);
            nm[ACNET_NAME_LEN] = '\0';
            printf("SLOT %d %s %s\n", i, nm[0] ? nm : "-", r.slots[i].online ? "online" : "away");
        }
        return ACNET_MSG_STATUS_REPLY;
    }
    case ACNET_MSG_WELCOME: {
        acnet_welcome_t w;
        int i;
        if (payload_len != sizeof(w)) return 0;
        memcpy(&w, payload, sizeof(w));
        printf("WELCOME client_id=%u slot=%u authority=%u town_present=%u version=%u peers=%u server_ms=%lld tz=%d\n",
               w.client_id, w.slot, w.authority_client_id, w.town_present, w.town_version, w.peer_count,
               (long long)w.server_unix_ms, (int)w.server_tz_min);
        g_my_slot = w.slot;
        for (i = 0; i < w.peer_count && i < ACNET_MAX_PLAYERS; i++) {
            printf("PEER client_id=%u slot=%u ", w.peers[i].client_id, w.peers[i].slot);
            print_name("name", w.peers[i].name);
            printf("\n");
        }
        break;
    }
    case ACNET_MSG_REJECT: {
        acnet_reject_t r;
        if (payload_len != sizeof(r)) return 0;
        memcpy(&r, payload, sizeof(r));
        r.text[sizeof(r.text) - 1] = '\0';
        printf("REJECT reason=%u text=%s\n", r.reason, r.text);
        break;
    }
    case ACNET_MSG_TOWN_DATA: {
        acnet_town_data_t d;
        if (payload_len != sizeof(d)) return 0;
        memcpy(&d, payload, sizeof(d));
        printf("TOWN present=%u version=%u size=%zu", d.present, d.town_version, blob_len);
        if (d.present && blob_len == ACNET_TOWN_SIZE && save_town_to) {
            FILE* fp = fopen(save_town_to, "wb");
            if (fp && fwrite(payload + payload_len, 1, blob_len, fp) == blob_len) {
                printf(" saved=%s", save_town_to);
            } else {
                printf(" saved=FAILED");
            }
            if (fp) fclose(fp);
        }
        printf("\n");
        break;
    }
    case ACNET_MSG_TOWN_ACK:
    case ACNET_MSG_TOWN_VERSION: {
        acnet_town_ack_t a;
        if (payload_len != sizeof(a)) return 0;
        memcpy(&a, payload, sizeof(a));
        if (hdr.type == ACNET_MSG_TOWN_ACK) {
            printf("ACK status=%u version=%u\n", a.status, a.town_version);
        } else {
            printf("VERSION version=%u by_slot=%u\n", a.town_version, a.by_slot);
        }
        break;
    }
    case ACNET_MSG_PEER_JOINED:
    case ACNET_MSG_PEER_LEFT: {
        acnet_peer_t p;
        if (payload_len != sizeof(p)) return 0;
        memcpy(&p, payload, sizeof(p));
        printf("%s client_id=%u slot=%u ", hdr.type == ACNET_MSG_PEER_JOINED ? "JOIN" : "LEFT", p.client_id, p.slot);
        print_name("name", p.name);
        printf("\n");
        break;
    }
    case ACNET_MSG_AUTHORITY: {
        acnet_authority_t a;
        if (payload_len != sizeof(a)) return 0;
        memcpy(&a, payload, sizeof(a));
        printf("AUTHORITY client_id=%u\n", a.client_id);
        break;
    }
    case ACNET_MSG_SLOT: {
        acnet_slot_t r;
        if (payload_len != sizeof(r)) return 0;
        memcpy(&r, payload, sizeof(r));
        printf("SLOT slot=%u reason=%u\n", r.slot, r.reason);
        break;
    }
    case ACNET_MSG_WEATHER: {
        acnet_weather_t w;
        if (payload_len != sizeof(w)) return 0;
        memcpy(&w, payload, sizeof(w));
        printf("WEATHER type=%d intensity=%d\n", w.type, w.intensity);
        break;
    }
    case ACNET_MSG_NPC_STATE: {
        acnet_npc_hdr_t h;
        int i;
        if (payload_len < sizeof(h)) return 0;
        memcpy(&h, payload, sizeof(h));
        for (i = 0; i < h.count && sizeof(h) + (i + 1) * sizeof(acnet_npc_state_t) <= payload_len; i++) {
            acnet_npc_state_t s;
            memcpy(&s, payload + sizeof(h) + i * sizeof(s), sizeof(s));
            printf("NPC client_id=%u npc=%u pos=%.1f,%.1f,%.1f angle=%d walking=%u act=%u flags=%u\n", h.client_id,
                   s.npc_id, s.x, s.y, s.z, s.angle_y, s.walking, s.act, s.flags);
        }
        break;
    }
    case ACNET_MSG_CHAT: {
        acnet_chat_t m;
        if (payload_len != sizeof(m)) return 0;
        memcpy(&m, payload, sizeof(m));
        if (m.len > ACNET_CHAT_LEN) m.len = ACNET_CHAT_LEN;
        printf("CHAT slot=%u text=%.*s\n", m.slot, (int)m.len, m.text);
        break;
    }
    case ACNET_MSG_PONG: {
        acnet_pong_t p;
        if (payload_len != sizeof(p)) return 0;
        memcpy(&p, payload, sizeof(p));
        printf("PONG nonce=%u server_ms=%lld tz=%d\n", p.nonce, (long long)p.server_unix_ms, (int)p.server_tz_min);
        break;
    }
    case ACNET_MSG_LAND_CELLS: {
        acnet_land_hdr_t h;
        int i;
        if (payload_len < sizeof(h)) return 0;
        memcpy(&h, payload, sizeof(h));
        printf("LAND count=%u", h.count);
        for (i = 0; i < h.count && sizeof(h) + (i + 1) * sizeof(acnet_land_cell_t) <= payload_len; i++) {
            acnet_land_cell_t c;
            memcpy(&c, payload + sizeof(h) + i * sizeof(c), sizeof(c));
            printf(" cell=%u,%u,%u,%u,%u", c.fx, c.fz, c.utx, c.utz, c.item);
        }
        printf("\n");
        break;
    }
    case ACNET_MSG_RESIDENT_DATA: {
        acnet_resident_data_t r;
        if (payload_len != sizeof(r)) return 0;
        memcpy(&r, payload, sizeof(r));
        printf("RESIDENT slot=%u version=%u bytes=%zu\n", r.slot, r.town_version, blob_len);
        break;
    }
    case ACNET_MSG_PLAYER_STATE: {
        acnet_player_state_t s;
        if (payload_len != sizeof(s)) return 0;
        memcpy(&s, payload, sizeof(s));
        printf("STATE client_id=%u slot=%u seq=%u area=%u pos=%.1f,%.1f,%.1f anim=%d/%d part=%d frame=%.1f/%.1f speed=%.2f flags=%u item=%u talk=%u mode=%u/%u show=%u\n",
               s.client_id, s.slot, s.seq, s.area, s.x, s.y, s.z, s.anim0_idx, s.anim1_idx, s.part_table_idx,
               s.anim0_frame, s.anim1_frame, s.anim_speed, s.flags, s.item, s.talk_npc, s.anim0_mode, s.anim1_mode,
               s.show_item);
        break;
    }
    default:
        printf("UNKNOWN type=%u\n", hdr.type);
        break;
    }
    fflush(stdout);
    return hdr.type;
}

/* Service the connection until a packet of type `want` arrives (or any
 * packet when want == 0), printing everything on the way. Returns the type
 * received, -1 on disconnect, 0 on timeout. */
static int wait_for(ENetHost* host, ENetPeer* peer, int want, uint32_t timeout_ms, const char* save_town_to) {
    uint32_t deadline = enet_time_get() + timeout_ms;
    for (;;) {
        ENetEvent ev;
        uint32_t now = enet_time_get();
        int rc;
        if ((int32_t)(deadline - now) <= 0) return 0;
        rc = enet_host_service(host, &ev, deadline - now);
        if (rc < 0) return -1;
        if (rc == 0) continue;
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            printf("DISCONNECTED\n");
            return -1;
        }
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            int t = describe(ev.packet, save_town_to);
            enet_packet_destroy(ev.packet);
            if (t == ACNET_MSG_REJECT) return t;
            if (want == 0 || t == want) return t;
        }
        (void)peer;
    }
}

static uint8_t* read_file(const char* path, size_t* out_len) {
    FILE* fp = fopen(path, "rb");
    uint8_t* buf;
    long n;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) {
        fclose(fp);
        return NULL;
    }
    buf = (uint8_t*)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_len = (size_t)n;
    return buf;
}

static void usage(void) {
    fprintf(stderr,
            "usage: acnet_cli --server HOST --invite CODE --name NAME [--port N] [--slot N]\n"
            "                 [--download FILE] [--upload FILE] [--reason save|leave|periodic|new]\n"
            "                 [--chat TEXT] [--ping] [--wait SECS] [--quiet]\n"
            "       acnet_cli --server HOST --invite CODE --status   (lobby query, no login)\n"
            "       ... --state X,Y,Z      send one player-state packet after login\n"
            "       ... --anim N           animation index to put in it (default: standing still)\n"
            "       ... --area N           scene id to put in it (default: the outdoor town)\n"
            "       ... --claim N          say our character is in save block N; prints the slot we end up in\n"
            "       ... --weather T,I      send the town weather (type, intensity); with --wait, once a second\n"
            "       ... --vanish           exit without saying goodbye (a crashed game), after everything else\n"
            "       ... --npc ID,X,Z[,ACT,FLAGS]  send one villager state (we own villager ID at X,Z)\n"
            "       ... --item N            hold item kind N in the --state (0 = empty hands)\n"
            "       ... --anim-cycle N      with --state-every: a different animation each send, 0..N-1\n"
            "       ... --anim-speed S      animation speed in the --state (default 0: a held pose)\n"
            "       ... --anim-once         play the animation once and hold its last frame (default: loop)\n"
            "       ... --push TOWN.gci     push our own two blocks out of TOWN.gci without saving\n"
            "       ... --state-every MS   with --state and --wait: keep resending it every MS,\n"
            "                              so a running game keeps drawing this fake resident\n"
            "       ... --land FX,FZ,UTX,UTZ,ITEM   send one changed land cell after login\n");
}

int main(int argc, char** argv) {
    const char* server = NULL;
    const char* invite = NULL;
    const char* name = NULL;
    const char* download = NULL;
    const char* upload = NULL;
    const char* chat = NULL;
    const char* state = NULL;
    const char* land = NULL;
    int port = ACNET_DEFAULT_PORT, slot = ACNET_SLOT_ANY, wait_secs = 0, do_ping = 0, do_status = 0;
    int state_every_ms = 0;
    int anim_idx = ACNET_PLAYER_ANIM_WAIT;
    int area = ACNET_AREA_FIELD;
    int claim = -1;
    const char* weather = NULL;
    acnet_weather_t wx;
    int vanish = 0;
    const char* npc = NULL;
    const char* push = NULL;
    int item = 0;
    int anim_cycle = 0, cycle_i = 0;
    float anim_speed = 0.0f;
    int anim_once = 0;
    acnet_player_state_t ps;
    uint8_t reason = ACNET_UPLOAD_SAVE;
    ENetHost* host;
    ENetPeer* peer;
    ENetAddress addr;
    ENetEvent ev;
    int i, rc, exit_code = 0;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--server") == 0 && v) { server = v; i++; }
        else if (strcmp(a, "--port") == 0 && v) { port = atoi(v); i++; }
        else if (strcmp(a, "--invite") == 0 && v) { invite = v; i++; }
        else if (strcmp(a, "--name") == 0 && v) { name = v; i++; }
        else if (strcmp(a, "--slot") == 0 && v) { slot = atoi(v); i++; }
        else if (strcmp(a, "--download") == 0 && v) { download = v; i++; }
        else if (strcmp(a, "--upload") == 0 && v) { upload = v; i++; }
        else if (strcmp(a, "--chat") == 0 && v) { chat = v; i++; }
        else if (strcmp(a, "--state") == 0 && v) { state = v; i++; }
        else if (strcmp(a, "--state-every") == 0 && v) { state_every_ms = atoi(v); i++; }
        else if (strcmp(a, "--anim") == 0 && v) { anim_idx = atoi(v); i++; }
        else if (strcmp(a, "--area") == 0 && v) { area = atoi(v); i++; }
        else if (strcmp(a, "--claim") == 0 && v) { claim = atoi(v); i++; }
        else if (strcmp(a, "--weather") == 0 && v) { weather = v; i++; }
        else if (strcmp(a, "--vanish") == 0) { vanish = 1; }
        else if (strcmp(a, "--npc") == 0 && v) { npc = v; i++; }
        else if (strcmp(a, "--push") == 0 && v) { push = v; i++; }
        else if (strcmp(a, "--item") == 0 && v) { item = atoi(v); i++; }
        else if (strcmp(a, "--anim-cycle") == 0 && v) { anim_cycle = atoi(v); i++; }
        else if (strcmp(a, "--anim-speed") == 0 && v) { anim_speed = (float)atof(v); i++; }
        else if (strcmp(a, "--anim-once") == 0) { anim_once = 1; }
        else if (strcmp(a, "--land") == 0 && v) { land = v; i++; }
        else if (strcmp(a, "--wait") == 0 && v) { wait_secs = atoi(v); i++; }
        else if (strcmp(a, "--ping") == 0) { do_ping = 1; }
        else if (strcmp(a, "--status") == 0) { do_status = 1; }
        else if (strcmp(a, "--quiet") == 0) { g_quiet = 1; }
        else if (strcmp(a, "--reason") == 0 && v) {
            i++;
            if (strcmp(v, "save") == 0) reason = ACNET_UPLOAD_SAVE;
            else if (strcmp(v, "leave") == 0) reason = ACNET_UPLOAD_LEAVE;
            else if (strcmp(v, "periodic") == 0) reason = ACNET_UPLOAD_PERIODIC;
            else if (strcmp(v, "new") == 0) reason = ACNET_UPLOAD_NEW_TOWN;
            else { usage(); return 2; }
        } else { usage(); return 2; }
    }
    if (!server || !invite || (!name && !do_status)) {
        usage();
        return 2;
    }
    if (enet_initialize() != 0) return 1;
    host = enet_host_create(NULL, 1, ACNET_CHANNELS, 0, 0);
    if (!host) return 1;
    if (enet_address_set_host(&addr, server) != 0) {
        fprintf(stderr, "cannot resolve %s\n", server);
        return 1;
    }
    addr.port = (enet_uint16)port;
    peer = enet_host_connect(host, &addr, ACNET_CHANNELS, 0);
    if (!peer) return 1;
    rc = enet_host_service(host, &ev, 5000);
    if (rc <= 0 || ev.type != ENET_EVENT_TYPE_CONNECT) {
        printf("CONNECT_TIMEOUT\n");
        return 3;
    }
    if (!g_quiet) printf("CONNECTED\n");

    if (do_status) {
        acnet_status_request_t q;
        memset(&q, 0, sizeof(q));
        snprintf(q.invite, sizeof(q.invite), "%s", invite);
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_STATUS_REQUEST, &q, sizeof(q), NULL, 0, 1);
        rc = wait_for(host, peer, ACNET_MSG_STATUS_REPLY, 5000, NULL);
        exit_code = rc == ACNET_MSG_STATUS_REPLY ? 0 : 3;
        goto done;
    }

    {
        acnet_hello_t h;
        memset(&h, 0, sizeof(h));
        snprintf(h.invite, sizeof(h.invite), "%s", invite);
        snprintf(h.name, sizeof(h.name), "%s", name);
        h.client_build = 1;
        h.want_slot = (uint8_t)slot;
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_HELLO, &h, sizeof(h), NULL, 0, 1);
    }
    rc = wait_for(host, peer, ACNET_MSG_WELCOME, 5000, NULL);
    if (rc == ACNET_MSG_REJECT) { exit_code = 1; goto done; }
    if (rc <= 0) { exit_code = 3; goto done; }

    if (claim >= 0) {
        acnet_claim_slot_t q;
        memset(&q, 0, sizeof(q));
        q.player_no = (uint8_t)claim;
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_CLAIM_SLOT, &q, sizeof(q), NULL, 0, 1);
        rc = wait_for(host, peer, ACNET_MSG_SLOT, 5000, NULL);
        if (rc <= 0) { exit_code = 3; goto done; }
    }
    if (npc) {
        struct { acnet_npc_hdr_t h; acnet_npc_state_t s; } ACNET_PACKED pk;
        int id = 0, act = ACNET_NPC_ACT_NONE, flags = 0;
        memset(&pk, 0, sizeof(pk));
        sscanf(npc, "%d,%f,%f,%d,%d", &id, &pk.s.x, &pk.s.z, &act, &flags);
        pk.h.count = 1;
        pk.s.npc_id = (uint16_t)id;
        pk.s.act = (uint8_t)act;
        pk.s.flags = (uint8_t)flags;
        send_msg(peer, ACNET_CH_STATE, ACNET_MSG_NPC_STATE, &pk, sizeof(pk), NULL, 0, 0);
        enet_host_flush(host);
    }
    memset(&wx, 0, sizeof(wx));
    if (weather) {
        int t = 0, in = 0;
        sscanf(weather, "%d,%d", &t, &in);
        wx.type = (int16_t)t;
        wx.intensity = (int16_t)in;
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_WEATHER, &wx, sizeof(wx), NULL, 0, 1);
        enet_host_flush(host);
    }
    memset(&ps, 0, sizeof(ps));
    if (state) {
        sscanf(state, "%f,%f,%f", &ps.x, &ps.y, &ps.z);
        ps.seq = 1;
        /* Frame 1 of the animation at speed 0: a clean held pose, since this
         * fake resident never advances it. */
        ps.area = (uint32_t)area;
        ps.anim0_idx = (int16_t)anim_idx;
        ps.anim1_idx = (int16_t)anim_idx;
        ps.part_table_idx = 0;
        ps.anim0_frame = 1.0f;
        ps.anim1_frame = 1.0f;
        ps.anim_speed = anim_speed;
        ps.anim0_mode = ps.anim1_mode = ps.item_mode = anim_once ? ACNET_ANIM_MODE_STOP : ACNET_ANIM_MODE_REPEAT;
        ps.item = (uint16_t)item;
        ps.item_anim = -1;
        ps.item_frame = 1.0f;
        send_msg(peer, ACNET_CH_STATE, ACNET_MSG_PLAYER_STATE, &ps, sizeof(ps), NULL, 0, 0);
        enet_host_flush(host);
    }
    if (land) {
        struct { acnet_land_hdr_t h; acnet_land_cell_t c; } ACNET_PACKED pk;
        unsigned fx = 0, fz = 0, utx = 0, utz = 0, item = 0;
        memset(&pk, 0, sizeof(pk));
        sscanf(land, "%u,%u,%u,%u,%u", &fx, &fz, &utx, &utz, &item);
        pk.h.count = 1;
        pk.c.fx = (uint8_t)fx; pk.c.fz = (uint8_t)fz; pk.c.utx = (uint8_t)utx; pk.c.utz = (uint8_t)utz;
        pk.c.item = (uint16_t)item;
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_LAND_CELLS, &pk, sizeof(pk), NULL, 0, 1);
        enet_host_flush(host);
    }
    if (download) {
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_TOWN_REQUEST, NULL, 0, NULL, 0, 1);
        rc = wait_for(host, peer, ACNET_MSG_TOWN_DATA, 10000, download);
        if (rc <= 0) { exit_code = 3; goto done; }
    }
    if (upload) {
        size_t len = 0;
        uint8_t* blob = read_file(upload, &len);
        acnet_town_upload_t u;
        if (!blob) {
            fprintf(stderr, "cannot read %s\n", upload);
            exit_code = 1;
            goto done;
        }
        memset(&u, 0, sizeof(u));
        u.reason = reason;
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_TOWN_UPLOAD, &u, sizeof(u), blob, len, 1);
        free(blob);
        rc = wait_for(host, peer, ACNET_MSG_TOWN_ACK, 10000, NULL);
        if (rc <= 0) { exit_code = 3; goto done; }
    }
    if (push) {
        size_t len = 0;
        uint8_t* town = read_file(push, &len);
        acnet_resident_push_t q;
        uint8_t blocks[ACNET_RESIDENT_BLOB_SIZE];
        if (!town || len != ACNET_TOWN_SIZE || g_my_slot < 0 || g_my_slot >= ACNET_MAX_PLAYERS) {
            fprintf(stderr, "cannot push from %s (need a %u-byte town and a slot)\n", push, (unsigned)ACNET_TOWN_SIZE);
            free(town);
            exit_code = 1;
            goto done;
        }
        memcpy(blocks, town + ACNET_PRIVATE_OFFSET(g_my_slot), ACNET_PRIVATE_SIZE);
        memcpy(blocks + ACNET_PRIVATE_SIZE, town + ACNET_HOME_OFFSET(g_my_slot), ACNET_HOME_SIZE);
        free(town);
        memset(&q, 0, sizeof(q));
        q.slot = (uint8_t)g_my_slot;
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_RESIDENT_PUSH, &q, sizeof(q), blocks, sizeof(blocks), 1);
        enet_host_flush(host);
        printf("PUSHED slot=%d\n", g_my_slot);
    }
    if (chat) {
        acnet_chat_t m;
        memset(&m, 0, sizeof(m));
        m.len = (uint8_t)(strlen(chat) > ACNET_CHAT_LEN ? ACNET_CHAT_LEN : strlen(chat));
        memcpy(m.text, chat, m.len);
        send_msg(peer, ACNET_CH_CHAT, ACNET_MSG_CHAT, &m, sizeof(m), NULL, 0, 1);
        enet_host_flush(host);
    }
    if (do_ping) {
        acnet_ping_t p;
        p.nonce = 0xC0FFEE;
        send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_PING, &p, sizeof(p), NULL, 0, 1);
        rc = wait_for(host, peer, ACNET_MSG_PONG, 5000, NULL);
        if (rc <= 0) { exit_code = 3; goto done; }
    }
    if (wait_secs > 0) {
        uint32_t end = enet_time_get() + (uint32_t)wait_secs * 1000u;
        uint32_t next_state = enet_time_get();
        uint32_t next_weather = enet_time_get() + 1000u;
        while ((int32_t)(end - enet_time_get()) > 0) {
            uint32_t now = enet_time_get();
            uint32_t slice = end - now;
            /* Repeat the weather once a second so a listener who logs in
             * after us still gets it (only the authority's copy is relayed). */
            if (weather) {
                if ((int32_t)(next_weather - now) <= 0) {
                    send_msg(peer, ACNET_CH_CONTROL, ACNET_MSG_WEATHER, &wx, sizeof(wx), NULL, 0, 1);
                    next_weather = now + 1000u;
                }
                if (next_weather - now < slice) slice = next_weather - now;
            }
            /* A game forgets a resident whose state is older than a few
             * seconds, so a fake resident has to keep talking to stay drawn. */
            if (state && state_every_ms > 0) {
                if ((int32_t)(next_state - now) <= 0) {
                    ps.seq++;
                    if (anim_cycle > 0) { /* sweep: a new animation every send, from its first frame */
                        ps.anim0_idx = ps.anim1_idx = (int16_t)(cycle_i++ % anim_cycle);
                        ps.anim0_frame = ps.anim1_frame = 1.0f;
                        printf("ANIM %d\n", ps.anim0_idx);
                        fflush(stdout);
                    }
                    send_msg(peer, ACNET_CH_STATE, ACNET_MSG_PLAYER_STATE, &ps, sizeof(ps), NULL, 0, 0);
                    next_state = now + (uint32_t)state_every_ms;
                }
                if (next_state - now < slice) slice = next_state - now;
            }
            rc = wait_for(host, peer, 0, slice, NULL);
            if (rc < 0) break;
        }
    }

done:
    if (vanish) {
        printf("VANISHED\n");
        fflush(stdout);
        return exit_code; /* no disconnect: the socket just goes quiet, like a crash */
    }
    /* Disconnect only once everything we sent has been acknowledged. A plain
     * enet_peer_disconnect right after a send can put the packet and the
     * DISCONNECT in front of the server in the same service pass, and ENet
     * drops a peer's undelivered packets when it handles the DISCONNECT --
     * which lost the land cell in the e2e suite on a fast machine. */
    enet_peer_disconnect_later(peer, 0);
    {
        uint32_t end = enet_time_get() + 1000;
        while ((int32_t)(end - enet_time_get()) > 0 && enet_host_service(host, &ev, 100) >= 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
    }
    enet_host_destroy(host);
    enet_deinitialize();
    if (!g_quiet) printf("EXIT %d\n", exit_code);
    return exit_code;
}
