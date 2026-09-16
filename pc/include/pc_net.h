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
#include "protocol.h"

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

/* The resident slot the server assigned this client (0..3), or -1. */
int pc_net_assigned_slot(void);

/* The game reports which save block (Save_t.private_data index) its
 * character lives in, once it knows (after player select). A resident's slot
 * must equal that index; if the server had us elsewhere this asks it to move
 * us. Cheap to call every frame; only talks to the server when it matters. */
void pc_net_set_player_no(int player_no);

/* ----- Step 2: player-state stream -------------------------------------- *
 * The game fills in the local player's position, facing and animation state
 * once per frame (everything except client_id, slot and seq, which this
 * layer owns) and this layer sends it at up to 30 Hz. */
void pc_net_send_player_state(const acnet_player_state_t* state);

/* This client's own resident slot (0..3), or -1 when not connected. */
int pc_net_local_slot(void);

/* Number of other residents currently present (fresh state received). */
int pc_net_remote_count(void);

/* Latest known state of resident `slot`: returns 1 and fills *out if that
 * resident is present (a state arrived within the last few seconds), else 0.
 * The game drives that resident's puppet from it. */
int pc_net_get_remote_state(int slot, acnet_player_state_t* out);

/* Live resident sync. If another resident's saved character and house have
 * arrived since the last call, copy them out (big-endian, exactly as in the
 * town blob) and return 1. The caller writes them over that resident's
 * blocks in the running save and byte-swaps them in place. Never fires for
 * the local player's own slot. */
/* 1 when other residents should be drawn (settings.ini show_other_players).
 * Off leaves the shared town, saving and live resident sync fully working. */
int pc_net_puppets_enabled(void);

/* Land relay (step 3). The game sends the field-item cells that changed
 * since the last frame and takes the cells other residents changed. */
void pc_net_send_land_cells(const acnet_land_cell_t* cells, int count);
int  pc_net_take_land_cells(acnet_land_cell_t* out, int max);

/* Which Save_t.homes[] block a pending update for `slot` belongs to, or -1
 * when nothing is pending. 0xFF (an older server) means "use the arrangement". */
int pc_net_resident_update_house(int slot);
int pc_net_take_resident_update(int slot, void* private_out, size_t private_len, void* home_out,
                                size_t home_len);

/* ----- Step 3: villagers ------------------------------------------------ *
 * This client's id as the server knows it (what NPC states are stamped
 * with, and what decides ownership ties), or -1 when not connected. */
int pc_net_self_id(void);
/* Stream the villagers this game owns (up to ACNET_NPC_MAX per call). */
void pc_net_send_npc_states(const acnet_npc_state_t* states, int count);
/* Latest streamed state of villager npc_id and who sent it: 1 if one arrived
 * within the last few seconds, else 0. */
int  pc_net_get_npc_state(unsigned npc_id, acnet_npc_state_t* out, int* from_client);

/* ----- Step 3: shared weather ------------------------------------------- *
 * 1 when this client is the world authority (the oldest connected game). */
int pc_net_is_authority(void);
/* 1 when the local game must NOT run the world's own clockwork itself --
 * rolling weather, the daily land renewal -- because it is connected and
 * another game (the authority) does it and sends the results. */
int pc_net_world_is_remote(void);
#define pc_net_weather_is_remote pc_net_world_is_remote
/* The authority reports its weather (and its land-renewal timestamp, raw
 * bytes) every frame; this sends them when something changed or every few
 * seconds. Others take what arrived, once per message. */
void pc_net_send_weather(int type, int intensity, const void* grow_renew_time, size_t grow_renew_size);
int  pc_net_take_weather(int* type, int* intensity, void* grow_renew_time_out, size_t grow_renew_size);

/* ----- Step 4: chat and clock ------------------------------------------- */
void pc_net_send_chat(const char* text);
/* Returns 1 and fills out_slot/out_text if a chat line is waiting, else 0. */
int  pc_net_poll_chat(int* out_slot, char* out_text, int out_size);
/* The login name of the resident in `slot` ("" if none / not connected). */
const char* pc_net_peer_name(int slot);
/* How far the host's wall clock is ahead of this PC's, in ms (negative:
 * behind), or 0 when not connected. The town has one clock, the host's:
 * lb_rtc.c adds this to the hardware time on the PC build. Learned at
 * login and refreshed by a ping every minute. */
long long pc_net_server_clock_skew_ms(void);

/* ----- Step 4: resident push -------------------------------------------- *
 * Send our own character and house blocks (big-endian, as in the town blob)
 * to the server without a save, so the others see a brand-new character,
 * a new shirt or moved furniture at once. Cheap to call every frame: it
 * hashes the blocks and only sends when they changed, at most once every
 * 5 s. force sends even if unchanged (subject to the rate limit). Returns 1
 * when a push went out. */
int pc_net_push_own_blocks(const void* private_be, size_t private_len, const void* home_be, size_t home_len,
                           int house, int force);

#ifdef __cplusplus
}
#endif

#endif /* PC_NET_H */
