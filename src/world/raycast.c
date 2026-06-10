#include "world/raycast.h"
#include "world/world.h"
#include "config.h"
#include <raymath.h>
#include <math.h>

// World space here: x/z are map coordinates * WORLD_SCALE, y is height
// (heights are stored unscaled - see SectorToWorld). The ray parameter t is
// world-units of distance, and the same t indexes the ray's 2D map-space
// shadow, so wall crossings and plane crossings compare directly.

#define RAY_EPS    1e-4f
#define MAX_VISITS 256     // sector-crossing guard against degenerate loops

static float Cross2(Vector2 p, Vector2 q) { return p.x * q.y - p.y * q.x; }

// Solve ray.y(t) == sector plane height along the ray, within (tmin, tmax).
// WorldFloorZAt/WorldCeilZAt are affine over the map, so the height along
// the ray is exactly linear in t: two samples pin it down, slopes included.
static bool SolvePlane(int s, bool ceil, Vector3 origin, Vector3 dir,
                       Vector2 m0, Vector2 md, float tmin, float tmax,
                       float *out_t) {
    Vector2 m1 = Vector2Add(m0, md);
    float h0 = ceil ? WorldCeilZAt(s, m0) : WorldFloorZAt(s, m0);
    float h1 = ceil ? WorldCeilZAt(s, m1) : WorldFloorZAt(s, m1);
    float denom = dir.y - (h1 - h0);
    if (fabsf(denom) < 1e-7f) return false;        // ray parallel to the plane
    float t = (h0 - origin.y) / denom;
    if (t <= tmin + RAY_EPS || t >= tmax) return false;
    *out_t = t;
    return true;
}

bool WorldRaycast(Vector3 origin, Vector3 dir, float max_dist, RayHit *hit) {
    *hit = (RayHit){ .kind = RAY_HIT_NONE, .sector = -1, .wall = -1 };

    float dlen = Vector3Length(dir);
    if (dlen < RAY_EPS || max_dist <= 0.0f) return false;
    dir = Vector3Scale(dir, 1.0f / dlen);

    Vector2 m0 = { origin.x / WORLD_SCALE, origin.z / WORLD_SCALE };
    Vector2 md = { dir.x / WORLD_SCALE,    dir.z / WORLD_SCALE };

    int s = WorldSectorAt(m0);
    if (s < 0) return false;

    float tmin = 0.0f;
    for (int visit = 0; visit < MAX_VISITS; visit++) {
        // Nearest wall crossing of this sector's loop beyond tmin. The loop
        // is closed, so the first crossing ahead of us is where we leave.
        float t_wall = max_dist;
        int   w_hit  = -1;
        int start = sectors[s].wall_start, n = sectors[s].wall_count;
        for (int i = 0; i < n; i++) {
            int w = start + i;
            Vector2 a  = vertices[walls[w].point_start].points;
            Vector2 b  = vertices[walls[w].point_end].points;
            Vector2 ab = Vector2Subtract(b, a);
            float denom = Cross2(md, ab);
            if (fabsf(denom) < 1e-9f) continue;            // parallel
            Vector2 am = Vector2Subtract(a, m0);
            float t = Cross2(am, ab) / denom;
            float u = Cross2(am, md) / denom;
            if (u < -1e-4f || u > 1.0f + 1e-4f) continue;  // misses the segment
            if (t <= tmin + RAY_EPS || t >= t_wall) continue;
            t_wall = t;
            w_hit  = w;
        }

        // Floor/ceiling crossing inside the span we own, (tmin, t_wall).
        float t_f, t_c;
        bool hf = SolvePlane(s, false, origin, dir, m0, md, tmin, t_wall, &t_f);
        bool hc = SolvePlane(s, true,  origin, dir, m0, md, tmin, t_wall, &t_c);
        if (hf || hc) {
            bool floor = hf && (!hc || t_f < t_c);
            float t = floor ? t_f : t_c;
            Vector3 nrm = floor ? WorldFloorNormal(s) : WorldCeilNormal(s);
            if (Vector3DotProduct(nrm, dir) > 0.0f) nrm = Vector3Negate(nrm);
            hit->kind   = floor ? RAY_HIT_FLOOR : RAY_HIT_CEILING;
            hit->dist   = t;
            hit->pos    = Vector3Add(origin, Vector3Scale(dir, t));
            hit->normal = nrm;
            hit->sector = s;
            return true;
        }

        if (w_hit < 0 || t_wall >= max_dist) return false; // escaped / out of range

        Vector3 p  = Vector3Add(origin, Vector3Scale(dir, t_wall));
        Vector2 pm = { p.x / WORLD_SCALE, p.z / WORLD_SCALE };

        // Portal: pass through when the ray height is inside the open window
        // (above both floors, below both ceilings, slope-aware). Outside the
        // window the crossing lands on a step face, which is a wall hit.
        int ns = walls[w_hit].next_sector;
        if (ns != NO_LINK) {
            float lo = fmaxf(WorldFloorZAt(s, pm), WorldFloorZAt(ns, pm));
            float hi = fminf(WorldCeilZAt(s, pm),  WorldCeilZAt(ns, pm));
            if (p.y > lo + RAY_EPS && p.y < hi - RAY_EPS) {
                s    = ns;
                tmin = t_wall;
                continue;
            }
        }

        Vector2 a = vertices[walls[w_hit].point_start].points;
        Vector2 b = vertices[walls[w_hit].point_end].points;
        Vector2 e = Vector2Normalize(Vector2Subtract(b, a));
        Vector2 n2 = { -e.y, e.x };
        if (n2.x * md.x + n2.y * md.y > 0.0f) n2 = (Vector2){ -n2.x, -n2.y };

        hit->kind   = RAY_HIT_WALL;
        hit->dist   = t_wall;
        hit->pos    = p;
        hit->normal = (Vector3){ n2.x, 0.0f, n2.y };
        hit->sector = s;
        hit->wall   = w_hit;
        return true;
    }
    return false;
}
