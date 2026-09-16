/* m_chat_bubble.c - chat messages in the game's own speech window
 * (multiplayer fork). See m_chat_bubble.h.
 *
 * The window is the villagers' dialogue window, driven exactly as the game
 * drives it (mMsg_request_main_appear), with three small hooks in m_msg:
 * the text comes from a buffer instead of the message ROM (message number
 * mMsg_CHAT_MSG_NO), the name tag shows a name we give it instead of a
 * villager's, and the window body is drawn shorter when the text has fewer
 * than four lines. The message ends with the game's own timed-end control
 * code, so the window closes itself; the continue button is locked so A
 * does nothing to it and the reader keeps walking.
 */
#ifdef TARGET_PC

#include "m_chat_bubble.h"

#include "m_play.h"
#include "m_msg.h"
#include "m_font.h"
#include "m_demo.h"
#include "m_event.h"
#include "m_common_data.h"
#include "m_private.h"
#include "pc_net.h"
#include <string.h>

#define mCB_QUEUE     8
#define mCB_TEXT_MAX  ACNET_CHAT_LEN
/* Line width limit in mFont_GetStringWidth units. Those are about 2.6 times
 * the pixels the window's text is drawn at (measured on screen: 17 narrow
 * characters were 210 units and 80 px). 500 ran a hair past the window's
 * right edge; 440 ends where a villager's longest lines do. */
#define mCB_LINE_PX   440.0f
#define mCB_MAX_LINES mMsg_MAX_LINE

typedef struct {
    int  slot;
    char text[mCB_TEXT_MAX + 1];
} mCB_msg_c;

static mCB_msg_c S_queue[mCB_QUEUE];
static int       S_queue_len;
static int       S_showing;      /* our message is in the window */
static int       S_showing_slot;

/* ASCII to the game's character set: letters, digits and the punctuation
 * the font has keep their codes (the table the PC text editor uses); the
 * few it lacks become '?'. */
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

extern void mCB_say(int slot, const char* ascii) {
    mCB_msg_c* m;
    if (slot < 0 || slot >= ACNET_MAX_PLAYERS || ascii == NULL || ascii[0] == '\0') {
        return;
    }
    if (S_queue_len == mCB_QUEUE) { /* drop the oldest */
        memmove(&S_queue[0], &S_queue[1], sizeof(S_queue[0]) * (mCB_QUEUE - 1));
        S_queue_len--;
    }
    m = &S_queue[S_queue_len++];
    m->slot = slot;
    strncpy(m->text, ascii, mCB_TEXT_MAX);
    m->text[mCB_TEXT_MAX] = '\0';
}

extern int mCB_active(int slot) {
    int i;
    if (S_showing && S_showing_slot == slot) {
        return 1;
    }
    for (i = 0; i < S_queue_len; i++) {
        if (S_queue[i].slot == slot) return 1;
    }
    return 0;
}

/* The name on the tag: the resident's saved character name (already in the
 * game's character set), else their login name. */
static int mCB_name(int slot, u8* out) {
    Private_c* priv = Save_GetPointer(private_data[slot]);
    int len = 0;

    if (mPr_NullCheckPersonalID(&priv->player_ID) == FALSE) {
        int i;
        memcpy(out, priv->player_ID.player_name, PLAYER_NAME_LEN);
        for (i = PLAYER_NAME_LEN; i > 0 && out[i - 1] == CHAR_SPACE; i--) {
        }
        len = i;
    }
    if (len == 0) {
        const char* login = pc_net_peer_name(slot);
        for (len = 0; login != NULL && login[len] != '\0' && len < PLAYER_NAME_LEN; len++) {
            out[len] = mCB_char(login[len]);
        }
    }
    return len;
}

/* Leave the window as we found it, for the next villager who talks. */
static void mCB_reset(mMsg_Window_c* msg_p) {
    mMsg_chat_name_len = 0;
    mMsg_chat_text_len = 0;
    mMsg_chat_body_scale_y = 1.0f;
    mMsg_chat_name_shift = 0.0f;
    msg_p->text_lines = mMsg_MAX_LINE;
    mMsg_Unset_LockContinue(msg_p);
    S_showing = 0;
}

/* Wrap the message onto up to four lines of the window's width and build
 * the message bytes: text, new lines, then the timed-end and end codes.
 * *used_out is how many of the ASCII characters fitted; the rest goes in
 * the next window. */
static int mCB_build(const char* ascii, int* lines_out, int* hold_out, int* used_out) {
    u8 text[mCB_TEXT_MAX + 1];
    int text_len = 0, i, pos = 0, lines = 0, out = 0;

    for (i = 0; ascii[i] != '\0' && text_len < mCB_TEXT_MAX; i++) {
        text[text_len++] = mCB_char(ascii[i]);
    }
    while (pos < text_len && lines < mCB_MAX_LINES) {
        int len = 0, last_space = -1;
        while (pos < text_len && text[pos] == CHAR_SPACE) pos++;
        while (pos + len < text_len) {
            if (text[pos + len] == CHAR_SPACE) last_space = len;
            if (len > 0 && (f32)mFont_GetStringWidth(text + pos, len + 1, FALSE) > mCB_LINE_PX) break;
            len++;
        }
        if (pos + len < text_len && last_space > 0 && text[pos + len] != CHAR_SPACE) {
            len = last_space; /* break between words */
        }
        if (len <= 0) len = text_len - pos;
        if (lines > 0) mMsg_chat_text[out++] = CHAR_NEW_LINE;
        memcpy(mMsg_chat_text + out, text + pos, (size_t)len);
        out += len;
        pos += len;
        lines++;
    }
    *used_out = pos;
    /* Closes by itself: the timed-end code's unit is four frames; two
     * seconds plus a character's worth each, up to about seventeen. */
    {
        int hold = 30 + pos;
        if (hold > 255) hold = 255;
        mMsg_chat_text[out++] = CHAR_CONTROL_CODE;
        mMsg_chat_text[out++] = mFont_CONT_CODE_MSG_TIME_END;
        mMsg_chat_text[out++] = (u8)hold;
        *hold_out = hold;
    }
    mMsg_chat_text[out++] = CHAR_CONTROL_CODE;
    mMsg_chat_text[out++] = mFont_CONT_CODE_LAST;
    *lines_out = lines > 0 ? lines : 1;
    return out;
}

extern void mCB_move(GAME_PLAY* play) {
    mMsg_Window_c* msg_p = mMsg_Get_base_window_p();
    mCB_msg_c m;
    Private_c* priv;
    rgba_t color;
    int lines, hold, used;
    f32 height;
    (void)play;

    if (S_showing) {
        /* Ours until it has gone, or until a villager took the window. */
        if (mMsg_Check_main_hide(msg_p) || msg_p->msg_data == NULL || msg_p->msg_data->msg_no != mMsg_CHAT_MSG_NO) {
            mCB_reset(msg_p);
        } else {
            return;
        }
    }
    if (S_queue_len == 0) {
        return;
    }
    /* Only when the window is free and no scene is being played. */
    if (!mMsg_Check_main_hide(msg_p) || mDemo_CheckDemo() || mEv_IsTitleDemo() || pc_net_local_slot() < 0) {
        return;
    }

    m = S_queue[0];
    memmove(&S_queue[0], &S_queue[1], sizeof(S_queue[0]) * (mCB_QUEUE - 1));
    S_queue_len--;

    mMsg_chat_text_len = mCB_build(m.text, &lines, &hold, &used);
    mMsg_chat_name_len = mCB_name(m.slot, mMsg_chat_name);
    if (used > 0 && m.text[used] != '\0') {
        /* More than four lines: the rest follows in the next window, like
         * a villager's next page. Back to the front of the queue. */
        const char* rest = m.text + used;
        while (*rest == ' ') rest++;
        if (*rest != '\0' && S_queue_len < mCB_QUEUE) {
            memmove(&S_queue[1], &S_queue[0], sizeof(S_queue[0]) * (size_t)S_queue_len);
            S_queue[0].slot = m.slot;
            strncpy(S_queue[0].text, rest, mCB_TEXT_MAX);
            S_queue[0].text[mCB_TEXT_MAX] = '\0';
            S_queue_len++;
        }
    }

    /* A window as tall as the text: four lines are the full 96; the name tag
     * moves down with the top edge. */
    height = 16.0f * (f32)lines + 32.0f;
    mMsg_chat_body_scale_y = height / 96.0f;
    mMsg_chat_name_shift = (96.0f - height) * 0.5f;
    msg_p->text_lines = lines;

    /* The tag colours the game gives a boy or a girl. */
    priv = Save_GetPointer(private_data[m.slot]);
    if (mPr_NullCheckPersonalID(&priv->player_ID) == FALSE && priv->gender == mPr_SEX_FEMALE) {
        msg_p->name_background_color.r = 235; msg_p->name_background_color.g = 140; msg_p->name_background_color.b = 210;
        msg_p->name_text_color.r = 45; msg_p->name_text_color.g = 0; msg_p->name_text_color.b = 30;
    } else {
        msg_p->name_background_color.r = 70; msg_p->name_background_color.g = 245; msg_p->name_background_color.b = 255;
        msg_p->name_text_color.r = 0; msg_p->name_text_color.g = 0; msg_p->name_text_color.b = 15;
    }
    msg_p->name_background_color.a = 255;
    msg_p->name_text_color.a = 255;

    color.r = 235; color.g = 255; color.b = 235; color.a = 255; /* the plain window */
    if (mMsg_request_main_appear(msg_p, NULL, TRUE, &color, mMsg_CHAT_MSG_NO, 5)) {
        mMsg_Set_LockContinue(msg_p); /* no continue button; A and B leave it alone */
        S_showing = 1;
        S_showing_slot = m.slot;
    } else {
        mCB_reset(msg_p);
    }
}

#endif /* TARGET_PC */
