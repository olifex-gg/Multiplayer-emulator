/* m_chat_bubble.h - chat messages in the game's own speech window
 * (multiplayer fork, PC build only).
 *
 * The window is the one the villagers talk in (m_msg.c): the name tag,
 * the letter-by-letter text with its voice, the pop-in and pop-out. A chat
 * message is shown in it with the sender's name on the tag, wrapped onto up
 * to four lines, the window shrunk to the number of lines, and it closes by
 * itself after a while. The reader keeps control of their character: the
 * window is opened directly, not through a conversation, so nobody is
 * frozen. A villager conversation always wins the window; chat waits.
 */
#ifndef M_CHAT_BUBBLE_H
#define M_CHAT_BUBBLE_H

#include "types.h"
#include "m_play.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show `ascii` (up to 96 characters) as said by resident `slot`. Queued if
 * the window is busy. */
extern void mCB_say(int slot, const char* ascii);

/* 1 while resident `slot`'s message is in the window or waiting for it. */
extern int mCB_active(int slot);

/* Per play frame, next to watch_my_step_move. */
extern void mCB_move(GAME_PLAY* play);

#ifdef __cplusplus
}
#endif

#endif /* M_CHAT_BUBBLE_H */
