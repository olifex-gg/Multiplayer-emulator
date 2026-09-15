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
        printf("WELCOME client_id=%u slot=%u authority=%u town_present=%u version=%u peers=%u server_ms=%lld\n",
               w.client_id, w.slot, w.authority_client_id, w.town_present, w.town_version, w.peer_count,
               (long long)w.server_unix_ms);
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
        printf("PONG nonce=%u server_ms=%lld\n", p.nonce, (long long)p.server_unix_ms);
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
        printf("STATE client_id=%u slot=%u seq=%u area=%u pos=%.1f,%.1f,%.1f\n", s.client_id, s.slot, s.seq,
               s.area, s.x, s.y, s.z);
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

    if (state) {
        acnet_player_state_t ps;
        memset(&ps, 0, sizeof(ps));
        sscanf(state, "%f,%f,%f", &ps.x, &ps.y, &ps.z);
        ps.seq = 1;
        ps.anim_index = 7;
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
        while ((int32_t)(end - enet_time_get()) > 0) {
            rc = wait_for(host, peer, 0, end - enet_time_get(), NULL);
            if (rc < 0) break;
        }
    }

done:
    enet_peer_disconnect(peer, 0);
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
