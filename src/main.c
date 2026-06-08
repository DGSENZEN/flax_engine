#include "raylib.h"
#include "rcamera.h"
#include "map.h"
#include "renderer/sectors.h"
#include "main.h"
#include <math.h>
#include <raymath.h>
#include <stdbool.h>

#define SCREEN_HEIGHT 820
#define SCREEN_WIDTH 1460
#define DOS_RES_X 320
#define DOS_RES_Y 200
#define TILE_SIZE 2.0f
#define MOVEMENT_SPEED 4.0f
#define SPRINT_SPEED 2.0f

typedef struct {
    Vector3 position;
    float yaw;
    float moveSpeed;
    float rotationSpeed;
    float playerRadius;
} Player3D;

Camera setupCamera(){
    Camera camera = {0};
    camera.position = (Vector3){0};
    camera.target = (Vector3){0};
    camera.up = (Vector3){0.0f, 1.0f, 0.0f};
    camera.fovy = 65.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    return camera;
};

Player3D setupPlayer(){
    Player3D player = {
        .position = {2.0f, 0.9f, 2.0f},
        .yaw = 0.0f,
        .moveSpeed = 4.0f,
        .rotationSpeed = 2.5f,
        .playerRadius = 0.4f,
    };
    return player;
};

void debugMovement(Camera *camera){
    
    DrawText(TextFormat("Current Position: (X: %06.3f, Y: %06.3f, Z: %06.3f)", camera->position.x, camera->position.y, camera->position.z), 15, 15, 20, BLACK);
    DrawText(TextFormat("Target: (X: %06.3f, Y: %06.3f, Z: %06.3f)", camera->target.x, camera->target.y, camera->target.z), 30, 30, 20, BLACK);

};


int main(void) {
    
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Flax Engine");

    Camera camera = setupCamera();
    Player3D player = setupPlayer();
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
    BuildSectorMeshes();
    while (!WindowShouldClose()) {

        if (IsKeyPressed(KEY_M)) {
            if (currentScr == GAME) {
                currentScr = MAP_EDITOR;
                ResetMapEditorCamera();
                EnableCursor();
            } else {
                currentScr = GAME;
                DisableCursor();
                BuildSectorMeshes();
                player.position = SectorToWorld(playerStart, 0.6f);
                player.yaw = playerStartYaw;
            }
        }

        BeginDrawing();
        if (currentScr == MAP_EDITOR){
            ClearBackground(GetColor(0x181818FF));
            DrawMapEditor();
        } 
        else if (currentScr == GAME) {
        ClearBackground(RAYWHITE);
        float dt = GetFrameTime();
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) player.yaw -= player.rotationSpeed * dt;
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) player.yaw += player.rotationSpeed * dt;

        Vector3 forward = {cosf(player.yaw), 0.0f, sinf(player.yaw)};
        Vector3 forwardSprint = {.x= cosf(player.yaw) * SPRINT_SPEED, .y = 0.0f, .z = sinf(player.yaw) * SPRINT_SPEED};
        Vector3 right = {-sinf(player.yaw), 0.0f, cosf(player.yaw)};
        Vector3 moveDir = {0};

        //source rec for curr frame slice
        Rectangle sourceRec = { 0 * frameWidth, 0.0f, frameWidth, frameHeight};
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))   moveDir = Vector3Add(moveDir, forward);
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) && IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) moveDir = Vector3Add(moveDir, forwardSprint);
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) moveDir = Vector3Subtract(moveDir, forward);

        Vector2 bob = {0};
        if (Vector3Length(moveDir) > 0.0f) {
            bob.x = sinf(GetTime() * 10.0f) * 4.0f;
            bob.y = cosf(GetTime() * 20.0f) * 6.0f;
        }
        Rectangle destRec = {weaponScreenPos.x + bob.x, weaponScreenPos.y + bob.y, frameWidth * texScale, frameHeight * texScale};

        if (Vector3Length(moveDir) > 0.0f) {
            moveDir = Vector3Normalize(moveDir);
            Vector3 velocity = Vector3Scale(moveDir, player.moveSpeed * dt);
            player.position.x += velocity.x;
            player.position.z += velocity.z;
        }

        camera.position = player.position;

        camera.target.x = player.position.x + cosf(player.yaw);
        camera.target.y = player.position.y;
        camera.target.z = player.position.z + sinf(player.yaw);
        // loop to draw the map
        BeginTextureMode(dosResCanvas);
        ClearBackground(RAYWHITE);
        BeginMode3D(camera);
        DrawSectorWorld();
        EndMode3D();
        EndTextureMode();

        ClearBackground(RAYWHITE);
        DrawTexturePro(dosResCanvas.texture, 
            (Rectangle){0, 0, (float)dosResCanvas.texture.width, (float)-dosResCanvas.texture.height},
            (Rectangle){0, 0, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT},
            (Vector2){0,0}, 0.0f, WHITE);

        DrawTexturePro(weaponTex, sourceRec, destRec, (Vector2){6, -2}, 0.0f, WHITE);
        debugMovement(&camera);
        DrawText(TextFormat("sectors:%d walls:%d verts:%d", sector_counter, wall_counter, vertex_counter),
         15, 60, 20, RED);

        }
        EndDrawing();
    }

    UnloadTexture(weaponTex);
    UnloadRenderTexture(dosResCanvas);
    UnloadSectorMeshes();
    CloseWindow();
    return 0;
}