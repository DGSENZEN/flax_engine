#ifndef FLAX_ENTITY_H
#define FLAX_ENTITY_H

#include "raylib.h"

void EntitySystemInit(void);
void EntitySystemUpdate(float dt);
void EntitySystemDraw3D(Camera camera);
int  EntityPickupsRemaining(void);

#endif // FLAX_ENTITY_H
