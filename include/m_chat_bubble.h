/* m_chat_bubble.h - chat messages in the game's own thought bubble
 * (multiplayer fork, PC build only).
 *
 * The bubble is the one the game shows over your head with an item's name
 * when you stand on it (m_watch_my_step.c, the "fki_win" models): the two
 * dots pop in one after another, the bubble grows with the text, the text is
 * the game font, and it fades out. One instance per resident, anchored to
 * that resident's character (ours) or their puppet, so it follows them. A
 * message longer than a bubble is shown a page at a time, like the game's
 * own dialogue.
 */
#ifndef M_CHAT_BUBBLE_H
#define M_CHAT_BUBBLE_H

#include "types.h"
#include "m_play.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show `ascii` (up to 96 characters) over resident `slot`. */
extern void mCB_say(int slot, const char* ascii);

/* 1 while resident `slot` has a bubble up (or fading). */
extern int mCB_active(int slot);
/* 1 if that bubble was drawn this frame (the speaker is in our scene, on
 * screen). The overlay shows the line in a corner otherwise. */
extern int mCB_visible(int slot);

/* Per play frame, next to watch_my_step_move / watch_my_step_draw. */
extern void mCB_move(GAME_PLAY* play);
extern void mCB_draw(GAME_PLAY* play);

#ifdef __cplusplus
}
#endif

#endif /* M_CHAT_BUBBLE_H */
