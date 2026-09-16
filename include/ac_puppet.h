/* ac_puppet.h - networked "puppet" residents (multiplayer fork).
 *
 * A puppet is another online resident, drawn with the player model and moved
 * by network state. Its profile and implementation live in the player
 * translation unit (m_puppet.c_inc, included by m_player.c) so it can reuse
 * the player's private draw path; this header only exposes the profile (for
 * the actor table) and the per-frame hook the local player calls.
 */
#ifndef AC_PUPPET_H
#define AC_PUPPET_H

#include "m_actor.h"
#ifdef TARGET_PC
#include "protocol.h" /* ACNET_NPC_FLAG_*, for the villager follow code */
#endif

extern ACTOR_PROFILE Puppet_Profile;

/* Called once per frame from the local Player_actor_move: sends this client's
 * player state to the server and spawns/updates/removes puppet actors for the
 * other residents. A no-op when multiplayer is not connected. */
extern void Puppet_net_update_local(ACTOR* player_actor, GAME* game);

/* Called at the end of every play frame (play_main). While the game is paused
 * (pockets open, a menu, a letter) the player's move proc does not run, so
 * this sends our state instead: the others keep seeing us standing there.
 * A no-op when the move proc ran this frame or multiplayer is off. */
extern void Puppet_net_idle(GAME* game);


/* Villager sync. Asked by a villager every frame: 1 if another resident's game
 * owns this villager right now and has told us where it is (position, facing,
 * whether it is walking, its act and ACNET_NPC_FLAG_* flags); the villager
 * then follows that instead of its own decisions. 0: run the normal AI (we
 * own it, or nobody does). */
extern int Puppet_npc_remote(ACTOR* npc, float* x, float* y, float* z, int* angle_y, int* walking, int* act,
                             int* flags);

#endif /* AC_PUPPET_H */
