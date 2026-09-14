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

#define IDC_HOST   1001
#define IDC_JOIN   1002
#define IDC_NAME   1003
#define IDC_ADDR   1004
#define IDC_CODE   1005
#define IDC_PLAY   1006
#define IDC_QUIT   1007
#define IDC_STATUS 1008
#define IDC_ADDRLBL 1009

static const char* GAME_EXE   = "AnimalCrossing.exe";
static const char* SERVER_EXE = "acnet_server.exe";
static const char* SETTINGS   = "settings.ini";
static const char* LAUNCHER_INI = "launcher.ini";
static const int   SERVER_PORT = 7777;

static HWND g_name, g_addr, g_code, g_status, g_addrlbl, g_host, g_join;
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

/* Directory the launcher executable lives in (with trailing backslash). */
static void compute_dir(void) {
    char path[MAX_PATH];
    char* slash;
    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    lstrcpynA(g_dir, path, sizeof(g_dir));
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
            MessageBoxA(wnd, "AnimalCrossing.exe was not found next to this launcher.\n"
                             "Put this launcher in the same folder as the game.", "Missing game", MB_OK|MB_ICONERROR);
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
            MessageBoxA(wnd, "acnet_server.exe was not found next to this launcher, so hosting is unavailable.\n"
                             "Ask whoever set up the game for the server file, or use Join instead.", "Cannot host", MB_OK|MB_ICONERROR);
            return;
        }
        if (!start_local_server(code)) {
            MessageBoxA(wnd, "The town server failed to start.", "Cannot host", MB_OK|MB_ICONERROR);
            return;
        }
        local_ip(ip, sizeof(ip));
        snprintf(msg, sizeof(msg),
                 "You are hosting!\n\nTell your friends to open this launcher, choose \"Join\", and enter:\n\n"
                 "    Server address:  %s\n    Invite code:     %s\n\n"
                 "(If they are not on your home network, you may need to forward UDP port %d on your router, "
                 "or host on an always-on server instead.)", ip, code, SERVER_PORT);
        MessageBoxA(wnd, msg, "Hosting", MB_OK|MB_ICONINFORMATION);
    }

    save_prefs(name, addr, code);

    if (!launch(GAME_EXE, 0)) {
        MessageBoxA(wnd, "Could not start the game.", "Error", MB_OK|MB_ICONERROR);
        return;
    }
    set_status(host ? "Hosting and launching the game..." : "Joining and launching the game...");
    PostQuitMessage(0);
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

    wnd = CreateWindowA(wc.lpszClassName, "Animal Crossing Online",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, 476, 340, NULL, NULL, inst, NULL);
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    while (GetMessage(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(wnd, &m)) { TranslateMessage(&m); DispatchMessage(&m); }
    }
    WSACleanup();
    return 0;
}
