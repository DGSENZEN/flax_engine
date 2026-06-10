#ifndef FLAX_ENEMIES_H
#define FLAX_ENEMIES_H

#include "raylib.h"
#include <stdbool.h>

// ===========================================================================
// Flax Engine - test enemies (Blood axe zombie, billboarded).
//
// A fixed pool of sprite enemies for exercising the combat loop: they chase
// the player, swing when close, take hits from the weapons (headshots count
// triple and get their own death animation), and leave corpses. Rendering
// is Build-style 8-angle billboarding: 5 drawn direction rows, the other 3
// angles mirrored. Meshes for real enemies come later; this is the test rig.
// ===========================================================================

void EnemiesInit(void);        // clears the pool, spawns map ENT_SPAWNs, loads the sheet
void EnemiesUpdate(float dt);
void EnemiesDraw3D(Camera camera);

// Drop a zombie at a map-space point (floor-snapped). False if off the map
// or the pool is full.
bool EnemySpawn(Vector2 map_pos);

// Ray vs live enemies (vertical cylinders). Returns the nearest enemy index
// or -1; fills *dist and *headshot (hit in the top of the cylinder).
int EnemiesRayHit(Vector3 origin, Vector3 dir, float max_dist,
                  float *dist, bool *headshot);

void EnemyDamage(int idx, float dmg, bool headshot);
int  EnemiesAlive(void);

// Gunfire is loud: idle/wandering enemies inside the radius turn aggressive.
void EnemiesAlertAt(Vector3 pos, float radius);

#endif // FLAX_ENEMIES_H
