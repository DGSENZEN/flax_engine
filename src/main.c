#include "raylib.h"
#include "rcamera.h"
#include "map.h"
#include "renderer/sectors.h"
#include "renderer/textures.h"
#include "io/flaxmap.h"
#include "world/raycast.h"
#include "game/player.h"
#include "game/weapons.h"
#include "game/enemies.h"
#include "entity/entity.h"
#include "renderer/decals.h"
#include "renderer/lights.h"
#include "core/log.h"
#include "core/profiler.h"
#include "main.h"
#include "config.h"
#include <math.h>
#include <raymath.h>
#include <stdbool.h>

// Low-res canvas. 320x180 matches the window's 16:9-ish aspect, so the
// upscale is uniform - a 320x200 canvas here gets stretched ~11% wider,
// which warps the view against the aim and the projected muzzle flash.
#define DOS_RES_X 320
#define DOS_RES_Y 200

Camera setupCamera(){
    Camera camera = {0};
    camera.position = (Vector3){0};
    camera.target = (Vector3){0};
    camera.up = (Vector3){0.0f, 1.0f, 0.0f};
    camera.fovy = 65.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    return camera;
};

void debugMovement(Camera *camera){
    DrawText(TextFormat("Current Position: (X: %06.3f, Y: %06.3f, Z: %06.3f)", camera->position.x, camera->position.y, camera->position.z), 15, 15, 20, BLACK);
    DrawText(TextFormat("Target: (X: %06.3f, Y: %06.3f, Z: %06.3f)", camera->target.x, camera->target.y, camera->target.z), 30, 30, 20, BLACK);
};

void drawCrosshair(void){
    int cx = SCREEN_WIDTH / 2, cy = SCREEN_HEIGHT / 2;
    Color shadow = Fade(BLACK, 0.6f);
    // ticks with a 1px drop shadow so they read on bright and dark surfaces
    DrawRectangle(cx - 9, cy,     6, 2, shadow); DrawRectangle(cx - 10, cy - 1, 6, 2, RAYWHITE);
    DrawRectangle(cx + 5, cy,     6, 2, shadow); DrawRectangle(cx + 4,  cy - 1, 6, 2, RAYWHITE);
    DrawRectangle(cx,     cy - 9, 2, 6, shadow); DrawRectangle(cx - 1,  cy - 10, 2, 6, RAYWHITE);
    DrawRectangle(cx,     cy + 5, 2, 6, shadow); DrawRectangle(cx - 1,  cy + 4, 2, 6, RAYWHITE);
};


int main(void) {

    FlaxLogInit(FLAX_ASSET_DIR "/../flax.log");   // before InitWindow: catches raylib boot
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Flax Engine");

    Camera camera = setupCamera();
    SetTargetFPS(60);
    FlaxScreen currentScr = GAME;

    //setting up the low-res canvas for dos-like rendering
    RenderTexture2D dosResCanvas = LoadRenderTexture(DOS_RES_X, DOS_RES_Y);
    SetTextureFilter(dosResCanvas.texture, TEXTURE_FILTER_POINT);
    DisableCursor();

    TexturesInit();
    LightsInit();

    // boot map: prefer the baked binary (the runtime artifact), fall back to
    // the text source so a freshly edited map still loads without a bake step
    if (MapBakedLoad(FLAXMAP_BAKED_PATH)) {
        FLOG_INFO(LOGCAT_IO, "boot map: %s (%d sectors, %d walls)", FLAXMAP_BAKED_PATH, sector_counter, wall_counter);
    } else if (MapSourceLoad(FLAXMAP_SOURCE_PATH)) {
        FLOG_INFO(LOGCAT_IO, "boot map: %s (text fallback, %d sectors)", FLAXMAP_SOURCE_PATH, sector_counter);
    } else {
        FLOG_WARN(LOGCAT_IO, "no boot map found, starting empty");
    }
    BuildSectorMeshes();
    PlayerSpawn(playerStart, playerStartYaw);
    WeaponsInit();
    EnemiesInit();
    EntitySystemInit();

    while (!WindowShouldClose()) {
        ProfFrameStart();

        if (IsKeyPressed(KEY_F1)) ProfToggle();
        if (IsKeyPressed(KEY_F2)) SetTargetFPS(ProfCycleFpsCap());

        if (IsKeyPressed(KEY_M)) {
            if (currentScr == GAME) {
                currentScr = MAP_EDITOR;
                ResetMapEditorCamera();
                EnableCursor();
                FLOG_INFO(LOGCAT_EDITOR, "entered map editor");
            } else {
                currentScr = GAME;
                DisableCursor();
                BuildSectorMeshes();
                PlayerSpawn(playerStart, playerStartYaw);
                DecalsClear();       // geometry may have changed under them
                WeaponsInit();
                EnemiesInit();
                EntitySystemInit();
                FLOG_INFO(LOGCAT_GAME, "entered game");
            }
        }

        BeginDrawing();
        if (currentScr == MAP_EDITOR){
            ClearBackground(GetColor(0x181818FF));
            PROF_BEGIN("editor");
            DrawMapEditor();
            PROF_END("editor");
        }
        else if (currentScr == GAME) {
            ClearBackground(RAYWHITE);
            float dt = GetFrameTime();

            PlayerUpdate(dt);          // movement, collision, gravity (profiled inside)
            PlayerApplyCamera(&camera);

            Vector3 viewDir = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
            LightsBeginFrame(dt);                  // weapons push dlights below
            WeaponsUpdate(dt, camera);
            EntitySystemUpdate(dt);
            EnemiesUpdate(dt);
            SectorWorldRelight();                  // CPU vertex lighting

            // T drops a test zombie where the crosshair points
            if (IsKeyPressed(KEY_T)) {
                RayHit th;
                if (WorldRaycast(camera.position, viewDir, 300.0f, &th)) {
                    Vector3 at = Vector3Add(th.pos, Vector3Scale(th.normal, 1.5f));
                    EnemySpawn((Vector2){ at.x / WORLD_SCALE, at.z / WORLD_SCALE });
                }
            }

            // world pass onto the low-res canvas
            PROF_BEGIN("world 3d");
            BeginTextureMode(dosResCanvas);
            ClearBackground(RAYWHITE);
            BeginMode3D(camera);
            DrawSectorWorld();
            DecalsDraw();
            EntitySystemDraw3D(camera);
            EnemiesDraw3D(camera);
            WeaponsDraw3D(camera);
            EndMode3D();
            EndTextureMode();
            PROF_END("world 3d");

            // upscale + HUD
            PROF_BEGIN("blit + hud");
            ClearBackground(RAYWHITE);
            DrawTexturePro(dosResCanvas.texture,
                (Rectangle){0, 0, (float)dosResCanvas.texture.width, (float)-dosResCanvas.texture.height},
                (Rectangle){0, 0, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT},
                (Vector2){0,0}, 0.0f, WHITE);

            // hurt flash: red wash over the world, under the gun
            float hurt = PlayerHurtFlash();
            if (hurt > 0.01f)
                DrawRectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, Fade(RED, 0.35f * hurt));

            WeaponsDrawHUD();
            drawCrosshair();

            // health readout, Doom-corner style
            float hp = PlayerHealth();
            Color hpCol = hp > 50 ? RAYWHITE : hp > 25 ? ORANGE : RED;
            DrawText(TextFormat("%3.0f", hp), 22, SCREEN_HEIGHT - 58, 50, Fade(BLACK, 0.5f));
            DrawText(TextFormat("%3.0f", hp), 20, SCREEN_HEIGHT - 60, 50, hpCol);
            DrawText("HEALTH", 22, SCREEN_HEIGHT - 78, 16, Fade(hpCol, 0.8f));
            DrawText(WeaponName(), 22, SCREEN_HEIGHT - 100, 16, Fade(RAYWHITE, 0.7f));
            DrawText(WeaponAmmoText(), 22, SCREEN_HEIGHT - 118, 16, Fade(RAYWHITE, 0.7f));

            debugMovement(&camera);
            DrawText(TextFormat("sectors:%d walls:%d verts:%d enemies:%d pickups:%d  [T spawn]",
                sector_counter, wall_counter, vertex_counter, EnemiesAlive(), EntityPickupsRemaining()),
                15, 60, 20, RED);
            PROF_END("blit + hud");
        }

        ProfDraw();    // F1 overlay, on top of either screen
        EndDrawing();
    }

    UnloadRenderTexture(dosResCanvas);
    UnloadSectorMeshes();
    TexturesUnload();
    CloseWindow();
    FlaxLogShutdown();
    return 0;
}
