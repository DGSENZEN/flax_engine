#include "raylib.h"
#include "rcamera.h"
#include "map.h"
#include "main.h"
#include <math.h>
#include <raymath.h>
#include <stdbool.h>

#define SCREEN_HEIGHT 820
#define SCREEN_WIDTH 1460
#define MAP_HEIGHT 16
#define MAP_WIDTH 16
#define DOS_RES_X 320
#define DOS_RES_Y 200
#define TILE_SIZE 2.0f
#define MOVEMENT_SPEED 4.0f


int dungeonMap[MAP_HEIGHT][MAP_WIDTH] = {
    {1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,1,0,0,0,0,0,0,1},
    {1,0,1,0,1,0,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,0,0,1,0,1},
    {1,1,0,1,1,1,1,1,0,1,0,1},
    {1,0,0,0,0,0,0,1,0,0,0,1},
    {1,0,1,1,1,1,1,1,1,1,0,1},
    {1,0,1,1,1,1,1,1,1,1,0,1},
    {1,0,0,0,1,0,0,0,0,0,0,1},
    {1,0,1,0,1,0,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,0,0,1,0,1},
    {1,1,0,1,1,1,1,1,0,1,0,1},
    {1,0,0,0,0,0,0,1,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1}
};


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
        .position = {2.0f, 0.6f, 2.0f},
        .yaw = 0.0f,
        .moveSpeed = 4.0f,
        .rotationSpeed = 2.5f,
        .playerRadius = 0.4f,
    };
    return player;
};

bool IsCollidingWithMap(Vector3 pos, float radius) {
    float checkAngles[] = {0, PI/2, 3*PI/2};

    for (int i = 0; i < 4; i++) {
        float checkX = pos.x + cosf(checkAngles[i] * radius);
        float checkZ = pos.z + sinf(checkAngles[i] * radius);

        int mapX = (int)roundf(checkX / TILE_SIZE);
        int mapZ = (int)roundf(checkZ/ TILE_SIZE);

        if (mapX < 0 || mapX >= MAP_WIDTH || mapZ < 0 || mapZ >= MAP_HEIGHT) {
            return true;
        }

        if (dungeonMap[mapZ][mapX] == 1) {
            return true;
        }
    }

    return false;
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

    //setting up cube texture
    Texture2D wallTexture = LoadTexture("assets/sprites/brickwork.png");
    Model wallCube = LoadModelFromMesh(GenMeshCube(TILE_SIZE, TILE_SIZE, TILE_SIZE));
    wallCube.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = wallTexture;

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
    while (!WindowShouldClose()) {

        if (IsKeyPressed(KEY_M)) {
            if (currentScr == GAME) {
                currentScr = MAP_EDITOR;
                ResetMapEditorCamera();
                EnableCursor();
            } else {
                currentScr = GAME;
                DisableCursor();
            }
        }

        BeginDrawing();
        if (currentScr == MAP_EDITOR){
            ClearBackground(BLACK);
            DrawMapEditor();
        } 
        else if (currentScr == GAME) {
        ClearBackground(RAYWHITE);
        float dt = GetFrameTime();
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) player.yaw -= player.rotationSpeed * dt;
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) player.yaw += player.rotationSpeed * dt;

        Vector3 forward = {cosf(player.yaw), 0.0f, sinf(player.yaw)};
        Vector3 right = {-sinf(player.yaw), 0.0f, cosf(player.yaw)};
        Vector3 moveDir = {0};

        //source rec for curr frame slice
        Rectangle sourceRec = { 0 * frameWidth, 0.0f, frameWidth, frameHeight};
        // dest rec
        Rectangle destRec = {weaponScreenPos.x, weaponScreenPos.y, frameWidth * texScale, frameHeight * texScale};
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) { 
            moveDir = Vector3Add(moveDir, forward);
            weaponScreenPos.x = sinf(GetTime() * 10.f) * 4.0f;
            weaponScreenPos.y = cosf(GetTime() * 20.0f) * 6.0f;

        }
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) {
            moveDir = Vector3Subtract(moveDir, forward);
            weaponScreenPos.x = sinf(GetTime() * 10.f) * 4.0f;
            weaponScreenPos.y = cosf(GetTime() * 20.0f) * 6.0f;

        }

        if (Vector3Length(moveDir) > 0.0f) {
            moveDir = Vector3Normalize(moveDir);

            Vector3 velocity = Vector3Scale(moveDir, player.moveSpeed * dt);

            Vector3 testPosX = player.position;
            testPosX.x += velocity.x;
            if (!IsCollidingWithMap(testPosX, player.playerRadius)) {
                player.position.x  = testPosX.x;
            }

            Vector3 testPosZ = player.position;
            testPosZ.z += velocity.z;
            if(!IsCollidingWithMap(testPosZ, player.playerRadius)){
                player.position.z = testPosZ.z;
            }
        }

        camera.position = player.position;

        camera.target.x = player.position.x + cosf(player.yaw);
        camera.target.y = player.position.y;
        camera.target.z = player.position.z + sinf(player.yaw);
        // loop to draw the map
        BeginTextureMode(dosResCanvas);
        ClearBackground(RAYWHITE);
        BeginMode3D(camera);
        for (int y = 0; y < MAP_HEIGHT; y++) {
            for (int x = 0; x < MAP_WIDTH; x++) {
                Vector3 tilePos = {x * TILE_SIZE, 0.0f, y * TILE_SIZE};

                if (dungeonMap[y][x] == 1) {
                    DrawModel(wallCube, tilePos, 1.0f , WHITE);
                } else {
                    DrawPlane((Vector3){tilePos.x, -TILE_SIZE/2, tilePos.z}, (Vector2){TILE_SIZE, TILE_SIZE}, GRAY);
                    DrawPlane((Vector3){tilePos.x, TILE_SIZE/2, tilePos.z}, (Vector2){TILE_SIZE, -TILE_SIZE}, BLACK);
                }
            }
        }
        EndMode3D();
        EndTextureMode();

        ClearBackground(RAYWHITE);
        DrawTexturePro(dosResCanvas.texture, 
            (Rectangle){0, 0, (float)dosResCanvas.texture.width, (float)-dosResCanvas.texture.height},
            (Rectangle){0, 0, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT},
            (Vector2){0,0}, 0.0f, WHITE);

        DrawTexturePro(weaponTex, sourceRec, destRec, (Vector2){6, -2}, 0.0f, WHITE);
        debugMovement(&camera);
        }
        EndDrawing();
    }

    UnloadTexture(weaponTex);
    UnloadTexture(wallTexture);
    UnloadRenderTexture(dosResCanvas);
    UnloadModel(wallCube);
    CloseWindow();
    return 0;
}