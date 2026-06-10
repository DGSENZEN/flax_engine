#include "renderer/billboard.h"
#include "rlgl.h"
#include <raymath.h>
#include <math.h>

// Two tricks make sprites survive free-look without artifacts:
//
// PARTIAL TILT - a fully Y-locked sprite goes edge-on (invisible) when the
// camera pitches steeply; a fully camera-facing one lies down flat and digs
// its edges into floors and walls. Leaning by a *fraction* of the camera
// pitch keeps sprites upright-looking at play angles and still face-on
// enough at extremes: at an 89-degree look-down a 0.55 factor leaves the
// sprite ~40 degrees open instead of invisible.
//
// DEPTH PULL - the quad is wider than anything's collision radius, so
// sprites hugging walls would sink into them. Sliding the quad toward the
// camera and shrinking it by the same perspective ratio renders identically
// but moves its depth off the geometry behind it.
#define BB_TILT 0.55f
#define BB_PULL 1.8f       // world units, clamped for very close sprites

void DrawSpriteBillboard(Camera camera, Texture2D tex, Rectangle source,
                         Vector3 center, Vector2 size, bool mirror, Color tint) {
    Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));

    // horizontal right + horizontal forward (guarded for straight up/down)
    Vector3 right = { -fwd.z, 0.0f, fwd.x };
    float rlen = sqrtf(right.x * right.x + right.z * right.z);
    Vector3 fh;
    if (rlen > 1e-4f) {
        right.x /= rlen; right.z /= rlen;
        fh = (Vector3){ fwd.x / rlen, 0.0f, fwd.z / rlen };
    } else {
        right = (Vector3){ 1.0f, 0.0f, 0.0f };
        fh    = (Vector3){ 0.0f, 0.0f, 1.0f };
    }

    // lean toward the camera by a fraction of the pitch
    float tilt = -asinf(Clamp(fwd.y, -1.0f, 1.0f)) * BB_TILT;
    float ct = cosf(tilt), st = sinf(tilt);
    Vector3 up = { -fh.x * st, ct, -fh.z * st };

    // pull toward the camera, shrink by the same perspective ratio
    Vector3 toCam = Vector3Subtract(camera.position, center);
    float d = Vector3Length(toCam);
    if (d > 0.01f) {
        float pull = fminf(BB_PULL, d * 0.45f);
        center = Vector3Add(center, Vector3Scale(toCam, pull / d));
        float k = (d - pull) / d;
        size.x *= k;
        size.y *= k;
    }

    Vector3 r = Vector3Scale(right, size.x * 0.5f);
    Vector3 u = Vector3Scale(up,    size.y * 0.5f);

    float u0 = source.x / (float)tex.width;
    float u1 = (source.x + source.width) / (float)tex.width;
    if (mirror) { float t = u0; u0 = u1; u1 = t; }
    float v0 = source.y / (float)tex.height;
    float v1 = (source.y + source.height) / (float)tex.height;

    Vector3 bl = Vector3Subtract(Vector3Subtract(center, r), u);
    Vector3 br = Vector3Subtract(Vector3Add(center, r), u);
    Vector3 tr = Vector3Add(Vector3Add(center, r), u);
    Vector3 tl = Vector3Add(Vector3Subtract(center, r), u);

    rlSetTexture(tex.id);
    rlBegin(RL_QUADS);
    rlColor4ub(tint.r, tint.g, tint.b, tint.a);
    // bl -> br -> tr -> tl is counter-clockwise from the camera: front-facing
    rlTexCoord2f(u0, v1); rlVertex3f(bl.x, bl.y, bl.z);
    rlTexCoord2f(u1, v1); rlVertex3f(br.x, br.y, br.z);
    rlTexCoord2f(u1, v0); rlVertex3f(tr.x, tr.y, tr.z);
    rlTexCoord2f(u0, v0); rlVertex3f(tl.x, tl.y, tl.z);
    rlEnd();
    rlSetTexture(0);
}
