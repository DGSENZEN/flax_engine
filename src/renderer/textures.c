#include "renderer/textures.h"
#include "world/world.h"
#include "core/log.h"
#include <string.h>
#include <ctype.h>

// Registry storage - mirrors the world texname table's fixed-record style.
typedef struct {
    char      name[TEXNAME_LEN];
    Texture2D texture;
} RegEntry;

static RegEntry g_reg[MAX_TEXNAMES];
static int      g_reg_count = 0;

static void Canonical(char out[TEXNAME_LEN], const char *name) {
    int n = 0;
    for (const char *c = name; *c && n < TEXNAME_LEN - 1; c++)
        out[n++] = (*c == ' ') ? '_' : (char)toupper((unsigned char)*c);
    out[n] = '\0';
}

static int FindByName(const char *canon) {
    for (int i = 0; i < g_reg_count; i++)
        if (strcmp(g_reg[i].name, canon) == 0) return i;
    return -1;
}

// Register (or override) a texture under a canonical name. Takes ownership
// of the Image and unloads it after upload.
static void AddTexture(const char *name, Image img) {
    char canon[TEXNAME_LEN];
    Canonical(canon, name);

    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureFilter(t, TEXTURE_FILTER_POINT);   // no smoothing: chunky texels
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);      // required for tiling UVs

    int i = FindByName(canon);
    if (i >= 0) {                                // asset overrides built-in
        UnloadTexture(g_reg[i].texture);
        g_reg[i].texture = t;
        return;
    }
    if (g_reg_count >= MAX_TEXNAMES) { UnloadTexture(t); return; }
    strcpy(g_reg[g_reg_count].name, canon);
    g_reg[g_reg_count].texture = t;
    g_reg_count++;
}

// ---------------------------------------------------------------------------
// Built-in procedural set. 64x64, generated in a few milliseconds; these are
// placeholders with enough character to read distances and surfaces while
// real art doesn't exist yet.
// ---------------------------------------------------------------------------

static Image GenBrick(void) {
    Color mortar = { 60, 52, 48, 255 };
    Color brick  = { 144, 70, 56, 255 };
    Color brick2 = { 128, 62, 50, 255 };
    Image img = GenImageColor(64, 64, mortar);
    for (int row = 0; row < 4; row++) {
        int y = row * 16;
        int shift = (row % 2) ? 16 : 0;                  // running bond offset
        for (int col = -1; col < 3; col++) {
            int x = col * 32 + shift;
            ImageDrawRectangle(&img, x + 2, y + 2, 28, 12,
                               ((row + col) % 2) ? brick : brick2);
        }
    }
    return img;
}

static Image GenStone(void) {
    Image img = GenImageCellular(64, 64, 16);
    ImageColorTint(&img, (Color){ 150, 142, 128, 255 });
    return img;
}

static Image GenFloor(void) {
    Image img = GenImageChecked(64, 64, 16, 16,
                                (Color){ 96, 96, 104, 255 },
                                (Color){ 116, 116, 126, 255 });
    // grout lines between the tiles
    for (int i = 0; i < 64; i += 16) {
        ImageDrawRectangle(&img, i, 0, 1, 64, (Color){ 58, 58, 64, 255 });
        ImageDrawRectangle(&img, 0, i, 64, 1, (Color){ 58, 58, 64, 255 });
    }
    return img;
}

static Image GenCeil(void) {
    Image img = GenImageColor(64, 64, (Color){ 78, 70, 62, 255 });
    Image noise = GenImageWhiteNoise(64, 64, 0.4f);
    ImageColorTint(&noise, (Color){ 90, 82, 74, 255 });
    ImageAlphaMask(&noise, noise);
    ImageDraw(&img, noise, (Rectangle){ 0, 0, 64, 64 },
              (Rectangle){ 0, 0, 64, 64 }, (Color){ 255, 255, 255, 70 });
    UnloadImage(noise);
    return img;
}

static Image GenPanel(void) {
    Image img = GenImageGradientLinear(64, 64, 0,
                                       (Color){ 92, 102, 112, 255 },
                                       (Color){ 56, 64, 74, 255 });
    // horizontal seams + rivets, vaguely tech-base
    for (int y = 15; y < 64; y += 16) {
        ImageDrawRectangle(&img, 0, y, 64, 2, (Color){ 38, 44, 52, 255 });
        for (int x = 6; x < 64; x += 16)
            ImageDrawRectangle(&img, x, y - 5, 2, 2, (Color){ 140, 150, 160, 255 });
    }
    return img;
}

static Image GenDefault(void) {
    // neutral checker - the "no texture assigned yet" look, but livable
    return GenImageChecked(64, 64, 8, 8,
                           (Color){ 110, 110, 118, 255 },
                           (Color){ 88, 88, 96, 255 });
}

// ---------------------------------------------------------------------------

void TexturesInit(void) {
    g_reg_count = 0;

    AddTexture("DEFAULT", GenDefault());   // index 0, the universal fallback
    AddTexture("BRICK",   GenBrick());
    AddTexture("STONE",   GenStone());
    AddTexture("FLOOR",   GenFloor());
    AddTexture("CEIL",    GenCeil());
    AddTexture("PANEL",   GenPanel());

    int builtins = g_reg_count;
    int loaded = 0;
#ifdef FLAX_ASSET_DIR
    const char *dir = FLAX_ASSET_DIR "/textures";
    if (DirectoryExists(dir)) {
        FilePathList files = LoadDirectoryFilesEx(dir, ".png", false);
        for (unsigned int i = 0; i < files.count; i++) {
            Image img = LoadImage(files.paths[i]);
            if (img.data) {
                AddTexture(GetFileNameWithoutExt(files.paths[i]), img);
                loaded++;
            }
        }
        UnloadDirectoryFiles(files);
    }
#endif
    FLOG_INFO(LOGCAT_RENDER, "texture registry: %d built-in, %d from assets/textures (%d total)",
              builtins, loaded, g_reg_count);
}

void TexturesUnload(void) {
    for (int i = 0; i < g_reg_count; i++) UnloadTexture(g_reg[i].texture);
    g_reg_count = 0;
}

void TexturesReload(void) {
    TexturesUnload();
    TexturesInit();
}

int TexRegistryCount(void) { return g_reg_count; }

const char *TexRegistryName(int index) {
    if (index < 0 || index >= g_reg_count) return "DEFAULT";
    return g_reg[index].name;
}

Texture2D TexRegistryTexture(int index) {
    if (index < 0 || index >= g_reg_count) index = 0;
    return g_reg[index].texture;
}

int TexRegistryResolve(int world_texture_id) {
    int i = FindByName(WorldTexname(world_texture_id));
    return (i >= 0) ? i : 0;
}
