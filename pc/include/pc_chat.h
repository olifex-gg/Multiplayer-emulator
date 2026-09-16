/* pc_chat.h - in-game text chat between the residents of a shared town
 * (multiplayer fork).
 *
 * Press T (when connected, not paused, not in the game's own text editor)
 * to open a line at the bottom of the screen, type, Enter to send, Escape
 * to cancel. Messages from everyone, including your own, are listed in the
 * top-left corner for a few seconds. While the line is open the keyboard
 * is the chat's: the game gets no button presses. Everything here is a
 * no-op in single-player.
 */
#ifndef PC_CHAT_H
#define PC_CHAT_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

struct game_s;

/* 1 while the chat line is open and taking keystrokes. */
extern int g_pc_chat_typing;

/* 1 while the game must not read the keyboard as a controller: the line is
 * open, or the Enter/Escape that just closed it is still held down (Enter is
 * also the Start button, and the inventory opened after every sent line
 * before this check existed). pc_pad.c asks every frame. */
int pc_chat_blocks_pad(void);

/* Forward an SDL event (key down, key up, text). Returns 1 if the chat
 * consumed it. */
int pc_chat_handle_event(const SDL_Event* e);

/* Append the chat overlay to the game's font display list, from graph_main
 * after game_main() returns (next to the pause menu). Also drains the
 * network's chat inbox. */
void pc_chat_draw(struct game_s* game);

#ifdef __cplusplus
}
#endif

#endif /* PC_CHAT_H */
