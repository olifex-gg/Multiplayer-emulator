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

extern ACTOR_PROFILE Puppet_Profile;

/* Called once per frame from the local Player_actor_move: sends this client's
 * player state to the server and spawns/updates/removes puppet actors for the
 * other residents. A no-op when multiplayer is not connected. */
extern void Puppet_net_update_local(ACTOR* player_actor, GAME* game);

#endif /* AC_PUPPET_H */
