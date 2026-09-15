/* launcher.c - "Animal Crossing Online" host/join launcher (multiplayer fork).
 *
 * A tiny Win32 GUI so non-technical players never touch a command prompt or a
 * config file. It sits next to AnimalCrossing.exe (and, for hosting, next to
 * acnet_server.exe). The player picks Host or Join, types a name and an invite
 * code, clicks Play; the launcher writes the game's settings.ini [Network]
 * block, starts the local server if hosting, and launches the game.
 *
 * Build (from repo root): bash launcher/build-windows.sh
 * The whole look is drawn with GDI by the launcher itself; it ships and uses
 * no game assets, fonts or logos.
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

static HWND g_name, g_addr, g_code, g_host, g_join, g_play, g_quit;
static HWND g_main_wnd;
static int  g_mode_host = 1;          /* 1 = host a town, 0 = join a friend */
static char g_status_text[256];

/* Defined with the rest of the window plumbing below; the waiting room uses it. */
static HWND mk(const char* cls, const char* text, DWORD style, int x, int y, int w, int h,
               HWND parent, int id);
/* The waiting room, opened by the Play button (defined further down). */
static void lobby_open(HWND parent, int is_host, const char* addr, const char* code,
                       const char* name);
static char g_dir[MAX_PATH]; /* folder the launcher (and game) live in */

/* ---- diagnostics ------------------------------------------------------- */
/* launcher.log next to the launcher: breadcrumbs plus, if it crashes, where.
 * Written with plain Win32 file calls so it works from any thread and needs
 * no state, and flushed per line so the last line before a crash survives. */
static void llog(const char* fmt, ...) {
    char path[MAX_PATH], line[1024];
    va_list ap;
    HANDLE h;
    DWORD n, written;
    SYSTEMTIME st;

    GetLocalTime(&st);
    n = (DWORD)snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond,
                        st.wMilliseconds);
    va_start(ap, fmt);
    n += (DWORD)vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    if (n > sizeof(line) - 3) n = sizeof(line) - 3;
    line[n++] = '\r'; line[n++] = '\n'; line[n] = '\0';

    snprintf(path, sizeof(path), "%slauncher.log", g_dir[0] ? g_dir : "");
    h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, line, n, &written, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : NULL;
    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    HMODULE mod = NULL;
    char name[MAX_PATH], msg[600];
    name[0] = '\0';
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr, &mod) && mod) {
        if (!GetModuleFileNameA(mod, name, sizeof(name))) name[0] = '\0';
    }
    llog("*** CRASH: exception 0x%08lX at %p in %s (base %p, offset 0x%lX)",
         (unsigned long)code, addr, name[0] ? name : "?", (void*)mod,
         (unsigned long)((char*)addr - (char*)mod));
    snprintf(msg, sizeof(msg),
             "The launcher crashed.\n\nException 0x%08lX at %p\nin %s\n\n"
             "Details were written to launcher.log next to the launcher - please send that file.",
             (unsigned long)code, addr, name[0] ? name : "(unknown module)");
    MessageBoxA(NULL, msg, "Animal Crossing Online", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ---- small helpers ------------------------------------------------------ */

static void set_status(const char* s) {
    lstrcpynA(g_status_text, s, sizeof(g_status_text));
    if (g_main_wnd) InvalidateRect(g_main_wnd, NULL, FALSE);
}

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
    return g_mode_host;
}

static void update_mode_ui(void) {
    int host = is_host_mode();
    EnableWindow(g_addr, host ? FALSE : TRUE);
    if (host) {
        char code[32];
        get_edit(g_code, code, sizeof(code));
        if (code[0] == '\0') { gen_code(code, sizeof(code)); SetWindowTextA(g_code, code); }
    }
    InvalidateRect(g_host, NULL, FALSE);
    InvalidateRect(g_join, NULL, FALSE);
    if (g_main_wnd) InvalidateRect(g_main_wnd, NULL, FALSE);
}

/* ---- the Play action ---------------------------------------------------- */

static void on_play(HWND wnd) {
    char name[64], addr[128], code[64], msg[512];
    int host = is_host_mode();
    llog("Play clicked (%s)", host ? "host" : "join");

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

    llog("checks passed; writing settings.ini");
    if (!write_settings(host, addr, code, name)) {
        llog("write_settings FAILED");
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
        llog("starting local server");
        if (!start_local_server(code)) {
            llog("start_local_server FAILED");
            MessageBoxA(wnd, "The town server failed to start.", "Cannot host", MB_OK|MB_ICONERROR);
            return;
        }
        (void)ip; (void)msg;
    }

    save_prefs(name, addr, code);
    set_status("");
    llog("opening waiting room");
    lobby_open(wnd, host, addr, code, name);
}


/* ---- look ---------------------------------------------------------------- */
/* Everything visual is drawn by the launcher itself with GDI. No game assets,
 * fonts or logos are used or shipped: the launcher borrows the feel of the
 * game's cream rounded dialogs with wood trim, its sky and grass, a leaf and
 * pill buttons, and draws all of it from scratch. */

#define CLR_SKY_TOP   RGB(0xB4, 0xE1, 0xF8)
#define CLR_SKY_BOT   RGB(0xEC, 0xF8, 0xFE)
#define CLR_GRASS     RGB(0x95, 0xD3, 0x68)
#define CLR_GRASS_DK  RGB(0x6D, 0xB4, 0x48)
#define CLR_CREAM     RGB(0xFF, 0xF8, 0xE2)
#define CLR_CREAM_DK  RGB(0xF0, 0xE0, 0xB9)
#define CLR_WOOD      RGB(0x8E, 0x5C, 0x2E)
#define CLR_WOOD_DK   RGB(0x5D, 0x3B, 0x1B)
#define CLR_TEXT      RGB(0x4A, 0x31, 0x15)
#define CLR_TEXT_SOFT RGB(0x93, 0x74, 0x50)
#define CLR_LEAF      RGB(0x55, 0xB3, 0x40)
#define CLR_LEAF_DK   RGB(0x31, 0x7E, 0x27)
#define CLR_BTN       RGB(0x5E, 0xC0, 0x4A)
#define CLR_BTN_HI    RGB(0x78, 0xD3, 0x62)
#define CLR_BTN_DK    RGB(0x3E, 0x91, 0x2F)
#define CLR_WHITE     RGB(0xFF, 0xFF, 0xFF)
#define CLR_ONLINE    RGB(0x3B, 0xB2, 0x44)
#define CLR_AWAY      RGB(0xBB, 0xB2, 0xA0)
#define CLR_BAD       RGB(0xD3, 0x4B, 0x3F)
#define CLR_WAIT      RGB(0xE8, 0xB4, 0x3A)

#define WIN_W 520
#define WIN_H 470

/* Layout shared by both windows: the cream panel under the title. */
#define PANEL_L 24
#define PANEL_T 80
#define PANEL_R (WIN_W - 24)
#define PANEL_B (WIN_H - 22)

static HFONT  g_font_title, g_font_body, g_font_bold, g_font_small;
static HBRUSH g_brush_field, g_brush_field_off;

static HFONT mkfont(const char* face, int px, int weight) {
    return CreateFontA(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

static void theme_init(void) {
    g_font_title = mkfont("Segoe UI", 30, FW_BOLD);
    g_font_body  = mkfont("Segoe UI", 16, FW_NORMAL);
    g_font_bold  = mkfont("Segoe UI", 16, FW_SEMIBOLD);
    g_font_small = mkfont("Segoe UI", 13, FW_NORMAL);
    g_brush_field     = CreateSolidBrush(CLR_WHITE);
    g_brush_field_off = CreateSolidBrush(CLR_CREAM_DK);
}

static void fill_rect(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* Vertical gradient in 2px bands; avoids linking msimg32. */
static void draw_gradient(HDC dc, RECT r, COLORREF top, COLORREF bot) {
    int h = r.bottom - r.top, y;
    for (y = 0; y < h; y += 2) {
        int t = h > 1 ? y * 255 / (h - 1) : 0;
        COLORREF c = RGB(GetRValue(top) + ((int)GetRValue(bot) - (int)GetRValue(top)) * t / 255,
                         GetGValue(top) + ((int)GetGValue(bot) - (int)GetGValue(top)) * t / 255,
                         GetBValue(top) + ((int)GetBValue(bot) - (int)GetBValue(top)) * t / 255);
        fill_rect(dc, r.left, r.top + y, r.right - r.left, 2, c);
    }
}

static void draw_round(HDC dc, RECT r, int radius, COLORREF fill, int has_fill, COLORREF edge, int edge_w) {
    HBRUSH b = has_fill ? CreateSolidBrush(fill) : (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN p = edge_w > 0 ? CreatePen(PS_SOLID, edge_w, edge) : (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (has_fill) DeleteObject(b);
    if (edge_w > 0) DeleteObject(p);
}

static void draw_ellipse(HDC dc, int l, int t, int r, int b, COLORREF fill) {
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, l, t, r, b);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
}

static void draw_cloud(HDC dc, int x, int y, int s) {
    draw_ellipse(dc, x, y + s / 3, x + s, y + s, CLR_WHITE);
    draw_ellipse(dc, x + s / 3, y, x + s, y + s * 3 / 4 + 4, CLR_WHITE);
    draw_ellipse(dc, x + s / 2, y + s / 4, x + s * 5 / 3, y + s, CLR_WHITE);
}

/* A leaf along the bottom-left -> top-right diagonal, drawn as two Bezier
 * arcs, with a midrib and a short stem. */
static void draw_leaf(HDC dc, int cx, int cy, int s, COLORREF fill, COLORREF edge) {
    POINT pt[7];
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 2, edge);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    pt[0].x = cx - s / 2;         pt[0].y = cy + s / 2;          /* stem end */
    pt[1].x = cx - s * 49 / 100;  pt[1].y = cy - s * 15 / 100;
    pt[2].x = cx - s * 16 / 100;  pt[2].y = cy - s * 48 / 100;
    pt[3].x = cx + s / 2;         pt[3].y = cy - s / 2;          /* tip */
    pt[4].x = cx + s * 48 / 100;  pt[4].y = cy + s * 16 / 100;
    pt[5].x = cx + s * 15 / 100;  pt[5].y = cy + s * 49 / 100;
    pt[6] = pt[0];
    BeginPath(dc);
    MoveToEx(dc, pt[0].x, pt[0].y, NULL);
    PolyBezierTo(dc, &pt[1], 6);
    EndPath(dc);
    StrokeAndFillPath(dc);
    MoveToEx(dc, pt[0].x + 3, pt[0].y - 3, NULL);
    LineTo(dc, pt[3].x - 3, pt[3].y + 3);
    MoveToEx(dc, pt[0].x, pt[0].y, NULL);
    LineTo(dc, pt[0].x - s / 5, pt[0].y + s / 5);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void draw_text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, UINT fmt, const char* t) {
    RECT r = { x, y, x + w, y + h };
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextA(dc, t, -1, &r, fmt);
    SelectObject(dc, of);
}

static void draw_dot(HDC dc, int cx, int cy, int r, COLORREF c) {
    draw_ellipse(dc, cx - r, cy - r, cx + r, cy + r, c);
}

/* Sky, clouds, and two rolling hills hanging off the bottom edge. */
static void draw_backdrop(HDC dc, int w, int h) {
    RECT sky = { 0, 0, w, h };
    draw_gradient(dc, sky, CLR_SKY_TOP, CLR_SKY_BOT);
    /* Clouds keep clear of the title text: far left behind the leaf, and the
     * strip of sky to the right of the title. */
    draw_cloud(dc, 6, 8, 24);
    draw_cloud(dc, w - 92, 14, 24);
    draw_cloud(dc, w - 58, 48, 14);
    draw_ellipse(dc, -w / 2, h - 96, w * 3 / 4, h + 170, CLR_GRASS_DK);
    draw_ellipse(dc, w / 4, h - 72, w + w / 2, h + 210, CLR_GRASS);
}

/* The cream dialog panel with wood trim and an inner highlight. */
static void draw_panel(HDC dc, RECT r) {
    RECT in = r;
    draw_round(dc, r, 26, CLR_CREAM, 1, CLR_WOOD, 4);
    InflateRect(&in, -7, -7);
    draw_round(dc, in, 18, 0, 0, CLR_CREAM_DK, 1);
}

/* Title row above the panel: leaf, big text, small line under it. */
static void draw_title(HDC dc, const char* title, const char* sub) {
    draw_leaf(dc, 46, 42, 30, CLR_LEAF, CLR_LEAF_DK);
    draw_text(dc, g_font_title, CLR_TEXT, 72, 16, WIN_W - 90, 40, DT_LEFT | DT_SINGLELINE | DT_VCENTER, title);
    if (sub) draw_text(dc, g_font_small, CLR_TEXT_SOFT, 74, 54, WIN_W - 90, 18, DT_LEFT | DT_SINGLELINE, sub);
}

/* Rounded frame painted on the parent behind an EDIT control. */
static void draw_field_frame(HDC dc, HWND edit, int enabled) {
    RECT r;
    GetWindowRect(edit, &r);
    MapWindowPoints(NULL, GetParent(edit), (POINT*)&r, 2);
    InflateRect(&r, 6, 5);
    draw_round(dc, r, 12, enabled ? CLR_WHITE : CLR_CREAM_DK, 1, CLR_WOOD, 2);
}

/* Owner-drawn buttons. kind: 0 primary (green), 1 secondary (cream),
 * 2 toggle selected, 3 toggle unselected. */
static void draw_button(const DRAWITEMSTRUCT* di, int kind) {
    RECT r = di->rcItem;
    HDC dc = di->hDC;
    int pressed  = (di->itemState & ODS_SELECTED) != 0;
    int disabled = (di->itemState & ODS_DISABLED) != 0;
    int radius = r.bottom - r.top;
    COLORREF fill, edge, txt;
    char text[128];

    GetWindowTextA(di->hwndItem, text, sizeof(text));
    fill_rect(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, CLR_CREAM);

    if (kind == 0)      { fill = pressed ? CLR_BTN_DK : CLR_BTN; edge = CLR_BTN_DK; txt = CLR_WHITE; }
    else if (kind == 2) { fill = CLR_LEAF;  edge = CLR_LEAF_DK; txt = CLR_WHITE; }
    else if (kind == 3) { fill = CLR_CREAM_DK; edge = CLR_WOOD; txt = CLR_TEXT_SOFT; }
    else                { fill = pressed ? CLR_CREAM_DK : CLR_WHITE; edge = CLR_WOOD; txt = CLR_TEXT; }
    if (disabled) { fill = CLR_CREAM_DK; edge = CLR_AWAY; txt = CLR_AWAY; }

    InflateRect(&r, -1, -1);
    if (pressed && !disabled) OffsetRect(&r, 1, 1);
    draw_round(dc, r, radius, fill, 1, edge, 2);
    if (kind == 0 && !disabled && !pressed) {
        RECT hi = r;
        hi.top += 3; hi.bottom = hi.top + (r.bottom - r.top) / 2 - 2;
        InflateRect(&hi, -6, 0);
        draw_round(dc, hi, radius, CLR_BTN_HI, 1, CLR_BTN_HI, 0);
    }
    draw_text(dc, g_font_bold, txt, r.left, r.top, r.right - r.left, r.bottom - r.top,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE, text);
    if (di->itemState & ODS_FOCUS) {
        RECT f = r;
        InflateRect(&f, -4, -4);
        draw_round(dc, f, radius, 0, 0, kind == 0 || kind == 2 ? CLR_WHITE : CLR_WOOD, 1);
    }
}

/* Double-buffered WM_PAINT: paint(dc) draws the whole client area. */
typedef void (*paint_fn)(HDC dc, int w, int h);
static void paint_buffered(HWND wnd, paint_fn paint) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    RECT rc;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ old;
    GetClientRect(wnd, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    old = SelectObject(mem, bmp);
    paint(mem, rc.right, rc.bottom);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(wnd, &ps);
}

static void set_font(HWND h, HFONT f) { SendMessageA(h, WM_SETFONT, (WPARAM)f, TRUE); }

/* Create a top-level window of exactly WIN_W x WIN_H client pixels. */
static HWND make_window(const char* cls, const char* title) {
    RECT r = { 0, 0, WIN_W, WIN_H };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    return CreateWindowA(cls, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                         r.bottom - r.top, NULL, NULL, GetModuleHandle(NULL), NULL);
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
static HWND    g_lb_wnd, g_lb_enter, g_lb_back;

/* What the waiting room shows, recomputed by lobby_refresh() from the shared
 * state and painted by lobby_paint(). */
typedef struct {
    int  conn_state;                 /* LOBBY_* */
    char conn_text[200];
    char slot_name[ACNET_MAX_PLAYERS][ACNET_NAME_LEN + 1];
    int  slot_online[ACNET_MAX_PLAYERS];
    int  slot_known;                 /* 1 = the resident rows carry real data */
    char town_text[240];
    char note_text[300];
} lobby_view_t;
static lobby_view_t g_lbv;
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

    llog("lobby thread: start (server %s, town %s)", L->host, L->invite);
    host = enet_host_create(NULL, 1, ACNET_CHANNELS, 0, 0);
    if (!host) {
        llog("lobby thread: enet_host_create FAILED");
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
                llog("lobby thread: connected");
                lobby_set_state(L, LOBBY_OK, "");
                last_poll = 0;
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                lobby_on_packet(L, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                llog("lobby thread: disconnected");
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
    llog("lobby thread: exit");
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
    char err[160];

    if (!L->wnd) return;

    EnterCriticalSection(&L->cs);
    state = L->state;
    have  = L->have;
    r     = L->rep;
    lstrcpynA(err, L->err, sizeof(err));
    LeaveCriticalSection(&L->cs);

    g_lbv.conn_state = state;
    if (state == LOBBY_OK) {
        lstrcpynA(g_lbv.conn_text, "Connected to the town server", sizeof(g_lbv.conn_text));
    } else if (state == LOBBY_CONNECTING) {
        lstrcpynA(g_lbv.conn_text, "Contacting the town server...", sizeof(g_lbv.conn_text));
    } else {
        snprintf(g_lbv.conn_text, sizeof(g_lbv.conn_text), "Not connected - %s",
                 err[0] ? err : "no reply from the server");
    }
    EnableWindow(g_lb_enter, state == LOBBY_OK ? TRUE : FALSE);

    g_lbv.slot_known = (state == LOBBY_OK && have);
    for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
        g_lbv.slot_name[i][0] = '\0';
        g_lbv.slot_online[i] = 0;
        if (g_lbv.slot_known && r.room_known) {
            memcpy(g_lbv.slot_name[i], r.slots[i].name, ACNET_NAME_LEN);
            g_lbv.slot_name[i][ACNET_NAME_LEN] = '\0';
            g_lbv.slot_online[i] = r.slots[i].online;
        }
    }

    if (state != LOBBY_OK || !have) {
        g_lbv.town_text[0] = '\0';
    } else if (!r.room_known) {
        lstrcpynA(g_lbv.town_text, "This town has not been started yet. Enter and save once to create it.",
                  sizeof(g_lbv.town_text));
    } else if (!r.town_present) {
        lstrcpynA(g_lbv.town_text, "Nobody has saved yet - the first person to save creates the town.",
                  sizeof(g_lbv.town_text));
    } else {
        lstrcpynA(g_lbv.town_text, "The town is ready. Everyone who enters loads the same one.",
                  sizeof(g_lbv.town_text));
    }
    InvalidateRect(g_lb_wnd, NULL, FALSE);
}

static void lobby_enter_town(HWND wnd) {
    llog("Enter Town clicked");
    lobby_stop();
    save_prefs(g_lb_name, g_lb_addr, g_lb_code);
    if (!launch(GAME_EXE, 0)) {
        MessageBoxA(wnd, "Could not start the game.", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    PostQuitMessage(0);
}

static void lobby_paint(HDC dc, int w, int h) {
    RECT panel = { PANEL_L, PANEL_T, PANEL_R, PANEL_B };
    char line[200];
    int  x = PANEL_L + 20, y, i;
    COLORREF dot;

    draw_backdrop(dc, w, h);
    snprintf(line, sizeof(line), "Town \"%s\"", g_lb_code);
    draw_title(dc, "Waiting room", line);
    draw_panel(dc, panel);

    /* connection line */
    y = PANEL_T + 18;
    dot = g_lbv.conn_state == LOBBY_OK ? CLR_ONLINE : g_lbv.conn_state == LOBBY_CONNECTING ? CLR_WAIT : CLR_BAD;
    draw_dot(dc, x + 7, y + 10, 6, dot);
    draw_text(dc, g_font_bold, CLR_TEXT, x + 22, y, PANEL_R - x - 40, 20, DT_LEFT | DT_SINGLELINE | DT_VCENTER,
              g_lbv.conn_text);

    /* resident rows */
    y += 34;
    for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
        RECT chip = { x, y, PANEL_R - 20, y + 36 };
        int  has = g_lbv.slot_name[i][0] != 0;
        draw_round(dc, chip, 18, has ? CLR_WHITE : CLR_CREAM_DK, 1, has ? CLR_WOOD : CLR_CREAM_DK, 2);
        draw_dot(dc, x + 18, y + 18, 11, has ? (g_lbv.slot_online[i] ? CLR_ONLINE : CLR_AWAY) : CLR_CREAM);
        snprintf(line, sizeof(line), "%d", i + 1);
        draw_text(dc, g_font_small, has ? CLR_WHITE : CLR_TEXT_SOFT, x + 8, y, 20, 36,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE, line);
        if (has) {
            draw_text(dc, g_font_bold, CLR_TEXT, x + 40, y, 250, 36, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                      g_lbv.slot_name[i]);
            draw_text(dc, g_font_small, g_lbv.slot_online[i] ? CLR_ONLINE : CLR_TEXT_SOFT, x + 40, y,
                      PANEL_R - 20 - x - 56, 36, DT_RIGHT | DT_VCENTER | DT_SINGLELINE,
                      g_lbv.slot_online[i] ? "in town now" : "away");
        } else {
            draw_text(dc, g_font_body, CLR_TEXT_SOFT, x + 40, y, 250, 36, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                      g_lbv.slot_known ? "empty - a friend can move in" : "");
        }
        y += 42;
    }

    /* town status and the note for friends */
    y += 2;
    draw_text(dc, g_font_body, CLR_TEXT, x, y, PANEL_R - 20 - x, 40, DT_LEFT | DT_WORDBREAK, g_lbv.town_text);
    y += 44;
    draw_text(dc, g_font_small, CLR_TEXT_SOFT, x, y, PANEL_R - 20 - x, 54, DT_LEFT | DT_WORDBREAK, g_lbv.note_text);
}

static LRESULT CALLBACK LobbyProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int by = PANEL_B - 58;
        g_lb_enter = mk("BUTTON", "Enter Town", BS_OWNERDRAW, PANEL_L + 20, by, 210, 42, wnd, IDC_LB_ENTER);
        g_lb_back  = mk("BUTTON", "Back", BS_OWNERDRAW, PANEL_R - 20 - 150, by, 150, 42, wnd, IDC_LB_BACK);
        EnableWindow(g_lb_enter, FALSE);

        memset(&g_lbv, 0, sizeof(g_lbv));
        g_lbv.conn_state = LOBBY_CONNECTING;
        lstrcpynA(g_lbv.conn_text, "Contacting the town server...", sizeof(g_lbv.conn_text));
        if (g_lb_is_host) {
            char ip[64];
            local_ip(ip, sizeof(ip));
            snprintf(g_lbv.note_text, sizeof(g_lbv.note_text),
                     "Friends in your house join with:  %s\n"
                     "Friends elsewhere need your public IP (search \"what is my IP\") "
                     "and UDP port %d forwarded to this PC.", ip, SERVER_PORT);
        } else {
            snprintf(g_lbv.note_text, sizeof(g_lbv.note_text), "Joining %s", g_lb_addr);
        }
        SetTimer(wnd, 1, 1000, NULL);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_buffered(wnd, lobby_paint);
        return 0;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        draw_button(di, di->CtlID == IDC_LB_ENTER ? 0 : 1);
        return TRUE;
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

    /* Reset everything except the critical section, which lives for the
     * whole process (initialized once in WinMain). */
    g_lobby.stop = 0;
    g_lobby.wnd = NULL;
    g_lobby.have = 0;
    g_lobby.err[0] = '\0';
    memset(&g_lobby.rep, 0, sizeof(g_lobby.rep));
    lstrcpynA(g_lobby.host, is_host ? "127.0.0.1" : addr, sizeof(g_lobby.host));
    lstrcpynA(g_lobby.invite, code, sizeof(g_lobby.invite));
    g_lobby.state = LOBBY_CONNECTING;

    llog("lobby: creating window");
    g_lb_wnd = make_window("ACOnlineLobby", "Animal Crossing Online - Waiting Room");
    if (!g_lb_wnd) { llog("lobby: CreateWindow FAILED (error %lu)", (unsigned long)GetLastError()); return; }
    g_lobby.wnd = g_lb_wnd;
    llog("lobby: window created");
    ShowWindow(parent, SW_HIDE);
    ShowWindow(g_lb_wnd, SW_SHOW);
    UpdateWindow(g_lb_wnd);

    g_lobby_thread = CreateThread(NULL, 0, lobby_worker, &g_lobby, 0, &tid);
    llog("lobby: thread %s", g_lobby_thread ? "started" : "FAILED");
}

/* ---- window plumbing ---------------------------------------------------- */

static HWND mk(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, HWND parent, int id) {
    return CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                         (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
}

static void main_paint(HDC dc, int w, int h) {
    RECT panel = { PANEL_L, PANEL_T, PANEL_R, PANEL_B };
    int x = PANEL_L + 20;
    int host = is_host_mode();

    draw_backdrop(dc, w, h);
    draw_title(dc, "Animal Crossing Online", "Play the GameCube classic with friends, in one shared town");
    draw_panel(dc, panel);

    draw_text(dc, g_font_small, CLR_TEXT_SOFT, x, 158, 220, 18, DT_LEFT | DT_SINGLELINE, "YOUR NAME");
    draw_text(dc, g_font_small, CLR_TEXT_SOFT, x + 236, 158, 220, 18, DT_LEFT | DT_SINGLELINE, "INVITE CODE");
    draw_text(dc, g_font_small, CLR_TEXT_SOFT, x, 228, 440, 18, DT_LEFT | DT_SINGLELINE,
              host ? "SERVER ADDRESS  (not needed - you are the server)"
                   : "SERVER ADDRESS  (ask your host for it)");
    draw_field_frame(dc, g_name, 1);
    draw_field_frame(dc, g_code, 1);
    draw_field_frame(dc, g_addr, !host);

    draw_text(dc, g_font_body, CLR_TEXT_SOFT, x, 300, PANEL_R - 20 - x, 60, DT_LEFT | DT_WORDBREAK,
              g_status_text[0] ? g_status_text
                               : (host ? "Hosting starts the town server on this PC. Your friends join with your address and the invite code."
                                       : "Enter the address and invite code your host gave you."));
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int x = PANEL_L + 20;
        g_host = mk("BUTTON", "Host a town",    BS_OWNERDRAW, x,       PANEL_T + 22, 212, 40, wnd, IDC_HOST);
        g_join = mk("BUTTON", "Join a friend",  BS_OWNERDRAW, x + 220, PANEL_T + 22, 212, 40, wnd, IDC_JOIN);
        g_name = mk("EDIT", "", ES_AUTOHSCROLL, x + 6,       180, 200, 22, wnd, IDC_NAME);
        g_code = mk("EDIT", "", ES_AUTOHSCROLL, x + 236 + 6, 180, 190, 22, wnd, IDC_CODE);
        g_addr = mk("EDIT", "", ES_AUTOHSCROLL, x + 6,       250, 420, 22, wnd, IDC_ADDR);
        g_play = mk("BUTTON", "Play", BS_OWNERDRAW, x, PANEL_B - 62, 210, 44, wnd, IDC_PLAY);
        g_quit = mk("BUTTON", "Quit", BS_OWNERDRAW, PANEL_R - 20 - 150, PANEL_B - 62, 150, 44, wnd, IDC_QUIT);
        set_font(g_name, g_font_body);
        set_font(g_code, g_font_body);
        set_font(g_addr, g_font_body);
        SendMessageA(g_name, EM_SETLIMITTEXT, ACNET_NAME_LEN - 1, 0);
        SendMessageA(g_code, EM_SETLIMITTEXT, ACNET_INVITE_LEN - 1, 0);
        load_prefs();
        update_mode_ui();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_buffered(wnd, main_paint);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int enabled = IsWindowEnabled((HWND)lp);
        SetTextColor(dc, enabled ? CLR_TEXT : CLR_TEXT_SOFT);
        SetBkColor(dc, enabled ? CLR_WHITE : CLR_CREAM_DK);
        return (LRESULT)(enabled ? g_brush_field : g_brush_field_off);
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        int kind;
        switch (di->CtlID) {
        case IDC_HOST: kind = is_host_mode() ? 2 : 3; break;
        case IDC_JOIN: kind = is_host_mode() ? 3 : 2; break;
        case IDC_PLAY: kind = 0; break;
        default:       kind = 1; break;
        }
        draw_button(di, kind);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_HOST: g_mode_host = 1; update_mode_ui(); return 0;
        case IDC_JOIN: g_mode_host = 0; update_mode_ui(); return 0;
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

    compute_dir();
    SetUnhandledExceptionFilter(crash_filter);
    InitializeCriticalSection(&g_lobby.cs);
    llog("---- launcher start, folder %s", g_dir);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    theme_init();

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; /* everything is painted in WM_PAINT */
    wc.lpszClassName = "ACOnlineLauncher";
    RegisterClassA(&wc);

    wc.lpfnWndProc = LobbyProc;
    wc.lpszClassName = "ACOnlineLobby";
    RegisterClassA(&wc);

    llog("window classes registered");
    if (enet_initialize() != 0) {
        llog("enet_initialize FAILED");
        MessageBoxA(NULL, "Could not start networking (enet_initialize failed).",
                    "Animal Crossing Online", MB_OK | MB_ICONERROR);
        return 1;
    }

    wnd = make_window("ACOnlineLauncher", "Animal Crossing Online");
    g_main_wnd = wnd;
    llog("main window %s", wnd ? "created" : "FAILED");
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    while (GetMessage(&m, NULL, 0, 0) > 0) {
        HWND active = g_lb_wnd ? g_lb_wnd : wnd;
        /* Enter triggers the main action; owner-drawn buttons cannot be the
         * dialog's default button, so do it by hand. */
        /* Ctrl+A selects all in a text field; plain EDIT controls do not. */
        if (m.message == WM_KEYDOWN && m.wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            char cls[16];
            if (GetClassNameA(m.hwnd, cls, sizeof(cls)) && lstrcmpiA(cls, "EDIT") == 0) {
                SendMessageA(m.hwnd, EM_SETSEL, 0, -1);
                continue;
            }
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN) {
            if (g_lb_wnd && IsWindowEnabled(g_lb_enter)) { PostMessageA(g_lb_wnd, WM_COMMAND, IDC_LB_ENTER, 0); continue; }
            if (!g_lb_wnd) { PostMessageA(wnd, WM_COMMAND, IDC_PLAY, 0); continue; }
        }
        if (!IsDialogMessage(active, &m)) { TranslateMessage(&m); DispatchMessage(&m); }
    }
    llog("message loop ended");
    lobby_stop();
    enet_deinitialize();
    WSACleanup();
    llog("clean exit");
    return 0;
}
