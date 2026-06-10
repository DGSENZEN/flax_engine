#include "raylib.h"
#include "rcamera.h"
#include "map.h"
#include "renderer/sectors.h"
#include "renderer/textures.h"
#include "io/flaxmap.h"
#include "game/player.h"
#include "core/log.h"
#include "core/profiler.h"
#include "main.h"
#include "config.h"
#include <math.h>
#include <raymath.h>
#include <stdbool.h>

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


int main(void) {

    FlaxLogInit(FLAX_ASSET_DIR "/../flax.log");   // before InitWindow: catches raylib boot
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Flax Engine");

    Camera camera = setupCamera();
    SetTargetFPS(60);
    FlaxScreen currentScr = GAME;

    //setup the player sprite
    Texture2D weaponTex = LoadTexture("assets/sprites/player_test_sprite.png");
    float texScale = 4.5f;
    float frameWidth = (float) weaponTex.width;
    float frameHeight = (float) weaponTex.height;
    Vector2 weaponScreenPos = {
        (SCREEN_WIDTH - (frameWidth * texScale)),
        (SCREEN_HEIGHT - (frameHeight * texScale) + 20.0f)
    };

    //setting up the low-res canvas for dos-like rendering
    RenderTexture2D dosResCanvas = LoadRenderTexture(DOS_RES_X, DOS_RES_Y);
    SetTextureFilter(dosResCanvas.texture, TEXTURE_FILTER_POINT);
    DisableCursor();

    TexturesInit();

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

            // weapon bob from actual horizontal speed, grounded only
            Rectangle sourceRec = { 0.0f, 0.0f, frameWidth, frameHeight };
            Vector2 bob = {0};
            if (PlayerOnGround() && PlayerSpeedXZ() > 1.0f) {
                bob.x = sinf(GetTime() * 10.0f) * 4.0f;
                bob.y = cosf(GetTime() * 20.0f) * 6.0f;
            }
            Rectangle destRec = {weaponScreenPos.x + bob.x, weaponScreenPos.y + bob.y, frameWidth * texScale, frameHeight * texScale};

            // world pass onto the low-res canvas
            PROF_BEGIN("world 3d");
            BeginTextureMode(dosResCanvas);
            ClearBackground(RAYWHITE);
            BeginMode3D(camera);
            DrawSectorWorld();
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

            DrawTexturePro(weaponTex, sourceRec, destRec, (Vector2){6, -2}, 0.0f, WHITE);
            debugMovement(&camera);
            DrawText(TextFormat("sectors:%d walls:%d verts:%d", sector_counter, wall_counter, vertex_counter),
                15, 60, 20, RED);
            PROF_END("blit + hud");
        }

        ProfDraw();    // F1 overlay, on top of either screen
        EndDrawing();
    }

    UnloadTexture(weaponTex);
    UnloadRenderTexture(dosResCanvas);
    UnloadSectorMeshes();
    TexturesUnload();
    CloseWindow();
    FlaxLogShutdown();
    return 0;
}
