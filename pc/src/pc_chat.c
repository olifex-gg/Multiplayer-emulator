/* pc_chat.c - in-game text chat overlay. See pc_chat.h. */
#include "pc_platform.h" /* first: it sets up SDL without SDL's main() rename */
#include "pc_chat.h"

#include "pc_net.h"
#include "pc_text_draw.h"
#include "pc_pause_menu.h" /* g_pc_paused */
#include "pc_typing.h"     /* g_pc_typing_mode / g_pc_editor_active */

#include "graph.h"
#include "m_font.h"
#include "m_rcp.h"
#include <dolphin/os.h>

#include <stdio.h>
#include <string.h>

#define CHAT_LINES     6
#define CHAT_SHOW_MS   14000 /* how long a line stays on screen */
#define CHAT_FADE_MS   2000  /* ...of which the last part fades out */
#define CHAT_KEY       SDLK_t

int g_pc_chat_typing;

typedef struct {
    char   text[ACNET_NAME_LEN + 2 + ACNET_CHAT_LEN + 1];
    Uint32 ms;
    int    used;
} chat_line_t;

static chat_line_t s_lines[CHAT_LINES]; /* oldest first */
static char        s_input[ACNET_CHAT_LEN + 1];
static int         s_input_len;
static int         s_swallow_open_key; /* the T that opened the line also arrives as text */
static SDL_Keycode s_closing_key;      /* the key that closed the line, until it comes back up */
static Uint32      s_closed_ms;

static void chat_add(int slot, const char* text) {
    const char* name = pc_net_peer_name(slot);
    char label[ACNET_NAME_LEN + 16];
    int i;

    if (name == NULL || name[0] == '\0') {
        snprintf(label, sizeof(label), "Resident %d", slot + 1);
    } else {
        snprintf(label, sizeof(label), "%s", name);
    }
    for (i = 0; i + 1 < CHAT_LINES; i++) {
        s_lines[i] = s_lines[i + 1];
    }
    snprintf(s_lines[CHAT_LINES - 1].text, sizeof(s_lines[CHAT_LINES - 1].text), "%s: %s", label, text);
    s_lines[CHAT_LINES - 1].ms = SDL_GetTicks();
    s_lines[CHAT_LINES - 1].used = 1;
    OSReport("[chat] %s: %s\n", label, text);
}

static void chat_open(void) {
    g_pc_chat_typing = 1;
    s_input_len = 0;
    s_input[0] = '\0';
    s_swallow_open_key = 1;
    SDL_StartTextInput();
}

static void chat_close_by(SDL_Keycode key) {
    s_closing_key = key;
    s_closed_ms = SDL_GetTicks();
    g_pc_chat_typing = 0;
    s_input_len = 0;
    s_input[0] = '\0';
    /* The game's own text editor may want text input; leave it to it. */
    if (!(g_pc_typing_mode && g_pc_editor_active)) {
        SDL_StopTextInput();
    }
}

static void chat_send(void) {
    int i;
    /* Trim trailing spaces; an empty line just closes. */
    while (s_input_len > 0 && s_input[s_input_len - 1] == ' ') {
        s_input[--s_input_len] = '\0';
    }
    for (i = 0; i < s_input_len && s_input[i] == ' '; i++) {
    }
    if (i < s_input_len) {
        pc_net_send_chat(s_input + i);
        chat_add(pc_net_local_slot(), s_input + i); /* the server does not echo to the sender */
    }
}

int pc_chat_handle_event(const SDL_Event* e) {
    if (e->type == SDL_KEYDOWN) {
        if (!g_pc_chat_typing) {
            if (e->key.keysym.sym == CHAT_KEY && !e->key.repeat && pc_net_enabled() && !g_pc_paused &&
                !(g_pc_typing_mode && g_pc_editor_active)) {
                chat_open();
                return 1;
            }
            return 0;
        }
        switch (e->key.keysym.sym) {
            case SDLK_ESCAPE:
                chat_close_by(SDLK_ESCAPE);
                return 1;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                chat_send();
                chat_close_by(e->key.keysym.sym);
                return 1;
            case SDLK_BACKSPACE:
                if (s_input_len > 0) {
                    s_input[--s_input_len] = '\0';
                }
                return 1;
            default:
                return 1; /* every other key is the chat's while the line is open */
        }
    }
    if (e->type == SDL_KEYUP) {
        if (s_closing_key != SDLK_UNKNOWN && e->key.keysym.sym == s_closing_key) {
            s_closing_key = SDLK_UNKNOWN; /* released: the game may have the keyboard back */
            return 1;
        }
        return 0;
    }
    if (e->type == SDL_TEXTINPUT && g_pc_chat_typing) {
        const char* t = e->text.text;
        if (s_swallow_open_key) {
            s_swallow_open_key = 0;
            if ((t[0] == 't' || t[0] == 'T') && t[1] == '\0') {
                return 1;
            }
        }
        /* The game font has plain ASCII; anything else becomes '?'. */
        for (; *t; t++) {
            unsigned char c = (unsigned char)*t;
            char out;
            if (c >= 0x20 && c < 0x7F) {
                out = (char)c;
            } else if ((c & 0xC0) == 0x80) {
                continue; /* continuation byte of a character we already replaced */
            } else {
                out = '?';
            }
            if (s_input_len < ACNET_CHAT_LEN) {
                s_input[s_input_len++] = out;
                s_input[s_input_len] = '\0';
            }
        }
        return 1;
    }
    return 0;
}

int pc_chat_blocks_pad(void) {
    if (g_pc_chat_typing) {
        return 1;
    }
    if (s_closing_key != SDLK_UNKNOWN) {
        if (SDL_GetTicks() - s_closed_ms < 1500) {
            return 1;
        }
        s_closing_key = SDLK_UNKNOWN; /* the key-up never came (focus changed?); do not block forever */
    }
    return 0;
}

/* A translucent box in font-space (320 x 240) coordinates, like pc_menu_dim_rect. */
static void chat_rect(GRAPH* graph, int x0, int y0, int x1, int y1, int alpha) {
    Gfx* gfx;
    OPEN_DISP(graph);
    gfx = NOW_FONT_DISP;
    gDPPipeSync(gfx++);
    gDPSetOtherMode(gfx++,
                    G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2);
    gDPSetCombineMode(gfx++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(gfx++, 0, 0, 0, 0, 0, alpha);
    gfx = gfx_gSPTextureRectangle1(gfx, x0 << 2, y0 << 2, x1 << 2, y1 << 2, 0, 0, 0, 0, 0);
    gDPPipeSync(gfx++);
    SET_FONT_DISP(gfx);
    CLOSE_DISP(graph);
}

void pc_chat_draw(struct game_s* game) {
    GRAPH* graph;
    Uint32 now;
    int i, shown = 0, widest = 0;
    const f32 line_h = 12.0f;
    const f32 scale = 0.75f;
    const f32 x = 6.0f;
    int slot;
    char text[ACNET_CHAT_LEN + 1];

    if (!pc_net_enabled() || game == NULL || game->graph == NULL) {
        return;
    }
    /* Drain what arrived this frame. */
    while (pc_net_poll_chat(&slot, text, sizeof(text))) {
        chat_add(slot, text);
    }

    now = SDL_GetTicks();
    for (i = 0; i < CHAT_LINES; i++) {
        if (s_lines[i].used && now - s_lines[i].ms < CHAT_SHOW_MS) {
            int w = (int)((f32)pc_text_width(s_lines[i].text) * scale);
            if (w > widest) widest = w;
            shown++;
        }
    }
    if (shown == 0 && !g_pc_chat_typing) {
        return;
    }

    graph = game->graph;
    mFont_SetMatrix(graph, mFont_MODE_FONT);

    if (shown > 0) {
        f32 y = 6.0f;
        chat_rect(graph, 2, 2, (int)(x + widest + 6.0f), (int)(6.0f + shown * line_h + 2.0f), 110);
        for (i = 0; i < CHAT_LINES; i++) {
            Uint32 age;
            int a = 255;
            if (!s_lines[i].used) continue;
            age = now - s_lines[i].ms;
            if (age >= CHAT_SHOW_MS) continue;
            if (age > CHAT_SHOW_MS - CHAT_FADE_MS) {
                a = (int)(255u * (CHAT_SHOW_MS - age) / CHAT_FADE_MS);
            }
            pc_text_draw(game, s_lines[i].text, x, y, 255, 255, 255, a, scale);
            y += line_h;
        }
    }

    if (g_pc_chat_typing) {
        char line[ACNET_CHAT_LEN + 8];
        f32 y = 240.0f - 18.0f;
        snprintf(line, sizeof(line), "> %s%s", s_input, ((now / 400) & 1) ? "_" : " ");
        chat_rect(graph, 2, (int)(y - 3.0f), 318, (int)(y + line_h + 1.0f), 150);
        pc_text_draw(game, line, x, y, 255, 240, 170, 255, scale);
        pc_text_draw(game, "Enter: send   Esc: cancel", 200.0f, y, 170, 170, 170, 255, 0.6f);
    }

    mFont_UnSetMatrix(graph, mFont_MODE_FONT);
}
