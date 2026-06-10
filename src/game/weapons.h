#ifndef FLAX_WEAPONS_H
#define FLAX_WEAPONS_H

#include "raylib.h"
#include <stdbool.h>

// ===========================================================================
// Flax Engine - weapons and ballistic effects.
//
// Three test weapons (keys 1/2/3) over shared primitives:
//   1 PITCHFORK  melee ray, quiet, lunge animation
//   2 SAWED-OFF  7-pellet spread + reload animation, loud
//   3 TOMMY GUN  fast auto hitscan
//   RMB          pooled projectile bolt, any weapon
//
// All effects live in fixed pools, recycled oldest-first. No allocation.
// The HUD gun sprite owns the muzzle: the flash anchors to the sprite and
// the 3D shot origin is that anchor unprojected through the camera.
// ===========================================================================

void WeaponsInit(void);    // call after TexturesInit (loads sheets, resolves fx)

// One tick: weapon switching, fire input, projectile/effect simulation.
// Needs the full camera to unproject the muzzle anchor.
void WeaponsUpdate(float dt, Camera camera);

// Tracers, bolts and puffs. Call inside BeginMode3D, after DecalsDraw.
void WeaponsDraw3D(Camera camera);

// First-person view model + muzzle flash. Screen space, after the 3D pass.
void WeaponsDrawHUD(void);

// Current weapon's display name for the HUD.
const char *WeaponName(void);

// Ammo inventory. AmmoType lives in world/world.h because map entities store
// those ids in their data field.
bool WeaponsGiveAmmo(int ammo_type, int amount);
int  WeaponsAmmo(int ammo_type);
int  WeaponsAmmoMax(int ammo_type);
const char *WeaponAmmoText(void);

#endif // FLAX_WEAPONS_H
