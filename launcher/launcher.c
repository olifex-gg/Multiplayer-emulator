/* launcher.c - "Animal Crossing Online" host/join launcher (multiplayer fork).
 *
 * A tiny Win32 GUI so non-technical players never touch a command prompt or a
 * config file. It sits next to AnimalCrossing.exe (and, for hosting, next to
 * acnet_server.exe). The player picks Host or Join, types a name and an invite
 * code, clicks Play; the launcher writes the game's settings.ini [Network]
 * block, starts the local server if hosting, and launches the game.
 *
 * Build (from repo root, with MinGW):
 *   i686-w64-mingw32-gcc -O2 -mwindows launcher/launcher.c \
 *       -o AnimalCrossingOnline.exe -lws2_32 -lshell32 -luser32 -lgdi32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <enet/enet.h>
#include "protocol.h"

#define IDC_HOST   1001
#define IDC_JOIN   1002
#define IDC_NAME   1003
#define IDC_ADDR   1004
#define IDC_CODE   1005
#define IDC_PLAY   1006
#define IDC_QUIT   1007
#define IDC_STATUS 1008
#define IDC_ADDRLBL 1009
#define IDC_LB_ENTER 1010
#define IDC_LB_BACK  1011

static const char* GAME_EXE   = "AnimalCrossing.exe";
static const char* SERVER_EXE = "acnet_server.exe";
static const char* SETTINGS   = "settings.ini";
static const char* LAUNCHER_INI = "launcher.ini";
static const int   SERVER_PORT = 7777;

static HWND g_name, g_addr, g_code, g_status, g_addrlbl, g_host, g_join;
static HWND g_main_wnd;

/* Defined with the rest of the window plumbing below; the waiting room uses it. */
static HWND mk(const char* cls, const char* text, DWORD style, int x, int y, int w, int h,
               HWND parent, int id);
/* The waiting room, opened by the Play button (defined further down). */
static void lobby_open(HWND parent, int is_host, const char* addr, const char* code,
                       const char* name);
static char g_dir[MAX_PATH]; /* folder the launcher (and game) live in */

/* ---- small helpers ------------------------------------------------------ */

static void set_status(const char* s) { SetWindowTextA(g_status, s); }

static void trim(char* s) {
    char* p = s;
    size_t n;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = '\0';
}

static void get_edit(HWND h, char* out, int cap) {
    out[0] = '\0';
    GetWindowTextA(h, out, cap);
    trim(out);
}

/* Case-insensitive substring search; avoids a shlwapi dependency. */
static const char* StrStrIA_compat(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; hay++) {
        if (_strnicmp(hay, needle, nl) == 0) return hay;
    }
    return NULL;
}

/* Directory the launcher executable lives in (with trailing backslash). */
static void compute_dir(void) {
    char path[MAX_PATH];
    char* slash;
    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    lstrcpynA(g_dir, path, sizeof(g_dir));
}

/* True when the launcher is running from a temp folder, which is what happens
 * if someone double-clicks it straight out of a .zip: Windows copies the one
 * file to %TEMP% and runs it there, with nothing else beside it. That is the
 * single most common way this launcher ends up unable to find the game. */
static int running_from_temp(void) {
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) return 0;
    if (StrStrIA_compat(g_dir, tmp) == g_dir) return 1;
    return StrStrIA_compat(g_dir, "\\Temp\\") != NULL;
}

static void path_in_dir(char* out, int cap, const char* leaf) {
    snprintf(out, cap, "%s%s", g_dir, leaf);
}

/* Is there any file in the rom\ subfolder? (the player's disc image) */
static int rom_has_image(void) {
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int found = 0;
    snprintf(pat, sizeof(pat), "%srom\\*.*", g_dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { found = 1; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* Best-effort local IPv4 address to show the host, so friends can connect. */
static void local_ip(char* out, int cap) {
    struct addrinfo hints, *res = NULL, *p;
    char host[256];
    lstrcpynA(out, "your-ip", cap);
    if (gethostname(host, sizeof(host)) != 0) return;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return;
    for (p = res; p != NULL; p = p->ai_next) {
        struct sockaddr_in* sin = (struct sockaddr_in*)p->ai_addr;
        char buf[64];
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            if (strncmp(buf, "127.", 4) != 0) { lstrcpynA(out, buf, cap); break; }
        }
    }
    freeaddrinfo(res);
}

static void gen_code(char* out, int cap) {
    static const char alpha[] = "abcdefghjkmnpqrstuvwxyz23456789";
    int i, n = 6;
    if (cap <= n) n = cap - 1;
    srand((unsigned)GetTickCount() ^ (unsigned)(uintptr_t)out);
    for (i = 0; i < n; i++) out[i] = alpha[rand() % (int)(sizeof(alpha) - 1)];
    out[n] = '\0';
}

/* Rewrite settings.ini: keep everything except a prior [Network] block/keys,
 * then append a fresh one. Preserves the player's graphics/audio settings. */
static int write_settings(int is_host, const char* addr, const char* code, const char* name) {
    char in_path[MAX_PATH], out_path[MAX_PATH];
    FILE* in;
    FILE* out;
    char line[512];
    path_in_dir(in_path, sizeof(in_path), SETTINGS);
    path_in_dir(out_path, sizeof(out_path), "settings.ini.tmp");

    out = fopen(out_path, "wb");
    if (!out) return 0;

    in = fopen(in_path, "rb");
    if (in) {
        int in_network = 0;
        while (fgets(line, sizeof(line), in)) {
            char key[512];
            char* eq;
            lstrcpynA(key, line, sizeof(key));
            trim(key);
            if (key[0] == '[') { in_network = (lstrcmpiA(key, "[Network]") == 0); if (in_network) continue; }
            if (in_network) {
                eq = strchr(key, '=');
                if (eq) {
                    *eq = '\0'; trim(key);
                    if (!lstrcmpiA(key,"server") || !lstrcmpiA(key,"server_port") ||
                        !lstrcmpiA(key,"invite") || !lstrcmpiA(key,"player_name") ||
                        !lstrcmpiA(key,"resident_slot")) continue;
                }
            }
            fputs(line, out);
        }
        fclose(in);
    }

    fprintf(out, "\n[Network]\n");
    fprintf(out, "server = %s\n", is_host ? "127.0.0.1" : addr);
    fprintf(out, "server_port = %d\n", SERVER_PORT);
    fprintf(out, "invite = %s\n", code);
    fprintf(out, "player_name = %s\n", name);
    fclose(out);

    DeleteFileA(in_path);
    return MoveFileA(out_path, in_path) ? 1 : 0;
}

/* Launch a program in the launcher's folder. hidden=1 for the server. */
static int launch(const char* leaf, int hidden) {
    char exe[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    BOOL ok;
    path_in_dir(exe, sizeof(exe), leaf);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return 0;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (hidden) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
    ok = CreateProcessA(exe, NULL, NULL, NULL, FALSE,
                        hidden ? CREATE_NO_WINDOW : 0, NULL, g_dir, &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return ok ? 1 : 0;
}

/* Write serverdata\invites.txt with the chosen code so the local server
 * accepts it, then start the server. */
static int start_local_server(const char* code) {
    char dir[MAX_PATH], inv[MAX_PATH];
    FILE* f;
    path_in_dir(dir, sizeof(dir), "serverdata");
    CreateDirectoryA(dir, NULL);
    path_in_dir(inv, sizeof(inv), "serverdata\\invites.txt");
    f = fopen(inv, "wb");
    if (f) { fprintf(f, "%s\n", code); fclose(f); }
    /* The server reads ACNET_DATA; set it for the child via env of this proc. */
    SetEnvironmentVariableA("ACNET_DATA", "serverdata");
    {
        char portbuf[16];
        snprintf(portbuf, sizeof(portbuf), "%d", SERVER_PORT);
        SetEnvironmentVariableA("ACNET_PORT", portbuf);
    }
    return launch(SERVER_EXE, 1);
}

static void save_prefs(const char* name, const char* addr, const char* code) {
    char ini[MAX_PATH];
    path_in_dir(ini, sizeof(ini), LAUNCHER_INI);
    WritePrivateProfileStringA("last", "name", name, ini);
    WritePrivateProfileStringA("last", "address", addr, ini);
    WritePrivateProfileStringA("last", "code", code, ini);
}

static void load_prefs(void) {
    char ini[MAX_PATH], buf[256];
    path_in_dir(ini, sizeof(ini), LAUNCHER_INI);
    GetPrivateProfileStringA("last", "name", "", buf, sizeof(buf), ini);
    SetWindowTextA(g_name, buf);
    GetPrivateProfileStringA("last", "address", "", buf, sizeof(buf), ini);
    SetWindowTextA(g_addr, buf);
    GetPrivateProfileStringA("last", "code", "", buf, sizeof(buf), ini);
    SetWindowTextA(g_code, buf);
}

static int is_host_mode(void) {
    return SendMessageA(g_host, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void update_mode_ui(void) {
    int host = is_host_mode();
    EnableWindow(g_addr, host ? FALSE : TRUE);
    SetWindowTextA(g_addrlbl, host ? "Your friends connect to your IP (shown after you click Play)"
                                   : "Server address (ask your host):");
    if (host) {
        char code[32];
        get_edit(g_code, code, sizeof(code));
        if (code[0] == '\0') { gen_code(code, sizeof(code)); SetWindowTextA(g_code, code); }
    }
}

/* ---- the Play action ---------------------------------------------------- */

static void on_play(HWND wnd) {
    char name[64], addr[128], code[64], msg[512];
    int host = is_host_mode();

    get_edit(g_name, name, sizeof(name));
    get_edit(g_addr, addr, sizeof(addr));
    get_edit(g_code, code, sizeof(code));

    if (name[0] == '\0') { MessageBoxA(wnd, "Please enter your name.", "Almost there", MB_OK|MB_ICONINFORMATION); return; }
    if (code[0] == '\0') { MessageBoxA(wnd, "Please enter an invite code.", "Almost there", MB_OK|MB_ICONINFORMATION); return; }
    if (!host && addr[0] == '\0') { MessageBoxA(wnd, "Please enter the server address your host gave you.", "Almost there", MB_OK|MB_ICONINFORMATION); return; }

    {
        char game[MAX_PATH];
        path_in_dir(game, sizeof(game), GAME_EXE);
        if (GetFileAttributesA(game) == INVALID_FILE_ATTRIBUTES) {
            char m[900];
            if (running_from_temp()) {
                snprintf(m, sizeof(m),
                         "It looks like you started this launcher from inside a .zip file.\n\n"
                         "Windows only copies the one file you clicked, so the game is not "
                         "beside it.\n\n"
                         "Right-click the .zip, choose \"Extract All\", pick a normal folder "
                         "(your Desktop is fine), then run AnimalCrossingOnline.exe from the "
                         "folder you extracted.");
            } else {
                snprintf(m, sizeof(m),
                         "AnimalCrossing.exe is not in this folder:\n\n    %s\n\n"
                         "The launcher and the game have to sit in the SAME folder, "
                         "alongside the shaders folder and the rom folder.\n\n"
                         "If you unzipped this, check for a folder inside a folder - the game "
                         "may be one level down. If the game was there and vanished, check "
                         "whether your antivirus quarantined it.", g_dir);
            }
            MessageBoxA(wnd, m, "Cannot find the game", MB_OK | MB_ICONERROR);
            return;
        }
    }
    if (!rom_has_image()) {
        char rom[MAX_PATH];
        path_in_dir(rom, sizeof(rom), "rom");
        CreateDirectoryA(rom, NULL);
        MessageBoxA(wnd, "No game disc image found.\n\n"
                         "Put your Animal Crossing disc image (.iso / .gcm / .ciso) into the 'rom' "
                         "folder next to the game, then click Play again.", "Game data needed", MB_OK|MB_ICONINFORMATION);
        ShellExecuteA(wnd, "open", rom, NULL, NULL, SW_SHOWNORMAL);
        return;
    }

    if (!write_settings(host, addr, code, name)) {
        MessageBoxA(wnd, "Could not write settings.ini (is the folder read-only?).", "Error", MB_OK|MB_ICONERROR);
        return;
    }

    if (host) {
        char ip[64];
        char srv[MAX_PATH];
        path_in_dir(srv, sizeof(srv), SERVER_EXE);
        if (GetFileAttributesA(srv) == INVALID_FILE_ATTRIBUTES) {
            {
                char m[700];
                snprintf(m, sizeof(m),
                         "acnet_server.exe is not in this folder:\n\n    %s\n\n"
                         "Hosting needs it. Ask whoever set up the game for that file, "
                         "or choose Join and let someone else host.", g_dir);
                MessageBoxA(wnd, m, "Cannot host", MB_OK | MB_ICONERROR);
            }
            return;
        }
        if (!start_local_server(code)) {
            MessageBoxA(wnd, "The town server failed to start.", "Cannot host", MB_OK|MB_ICONERROR);
            return;
        }
        (void)ip; (void)msg;
    }

    save_prefs(name, addr, code);
    set_status("");
    lobby_open(wnd, host, addr, code, name);
}

/* ---- waiting room ------------------------------------------------------- */
/* A lobby the player sees before entering the town. It keeps one connection
 * to the town server and polls it with a status request, which the server
 * answers WITHOUT logging us in -- so sitting here claims no resident slot and
 * disturbs nobody already playing. It exists because the game itself fails
 * silently: a wrong address, a wrong invite code or a full town all used to
 * end with the game quietly starting in single-player. */

#define LOBBY_POLL_MS    2000
#define LOBBY_CONNECT_MS 6000

enum { LOBBY_CONNECTING = 0, LOBBY_OK, LOBBY_FAILED };

typedef struct {
    CRITICAL_SECTION cs;
    volatile LONG    stop;
    HWND             wnd;
    char             host[160];
    char             invite[ACNET_INVITE_LEN + 1];
    int              state;
    char             err[160];
    int              have;
    acnet_status_reply_t rep;
} lobby_t;

static lobby_t g_lobby;
static HANDLE  g_lobby_thread;
static HWND    g_lb_wnd, g_lb_conn, g_lb_slot[ACNET_MAX_PLAYERS], g_lb_town, g_lb_note, g_lb_enter;
static int     g_lb_is_host;
static char    g_lb_name[64], g_lb_addr[160], g_lb_code[64];

static void lobby_set_state(lobby_t* L, int state, const char* err) {
    EnterCriticalSection(&L->cs);
    L->state = state;
    lstrcpynA(L->err, err ? err : "", sizeof(L->err));
    if (state != LOBBY_OK) L->have = 0;
    LeaveCriticalSection(&L->cs);
    if (L->wnd) PostMessageA(L->wnd, WM_APP + 1, 0, 0);
}

static void lobby_send_status(lobby_t* L, ENetPeer* peer) {
    unsigned char buf[sizeof(acnet_hdr_t) + sizeof(acnet_status_request_t)];
    acnet_hdr_t h;
    acnet_status_request_t q;
    size_t n;
    ENetPacket* pk;

    memset(&q, 0, sizeof(q));
    n = strlen(L->invite);
    if (n > ACNET_INVITE_LEN) n = ACNET_INVITE_LEN;
    memcpy(q.invite, L->invite, n);

    h.type        = ACNET_MSG_STATUS_REQUEST;
    h.version     = ACNET_PROTOCOL_VERSION;
    h.payload_len = (uint16_t)sizeof(q);
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &q, sizeof(q));

    pk = enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);
    if (pk && enet_peer_send(peer, ACNET_CH_CONTROL, pk) < 0) enet_packet_destroy(pk);
}

static void lobby_on_packet(lobby_t* L, const unsigned char* data, size_t len) {
    acnet_hdr_t h;
    if (len < sizeof(h)) return;
    memcpy(&h, data, sizeof(h));
    if (h.type != ACNET_MSG_STATUS_REPLY) return;
    if (len < sizeof(h) + sizeof(acnet_status_reply_t)) return;
    EnterCriticalSection(&L->cs);
    memcpy(&L->rep, data + sizeof(h), sizeof(L->rep));
    L->have = 1;
    LeaveCriticalSection(&L->cs);
    if (L->wnd) PostMessageA(L->wnd, WM_APP + 1, 0, 0);
}

static DWORD WINAPI lobby_worker(LPVOID arg) {
    lobby_t*  L    = (lobby_t*)arg;
    ENetHost* host = NULL;
    ENetPeer* peer = NULL;
    DWORD     connect_started = 0;
    DWORD     last_poll = 0;

    host = enet_host_create(NULL, 1, ACNET_CHANNELS, 0, 0);
    if (!host) {
        lobby_set_state(L, LOBBY_FAILED, "could not open a network socket");
        return 0;
    }

    while (!L->stop) {
        ENetEvent ev;
        DWORD now;

        if (!peer) {
            ENetAddress addr;
            if (enet_address_set_host(&addr, L->host) != 0) {
                lobby_set_state(L, LOBBY_FAILED, "that server address could not be found");
                Sleep(1500);
                continue;
            }
            addr.port = (enet_uint16)SERVER_PORT;
            peer = enet_host_connect(host, &addr, ACNET_CHANNELS, 0);
            if (!peer) {
                lobby_set_state(L, LOBBY_FAILED, "could not start a connection");
                Sleep(1500);
                continue;
            }
            connect_started = GetTickCount();
            last_poll = 0;
            lobby_set_state(L, LOBBY_CONNECTING, "");
        }

        while (enet_host_service(host, &ev, 100) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                lobby_set_state(L, LOBBY_OK, "");
                last_poll = 0;
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                lobby_on_packet(L, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                peer = NULL;
                lobby_set_state(L, LOBBY_FAILED, "the server closed the connection");
                break;
            }
            if (L->stop) break;
        }
        if (L->stop || !peer) continue;

        now = GetTickCount();
        if (L->state == LOBBY_CONNECTING && now - connect_started > LOBBY_CONNECT_MS) {
            enet_peer_reset(peer);
            peer = NULL;
            lobby_set_state(L, LOBBY_FAILED,
                            "no reply. Check the address, and that the host has clicked Host");
            Sleep(1500);
            continue;
        }
        if (L->state == LOBBY_OK && (last_poll == 0 || now - last_poll >= LOBBY_POLL_MS)) {
            lobby_send_status(L, peer);
            last_poll = now;
        }
    }

    if (peer) enet_peer_disconnect_now(peer, 0);
    enet_host_destroy(host);
    return 0;
}

static void lobby_stop(void) {
    if (!g_lobby_thread) return;
    g_lobby.stop = 1;
    WaitForSingleObject(g_lobby_thread, 3000);
    CloseHandle(g_lobby_thread);
    g_lobby_thread = NULL;
}

static void lobby_refresh(void) {
    lobby_t* L = &g_lobby;
    acnet_status_reply_t r;
    int  state, have, i;
    char err[160], line[320];

    EnterCriticalSection(&L->cs);
    state = L->state;
    have  = L->have;
    r     = L->rep;
    lstrcpynA(err, L->err, sizeof(err));
    LeaveCriticalSection(&L->cs);

    if (state == LOBBY_OK) {
        SetWindowTextA(g_lb_conn, "Connected to the town server.");
    } else if (state == LOBBY_CONNECTING) {
        SetWindowTextA(g_lb_conn, "Contacting the town server...");
    } else {
        snprintf(line, sizeof(line), "Not connected - %s", err[0] ? err : "no reply from the server");
        SetWindowTextA(g_lb_conn, line);
    }
    EnableWindow(g_lb_enter, state == LOBBY_OK ? TRUE : FALSE);

    for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
        char nm[ACNET_NAME_LEN + 1];
        if (state == LOBBY_OK && have && r.room_known) {
            memcpy(nm, r.slots[i].name, ACNET_NAME_LEN);
            nm[ACNET_NAME_LEN] = '\0';
            if (nm[0]) {
                snprintf(line, sizeof(line), "Resident %d:   %s   %s", i + 1, nm,
                         r.slots[i].online ? "(in town now)" : "(away)");
            } else {
                snprintf(line, sizeof(line), "Resident %d:   - empty -", i + 1);
            }
        } else if (state == LOBBY_OK && have) {
            snprintf(line, sizeof(line), "Resident %d:   - empty -", i + 1);
        } else {
            snprintf(line, sizeof(line), "Resident %d:", i + 1);
        }
        SetWindowTextA(g_lb_slot[i], line);
    }

    if (state != LOBBY_OK || !have) {
        SetWindowTextA(g_lb_town, "");
    } else if (!r.room_known) {
        SetWindowTextA(g_lb_town,
                       "This town has not been started yet. Enter and save once to create it.");
    } else if (!r.town_present) {
        SetWindowTextA(g_lb_town,
                       "Nobody has saved yet - the first person to save creates the town.");
    } else {
        snprintf(line, sizeof(line), "The town is ready. Everyone who enters loads the same one.");
        SetWindowTextA(g_lb_town, line);
    }
}

static void lobby_enter_town(HWND wnd) {
    lobby_stop();
    save_prefs(g_lb_name, g_lb_addr, g_lb_code);
    if (!launch(GAME_EXE, 0)) {
        MessageBoxA(wnd, "Could not start the game.", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    PostQuitMessage(0);
}

static LRESULT CALLBACK LobbyProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int y = 12, i;
        char line[320];
        mk("STATIC", "Waiting room", 0, 16, y, 300, 20, wnd, 0);
        y += 24;
        g_lb_conn = mk("STATIC", "Contacting the town server...", 0, 16, y, 420, 20, wnd, 0);
        y += 28;
        snprintf(line, sizeof(line), "Town \"%s\"", g_lb_code);
        mk("STATIC", line, 0, 16, y, 420, 20, wnd, 0);
        y += 24;
        for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
            g_lb_slot[i] = mk("STATIC", "", 0, 28, y, 410, 20, wnd, 0);
            y += 20;
        }
        y += 8;
        g_lb_town = mk("STATIC", "", 0, 16, y, 430, 34, wnd, 0);
        y += 40;
        g_lb_note = mk("STATIC", "", 0, 16, y, 430, 52, wnd, 0);
        y += 58;
        g_lb_enter = mk("BUTTON", "Enter Town", BS_DEFPUSHBUTTON, 16, y, 130, 32, wnd, IDC_LB_ENTER);
        mk("BUTTON", "Back", 0, 316, y, 130, 32, wnd, IDC_LB_BACK);
        EnableWindow(g_lb_enter, FALSE);

        if (g_lb_is_host) {
            char ip[64];
            local_ip(ip, sizeof(ip));
            snprintf(line, sizeof(line),
                     "Friends in your house join with:  %s\n"
                     "Friends elsewhere need your public IP (search \"what is my IP\")\n"
                     "and UDP port %d forwarded to this PC.", ip, SERVER_PORT);
        } else {
            snprintf(line, sizeof(line), "Joining %s", g_lb_addr);
        }
        SetWindowTextA(g_lb_note, line);
        SetTimer(wnd, 1, 1000, NULL);
        return 0;
    }
    case WM_APP + 1:
    case WM_TIMER:
        lobby_refresh();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_LB_ENTER: lobby_enter_town(wnd); return 0;
        case IDC_LB_BACK:
            lobby_stop();
            DestroyWindow(wnd);
            g_lb_wnd = NULL;
            ShowWindow(g_main_wnd, SW_SHOW);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        lobby_stop();
        DestroyWindow(wnd);
        g_lb_wnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

/* Open the waiting room. addr is what the game will connect to. */
static void lobby_open(HWND parent, int is_host, const char* addr, const char* code, const char* name) {
    DWORD tid;

    g_lb_is_host = is_host;
    lstrcpynA(g_lb_name, name, sizeof(g_lb_name));
    lstrcpynA(g_lb_addr, addr, sizeof(g_lb_addr));
    lstrcpynA(g_lb_code, code, sizeof(g_lb_code));

    memset(&g_lobby, 0, sizeof(g_lobby));
    InitializeCriticalSection(&g_lobby.cs);
    lstrcpynA(g_lobby.host, is_host ? "127.0.0.1" : addr, sizeof(g_lobby.host));
    lstrcpynA(g_lobby.invite, code, sizeof(g_lobby.invite));
    g_lobby.state = LOBBY_CONNECTING;

    g_lb_wnd = CreateWindowA("ACOnlineLobby", "Animal Crossing Online - Waiting Room",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 476, 400, NULL, NULL,
                             GetModuleHandle(NULL), NULL);
    if (!g_lb_wnd) return;
    g_lobby.wnd = g_lb_wnd;
    ShowWindow(parent, SW_HIDE);
    ShowWindow(g_lb_wnd, SW_SHOW);
    UpdateWindow(g_lb_wnd);

    g_lobby_thread = CreateThread(NULL, 0, lobby_worker, &g_lobby, 0, &tid);
}

/* ---- window plumbing ---------------------------------------------------- */

static HWND mk(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, HWND parent, int id) {
    return CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                         (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int y = 12;
        mk("STATIC", "Play Animal Crossing online with friends.", 0, 16, y, 420, 20, wnd, 0); y += 30;
        mk("BUTTON", "Mode", BS_GROUPBOX, 12, y, 430, 60, wnd, 0);
        g_host = mk("BUTTON", "Host a new town (others join you)", BS_AUTORADIOBUTTON | WS_GROUP, 24, y+22, 250, 20, wnd, IDC_HOST);
        g_join = mk("BUTTON", "Join a friend's town", BS_AUTORADIOBUTTON, 284, y+22, 150, 20, wnd, IDC_JOIN);
        y += 74;
        mk("STATIC", "Your name:", 0, 16, y+3, 120, 20, wnd, 0);
        g_name = mk("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 140, y, 200, 22, wnd, IDC_NAME); y += 34;
        g_addrlbl = mk("STATIC", "Server address (ask your host):", 0, 16, y+3, 300, 20, wnd, IDC_ADDRLBL);
        g_addr = mk("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 16, y+22, 300, 22, wnd, IDC_ADDR); y += 56;
        mk("STATIC", "Invite code:", 0, 16, y+3, 120, 20, wnd, 0);
        g_code = mk("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 140, y, 200, 22, wnd, IDC_CODE); y += 40;
        mk("BUTTON", "Play", BS_DEFPUSHBUTTON, 16, y, 120, 32, wnd, IDC_PLAY);
        mk("BUTTON", "Quit", 0, 322, y, 120, 32, wnd, IDC_QUIT); y += 42;
        g_status = mk("STATIC", "", 0, 16, y, 426, 40, wnd, IDC_STATUS);
        SendMessageA(g_host, BM_SETCHECK, BST_CHECKED, 0);
        load_prefs();
        update_mode_ui();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_HOST:
        case IDC_JOIN: update_mode_ui(); return 0;
        case IDC_PLAY: on_play(wnd); return 0;
        case IDC_QUIT: PostQuitMessage(0); return 0;
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSA wc;
    HWND wnd;
    MSG m;
    WSADATA wsa;
    (void)prev; (void)cmd;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    compute_dir();

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "ACOnlineLauncher";
    RegisterClassA(&wc);

    wc.lpfnWndProc = LobbyProc;
    wc.lpszClassName = "ACOnlineLobby";
    RegisterClassA(&wc);

    if (enet_initialize() != 0) {
        MessageBoxA(NULL, "Could not start networking (enet_initialize failed).",
                    "Animal Crossing Online", MB_OK | MB_ICONERROR);
        return 1;
    }

    wnd = CreateWindowA(wc.lpszClassName, "Animal Crossing Online",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, 476, 340, NULL, NULL, inst, NULL);
    g_main_wnd = wnd;
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    while (GetMessage(&m, NULL, 0, 0) > 0) {
        HWND active = g_lb_wnd ? g_lb_wnd : wnd;
        if (!IsDialogMessage(active, &m)) { TranslateMessage(&m); DispatchMessage(&m); }
    }
    lobby_stop();
    enet_deinitialize();
    WSACleanup();
    return 0;
}
