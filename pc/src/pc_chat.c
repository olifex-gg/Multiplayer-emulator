/* pc_chat.c - in-game text chat: the line you type at the bottom of the
 * screen, and the hand-over of each message to the game's own speech window
 * (m_chat_bubble.c), where it appears with the sender's name on the tag the
 * way a villager's words do. See pc_chat.h. */
#include "pc_platform.h" /* first: it sets up SDL without SDL's main() rename */
#include "pc_chat.h"

#include "pc_net.h"
#include "pc_text_draw.h"
#include "pc_pause_menu.h" /* g_pc_paused */
#include "pc_typing.h"     /* g_pc_typing_mode / g_pc_editor_active */

#include "graph.h"
#include "m_font.h"
#include "m_rcp.h"
#include "m_chat_bubble.h"
#include <dolphin/os.h>

#include <stdio.h>
#include <string.h>

#define CHAT_KEY    SDLK_t
#define CHAT_SCALE  0.75f
#define CHAT_LINE_H 11.0f

int g_pc_chat_typing;

static char        s_input[ACNET_CHAT_LEN + 1];
static int         s_input_len;
static int         s_swallow_open_key; /* the T that opened the line also arrives as text */
static Uint32      s_open_ms;           /* when it opened: that text comes with the key press */
static SDL_Keycode s_closing_key;      /* the key that closed the line, until it comes back up */
static Uint32      s_closed_ms;

static const char* chat_name(int slot) {
    static char label[ACNET_NAME_LEN + 16];
    const char* name = pc_net_peer_name(slot);
    if (name == NULL || name[0] == '\0') {
        snprintf(label, sizeof(label), "Resident %d", slot + 1);
        return label;
    }
    return name;
}

static void chat_say(int slot, const char* text) {
    if (slot < 0 || slot >= ACNET_MAX_PLAYERS) {
        return;
    }
    mCB_say(slot, text); /* the game's speech window, with their name on the tag */
    OSReport("[chat] %s: %s\n", chat_name(slot), text);
}

static void chat_open(void) {
    g_pc_chat_typing = 1;
    s_input_len = 0;
    s_input[0] = '\0';
    s_swallow_open_key = 1;
    s_open_ms = SDL_GetTicks();
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
        chat_say(pc_net_local_slot(), s_input + i); /* the server does not echo to the sender */
    }
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
            /* Only the text that came with the opening key press itself
             * (a real keyboard delivers it in the same instant); a T typed
             * later is the first letter of the message. */
            s_swallow_open_key = 0;
            if ((t[0] == 't' || t[0] == 'T') && t[1] == '\0' && SDL_GetTicks() - s_open_ms < 250) {
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

/* A translucent box in font-space (320 x 240) coordinates, like pc_menu_dim_rect. */
static void chat_rect(GRAPH* graph, f32 x0, f32 y0, f32 x1, f32 y1, int alpha) {
    Gfx* gfx;
    if (x1 <= x0 || y1 <= y0) return;
    OPEN_DISP(graph);
    gfx = NOW_FONT_DISP;
    gDPPipeSync(gfx++);
    gDPSetOtherMode(gfx++,
                    G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2);
    gDPSetCombineMode(gfx++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(gfx++, 0, 0, 0, 0, 0, alpha);
    gfx = gfx_gSPTextureRectangle1(gfx, (int)(x0 * 4.0f), (int)(y0 * 4.0f), (int)(x1 * 4.0f), (int)(y1 * 4.0f), 0, 0,
                                   0, 0, 0);
    gDPPipeSync(gfx++);
    SET_FONT_DISP(gfx);
    CLOSE_DISP(graph);
}

void pc_chat_draw(struct game_s* game) {
    GRAPH* graph;
    int in_slot;
    char text[ACNET_CHAT_LEN + 1];

    if (!pc_net_enabled() || game == NULL || game->graph == NULL) {
        return;
    }
    /* Drain what arrived this frame. */
    while (pc_net_poll_chat(&in_slot, text, sizeof(text))) {
        chat_say(in_slot, text);
    }
    if (!g_pc_chat_typing) {
        return;
    }

    graph = game->graph;
    mFont_SetMatrix(graph, mFont_MODE_FONT);
    {
        char line[ACNET_CHAT_LEN + 8];
        f32 y = 240.0f - 18.0f;
        const char* shown = s_input;
        /* A long line scrolls: only its tail fits the bar. */
        if (s_input_len > 62) {
            shown = s_input + (s_input_len - 62);
        }
        snprintf(line, sizeof(line), "> %s%s%s", shown == s_input ? "" : "...", shown,
                 ((SDL_GetTicks() / 400) & 1) ? "_" : " ");
        chat_rect(graph, 2.0f, y - 3.0f, 318.0f, y + CHAT_LINE_H + 2.0f, 150);
        pc_text_draw(game, line, 6.0f, y, 255, 240, 170, 255, CHAT_SCALE);
        if (s_input_len < 40) { /* the hint would sit on top of a long line */
            pc_text_draw(game, "Enter: send   Esc: cancel", 200.0f, y, 170, 170, 170, 255, 0.6f);
        }
    }
    mFont_UnSetMatrix(graph, mFont_MODE_FONT);
}
