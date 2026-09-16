/* m_chat_bubble.c - chat messages in the game's own thought bubble
 * (multiplayer fork). See m_chat_bubble.h.
 *
 * Everything about the look is copied from watch_my_step_move/draw in
 * m_watch_my_step.c so it matches the item-name bubble exactly: the same
 * models (fki_win_w1T/w2T: the two dots, w3T: the bubble, w4: the fading
 * bubble), the same placement to the upper left or right of the character,
 * the same growth with the text width, the same easing, the same font size
 * and colour. Only the text and the anchor differ.
 */
#ifdef TARGET_PC

#include "m_chat_bubble.h"

#include "m_play.h"
#include "m_font.h"
#include "m_lib.h"
#include "m_player_lib.h"
#include "m_watch_my_step.h" /* fki_win_* models */
#include "sys_matrix.h"
#include "m_rcp.h"
#include "ac_puppet.h"
#include "libultra/libultra.h"
#include "MSL_C/w_math.h"
#include <string.h>

#define mCB_MAX        4
#define mCB_TEXT_MAX   96
#define mCB_PAGE_WIDTH 150.0f /* text pixels (at the bubble's 0.875 scale) before a new page */
#define mCB_PAGE_HARD  30     /* a single word longer than this is cut */

typedef struct {
    int active;
    int slot;
    u8  text[mCB_TEXT_MAX + 1]; /* in the game's character set */
    int text_len;
    int page_start;
    int page_len;
    f32 page_timer; /* frames left on this page */

    /* Exactly the fields the game's bubble keeps. */
    f32 pos_x, pos_y;
    f32 opacity;
    f32 trans_x, trans_y;
    f32 scale;
    f32 timer;
    int mode;
    int visible;
} mCB_bubble_c;

static mCB_bubble_c S_bubble[mCB_MAX];

/* ASCII to the game's character set: letters, digits and the punctuation
 * the font has keep their codes (the same table pc_typing.c uses for the
 * game's text editor); the few it lacks become '?'. */
static u8 mCB_char(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return (u8)c;
    }
    switch (c) {
        case ' ': case '!': case '"': case '%': case '&': case '\'': case '(': case ')': case '*': case '+':
        case ',': case '-': case '.': case '/': case ':': case ';': case '<': case '=': case '>': case '?':
        case '@': case '\\': case '_':
            return (u8)c;
        default:
            return (u8)CHAR_QUESTIONMARK;
    }
}

static f32 mCB_text_width(const u8* s, int len) {
    return (f32)mFont_GetStringWidth((u8*)s, len, TRUE) * 0.875f;
}

/* The next page: whole words up to mCB_PAGE_WIDTH, and the bubble scale the
 * game derives from the text width. */
static void mCB_set_page(mCB_bubble_c* b, int start) {
    int len = 0, last_space = -1;
    f32 size;

    while (start < b->text_len && b->text[start] == CHAR_SPACE) {
        start++;
    }
    b->page_start = start;
    while (start + len < b->text_len) {
        if (b->text[start + len] == CHAR_SPACE) {
            last_space = len;
        }
        if (len > 0 && mCB_text_width(b->text + start, len + 1) > mCB_PAGE_WIDTH) {
            break;
        }
        if (len >= mCB_PAGE_HARD) {
            break;
        }
        len++;
    }
    if (start + len < b->text_len && last_space > 0 && b->text[start + len] != CHAR_SPACE) {
        len = last_space; /* break between words */
    }
    if (len <= 0) {
        len = b->text_len - start; /* whatever is left */
    }
    b->page_len = len;

    size = mCB_text_width(b->text + start, len) - 17.5f; /* as the game: width * 0.875 - 17.5 */
    if (size < 0.0f) {
        size = 0.0f;
    }
    b->scale = size / 122.5f;
    b->page_timer = 90.0f + 7.0f * (f32)len; /* 1.5 s plus 0.12 s a character */
    if (start + len >= b->text_len) {
        b->page_timer += 60.0f; /* the last page lingers a little */
    }
}

extern void mCB_say(int slot, const char* ascii) {
    mCB_bubble_c* b;
    int i;

    if (slot < 0 || slot >= mCB_MAX || ascii == NULL) {
        return;
    }
    b = &S_bubble[slot];
    b->slot = slot;
    for (i = 0; ascii[i] != '\0' && i < mCB_TEXT_MAX; i++) {
        b->text[i] = mCB_char(ascii[i]);
    }
    b->text_len = i;
    b->text[i] = CHAR_SPACE;
    if (b->text_len == 0) {
        return;
    }
    if (b->active && b->mode == 3) {
        /* Already up: just change what it says (the bubble re-sizes). */
        mCB_set_page(b, 0);
        return;
    }
    b->active = 1;
    b->opacity = 0.0f;
    b->timer = 2.0f;
    b->mode = 1;
    b->visible = 0;
    mCB_set_page(b, 0);
}

extern int mCB_active(int slot) {
    return slot >= 0 && slot < mCB_MAX && S_bubble[slot].active;
}

extern int mCB_visible(int slot) {
    return slot >= 0 && slot < mCB_MAX && S_bubble[slot].active && S_bubble[slot].visible;
}

extern void mCB_move(GAME_PLAY* play) {
    f32 dt = (f32)play->game.graph->dt_num_60fps_frames;
    int i;

    for (i = 0; i < mCB_MAX; i++) {
        mCB_bubble_c* b = &S_bubble[i];
        ACTOR* a;

        b->visible = 0;
        if (!b->active) {
            continue;
        }

        switch (b->mode) {
            case 1:
            case 2: {
                /* The two dots pop in, two frames apart, as in the game. */
                b->timer -= dt;
                if (b->timer <= 0.0f) {
                    b->timer = 2.0f;
                    b->mode++;
                }
                break;
            }
            case 3: {
                add_calc(&b->opacity, 1.0f, 1.0f - sqrtf(0.5), 0.25f, 0.15f);
                b->page_timer -= dt;
                if (b->page_timer <= 0.0f) {
                    if (b->page_start + b->page_len < b->text_len) {
                        mCB_set_page(b, b->page_start + b->page_len);
                    } else {
                        b->mode = 4;
                    }
                }
                break;
            }
            case 4: {
                add_calc(&b->opacity, 0.0f, 1.0f - sqrtf(0.5), 0.1f, 0.05f);
                add_calc(&b->opacity, 0.0f, 1.0f - sqrtf(0.5), 0.005f, 0.005f);
                if (b->opacity < 0.01f) {
                    b->mode = 0;
                    b->active = 0;
                }
                break;
            }
            default: {
                b->mode = 0;
                b->active = 0;
                break;
            }
        }
        if (!b->active) {
            continue;
        }

        /* Anchor: our own character, or that resident's puppet. Nothing to
         * anchor to (they are in another room, or off our screen): the bubble
         * keeps its timing but is not drawn; the overlay shows the text
         * in a corner instead. */
        a = Puppet_chat_actor(play, b->slot);
        if (a == NULL) {
            continue;
        }
        {
            xyz_t position = a->world.position;
            xyz_t screen_pos;

            position.y += 30.0f;
            Game_play_Projection_Trans(play, &position, &screen_pos);
            if (screen_pos.x < -60.0f || screen_pos.x > 380.0f || screen_pos.y < -60.0f || screen_pos.y > 300.0f) {
                continue; /* off screen */
            }
            if (b->mode < 4) {
                /* Placement to the upper left or right of the character, as
                 * the game's item bubble decides it. */
                b->trans_x = 1.0f;
                if (screen_pos.x > 200.0f) {
                    b->trans_x = -1.0f;
                } else if (screen_pos.x > 120.0f && a->shape_info.rotation.y >= 0) {
                    b->trans_x = -1.0f;
                }
                b->trans_y = 1.0f;
                if (screen_pos.y < 68.0f) {
                    b->trans_y = -1.0f;
                }
                b->pos_x = screen_pos.x - (b->trans_x * -40.0f + 160.0f);
                b->pos_y = screen_pos.y - (b->trans_y * 42.0f + 120.0f);
            }
        }
        b->visible = 1;
    }
}

/* One bubble, exactly as watch_my_step_draw draws the item bubble. */
static void mCB_draw_one(GAME_PLAY* play, mCB_bubble_c* b) {
    GAME* game = (GAME*)play;
    GRAPH* g = play->game.graph;

    OPEN_DISP(g);

    Matrix_scale(16.0f, 16.0f, 16.0f, MTX_LOAD);
    Matrix_translate(b->pos_x, -b->pos_y, 0.0f, MTX_MULT);

    {
        Gfx* font_gfx = NOW_FONT_DISP;
        gSPMatrix(font_gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(font_gfx++, fki_win_mode);
        gDPSetRenderMode(font_gfx++, G_RM_CLD_SURF, G_RM_CLD_SURF2);

        switch (b->mode) {
            case 3: {
                Matrix_push();
                Matrix_scale(b->opacity * (b->scale * 0.75f + 0.25f), b->opacity * (b->scale * 0.23333335f + 0.76666665f),
                             b->opacity, MTX_MULT);
                gSPMatrix(font_gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                Matrix_pull();
                gSPDisplayList(font_gfx++, fki_win_w3T_model);
            }
            /* fallthrough 3 -> 2 */
            case 2: {
                Matrix_push();
                Matrix_translate(b->trans_x * -1.0f, b->trans_y * -20.0f, 0.0f, MTX_MULT);
                gSPMatrix(font_gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                Matrix_pull();
                gSPDisplayList(font_gfx++, fki_win_w2T_model);
            }
            /* fallthrough 2 -> 1 */
            case 1: {
                Matrix_push();
                Matrix_translate(b->trans_x * -13.0f, b->trans_y * -30.0f, 0.0f, MTX_MULT);
                gSPMatrix(font_gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                Matrix_pull();
                gSPDisplayList(font_gfx++, fki_win_w1T_model);
                break;
            }
            case 4: {
                u8 a = (u8)(b->opacity * 255.0f);
                gDPSetPrimColor(font_gfx++, 0, a, 255, 255, 215, a);
                Matrix_push();
                Matrix_scale(b->scale * 0.75f + 0.25f, b->scale * 0.23333335f + 0.76666665f, 1.0f, MTX_MULT);
                gSPMatrix(font_gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                Matrix_pull();
                gSPDisplayList(font_gfx++, fki_win_w4_model);
                break;
            }
        }

        /* Reset font matrix scale */
        Matrix_scale(1.0f, 1.0f, 1.0f, MTX_LOAD);
        gSPMatrix(font_gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        SET_FONT_DISP(font_gfx);
    }

    CLOSE_DISP(g);

    if (b->mode >= 3) {
        f32 text_opacity = (b->opacity - 0.5f) * 2.0f;

        if (text_opacity > 0.0f) {
            mFont_SetLineStrings(game, b->text + b->page_start, b->page_len,
                                 (10.0f + ((b->pos_x + 160.0f) - (0.5f * (40.0f + (b->scale * 120.0f))))) +
                                     (1.0f - b->scale),
                                 120.0f + ((3.0f * b->scale) + (5.0f + (b->pos_y - (0.5f * (23.0f + (b->scale * 7.0f)))))),
                                 45, 45, 35, (int)(255.0f * text_opacity), FALSE, TRUE, 0.875f, 0.875f, mFont_MODE_FONT);
        }
    }
}

extern void mCB_draw(GAME_PLAY* play) {
    GRAPH* g = play->game.graph;
    Mtx* font_mtx;
    int i, any = 0;

    for (i = 0; i < mCB_MAX; i++) {
        if (S_bubble[i].active && S_bubble[i].visible && S_bubble[i].mode != 0) {
            any = 1;
        }
    }
    if (!any) {
        return;
    }

    /* The font's own orthographic projection, as the game's bubble loads it. */
    OPEN_DISP(g);
    font_mtx = GRAPH_ALLOC_TYPE(g, Mtx, 1);
    if (font_mtx != NULL) {
        mFont_CulcOrthoMatrix(font_mtx);
        gSPMatrix(NOW_FONT_DISP++, font_mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    }
    CLOSE_DISP(g);

    for (i = 0; i < mCB_MAX; i++) {
        mCB_bubble_c* b = &S_bubble[i];
        if (b->active && b->visible && b->mode != 0) {
            mCB_draw_one(play, b);
        }
    }
}

#endif /* TARGET_PC */
