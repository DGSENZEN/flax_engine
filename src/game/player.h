#ifndef FLAX_PLAYER_H
#define FLAX_PLAYER_H

#include "raylib.h"
#include <stdbool.h>

// ===========================================================================
// Flax Engine - first-person player: Quake-style movement over the sector
// world. See player.c for the movement model and the collision rules.
// ===========================================================================

// Place the player at a map-space start point (editor coordinates), facing
// yaw. Resolves the spawn sector and snaps the feet to its floor.
void PlayerSpawn(Vector2 map_pos, float yaw);

// One tick: input -> acceleration -> collide-and-slide -> gravity/step.
void PlayerUpdate(float dt);

// Write eye position + look direction into the raylib camera.
void PlayerApplyCamera(Camera *camera);

// Horizontal speed in world units/s (drives the weapon bob).
float PlayerSpeedXZ(void);
bool  PlayerOnGround(void);

// Feet position in world units (enemies chase this).
Vector3 PlayerPosition(void);

// Combat state: damage respawns the player at 0 hp; hurt flash is 1 on the
// hit decaying to 0 (drives the HUD red overlay).
void  PlayerDamage(float dmg);
bool  PlayerGiveHealth(float amount);
float PlayerHealth(void);
float PlayerHurtFlash(void);

#endif // FLAX_PLAYER_H
