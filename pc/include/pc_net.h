/* pc_net.h - multiplayer client: connect to a town server, sync the shared
 * town, and (in later steps) stream puppets and chat.
 *
 * Step 1 scope (this file): read connection settings from settings.ini, log
 * in to the server, download the shared town onto disk before the game's
 * normal save-load path runs, and upload the town after each in-game save.
 * The puppet, chat, and world-event paths are received and counted here but
 * not yet applied to the running game; that is steps 2 and 3 in
 * docs/MULTIPLAYER.md.
 *
 * All functions are safe to call when multiplayer is disabled (no server
 * configured): they become no-ops, so the single-player build is unchanged.
 */
#ifndef PC_NET_H
#define PC_NET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 once a server address is configured and the initial login+download has
 * been attempted. 0 in pure single-player. */
int pc_net_enabled(void);

/* Read [Network] settings from settings.ini. Called once at startup before
 * the game loads its save. If a server is configured, connect, log in, and
 * write the downloaded town into save/card_a/ so the stock load path picks
 * it up. Founding a brand-new town (server has none yet) is deferred until
 * the first upload. Returns 1 if multiplayer is active. */
int pc_net_init(void);

/* Pump the network. Call once per frame from VIWaitForRetrace. Cheap and
 * safe when disabled. */
void pc_net_service(void);

/* Upload the current on-disk town (save/card_a GCI) to the server. Called
 * after the game writes its save. reason is one of ACNET_UPLOAD_*. */
void pc_net_on_saved(int reason);

/* Disconnect cleanly (uploads a final copy with reason LEAVE). */
void pc_net_shutdown(void);

/* The resident slot the server assigned this client (0..3), or -1. Used by
 * the player-select integration so the human is dropped straight onto their
 * own resident instead of the slot picker. */
int pc_net_assigned_slot(void);

/* ----- Step 2: player-state stream -------------------------------------- *
 * The game samples the local player once per frame and sends it; it reads
 * the other residents to drive puppet actors. These use plain scalars so no
 * game header has to include the wire protocol. angle_y is the game's s16
 * facing angle; anim_index/anim_frame are the current keyframe animation. */
void pc_net_send_player_state(float x, float y, float z, int angle_y, unsigned anim_index,
                              float anim_frame, unsigned item, unsigned emote, unsigned area,
                              unsigned flags);

/* This client's own resident slot (0..3), or -1 when not connected. */
int pc_net_local_slot(void);

/* Number of other residents currently present (fresh state received). */
int pc_net_remote_count(void);

/* Latest known state of resident `slot`. Fills the caller-provided fields and
 * returns 1 if that resident is present, else 0. Declared with void* so game
 * headers need not include the wire protocol; the real type is
 * acnet_player_state_t and callers in pc_net-aware code cast accordingly.
 * A thin typed accessor is provided in pc_net_puppet.h for the engine side. */
/* Live resident sync. If another resident's saved character and house have
 * arrived since the last call, copy them out (big-endian, exactly as in the
 * town blob) and return 1. The caller writes them over that resident's
 * blocks in the running save and byte-swaps them in place. Never fires for
 * the local player's own slot. */
int pc_net_take_resident_update(int slot, void* private_out, size_t private_len, void* home_out,
                                size_t home_len);

int pc_net_get_remote_fields(int slot, float* x, float* y, float* z, int* angle_y,
                             unsigned* anim_index, float* anim_frame, unsigned* item,
                             unsigned* emote, unsigned* area);

/* ----- Step 4: chat and clock ------------------------------------------- */
void pc_net_send_chat(const char* text);
/* Returns 1 and fills out_slot/out_text if a chat line is waiting, else 0. */
int  pc_net_poll_chat(int* out_slot, char* out_text, int out_size);
/* server_unix_ms - local time at login, for aligning the town clock. */
long long pc_net_server_clock_skew_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_NET_H */
