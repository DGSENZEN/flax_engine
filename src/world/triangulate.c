#include "world/triangulate.h"
#include <math.h>
#include <stdbool.h>

// Ear clipping with hole bridging. Sizes are bounded by the world's
// fixed-array philosophy: a sector bigger than MAX_PTS points (after each
// hole adds 2 bridge duplicates) is an authoring error, not a use case.

#define MAX_PTS  512
#define EPS_AREA 1e-2f     // twice-area threshold for "degenerate/collinear"
#define EPS_PT   1e-5f     // coordinate equality

static float Cross3(Vector2 a, Vector2 b, Vector2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static float SignedArea2(const Vector2 *p, int n) {   // twice the signed area
    float a = 0.0f;
    for (int i = 0, j = n - 1; i < n; j = i++)
        a += p[j].x * p[i].y - p[i].x * p[j].y;
    return a;
}

static bool SamePt(Vector2 a, Vector2 b) {
    return fabsf(a.x - b.x) < EPS_PT && fabsf(a.y - b.y) < EPS_PT;
}

// Inclusive point-in-triangle: boundary counts as inside, because a vertex
// resting on an ear's edge still breaks clipping that ear.
static bool PointInTri(Vector2 p, Vector2 a, Vector2 b, Vector2 c) {
    float d1 = Cross3(a, b, p), d2 = Cross3(b, c, p), d3 = Cross3(c, a, p);
    bool has_neg = d1 < -EPS_AREA || d2 < -EPS_AREA || d3 < -EPS_AREA;
    bool has_pos = d1 >  EPS_AREA || d2 >  EPS_AREA || d3 >  EPS_AREA;
    return !(has_neg && has_pos);
}

// Proper segment crossing only: both endpoints strictly on opposite sides,
// both ways. Touching at an endpoint must NOT count - the bridge segment
// legitimately ends on a vertex of each loop, and the naive sign test reads
// that touch as a crossing and rejects every candidate.
static bool SegsCross(Vector2 a, Vector2 b, Vector2 c, Vector2 d) {
    float d1 = Cross3(c, d, a), d2 = Cross3(c, d, b);
    float d3 = Cross3(a, b, c), d4 = Cross3(a, b, d);
    bool s1 = (d1 >  EPS_AREA && d2 < -EPS_AREA) || (d1 < -EPS_AREA && d2 >  EPS_AREA);
    bool s2 = (d3 >  EPS_AREA && d4 < -EPS_AREA) || (d3 < -EPS_AREA && d4 >  EPS_AREA);
    return s1 && s2;
}

// Does segment (a,b) cross any edge of the point cycle p[0..n)?
static bool CrossesCycle(Vector2 a, Vector2 b, const Vector2 *p, int n) {
    for (int i = 0, j = n - 1; i < n; j = i++)
        if (SegsCross(a, b, p[j], p[i])) return true;
    return false;
}

int TriangulatePolygon(const Vector2 *verts, const int *loop_counts,
                       int loop_count, Vector2 *out, int max_tris) {
    if (loop_count < 1 || loop_counts[0] < 3) return 0;

    // ---- normalize: outer CCW, holes CW, into local loop storage ----------
    static Vector2 loops[MAX_PTS];
    int loop_off[16], total = 0, nloops = 0;
    int src = 0;
    for (int k = 0; k < loop_count && k < 16; k++) {
        int n = loop_counts[k];
        if (total + n > MAX_PTS) return 0;
        for (int i = 0; i < n; i++) loops[total + i] = verts[src + i];
        float a2 = SignedArea2(&loops[total], n);
        bool want_ccw = (k == 0);
        if ((a2 < 0.0f) == want_ccw) {                 // wrong winding: reverse
            for (int i = 0; i < n / 2; i++) {
                Vector2 t = loops[total + i];
                loops[total + i] = loops[total + n - 1 - i];
                loops[total + n - 1 - i] = t;
            }
        }
        loop_off[nloops++] = total;
        total += n;
        src += n;
    }

    // ---- merge holes into the outer boundary via bridges ------------------
    static Vector2 poly[MAX_PTS];
    int n = loop_counts[0];
    for (int i = 0; i < n; i++) poly[i] = loops[loop_off[0] + i];

    bool merged[16] = { true };                        // [0] = outer, done
    for (int pass = 1; pass < nloops; pass++) {
        for (int k = 1; k < nloops; k++) {
            if (merged[k]) continue;
            const Vector2 *hole = &loops[loop_off[k]];
            int hn = loop_counts[k];
            if (n + hn + 2 > MAX_PTS) { merged[k] = true; continue; }

            // best mutually visible (hole vertex, poly vertex) pair
            int bm = -1, bc = -1;
            float best = 1e30f;
            for (int m = 0; m < hn; m++) {
                for (int c = 0; c < n; c++) {
                    float dx = hole[m].x - poly[c].x, dy = hole[m].y - poly[c].y;
                    float d2 = dx * dx + dy * dy;
                    if (d2 >= best) continue;
                    if (CrossesCycle(hole[m], poly[c], poly, n)) continue;
                    bool blocked = false;
                    for (int j = 1; j < nloops && !blocked; j++)
                        if (!merged[j] &&
                            CrossesCycle(hole[m], poly[c], &loops[loop_off[j]], loop_counts[j]))
                            blocked = true;
                    if (blocked) continue;
                    best = d2; bm = m; bc = c;
                }
            }
            if (bm < 0) continue;    // not visible yet; later pass may free it

            // splice after poly[bc]: hole cycle from bm back to bm, then
            // a duplicate of poly[bc] to close the keyhole
            int ins = hn + 2;
            for (int i = n - 1; i > bc; i--) poly[i + ins] = poly[i];
            for (int i = 0; i <= hn; i++) poly[bc + 1 + i] = hole[(bm + i) % hn];
            poly[bc + 1 + hn + 1] = poly[bc];
            n += ins;
            merged[k] = true;
        }
    }

    // ---- ear clip ----------------------------------------------------------
    static int idx[MAX_PTS];
    for (int i = 0; i < n; i++) idx[i] = i;
    int tris = 0;

    while (n > 3 && tris < max_tris) {
        int clip = -1;
        for (int i = 0; i < n; i++) {
            Vector2 a = poly[idx[(i + n - 1) % n]];
            Vector2 b = poly[idx[i]];
            Vector2 c = poly[idx[(i + 1) % n]];
            if (Cross3(a, b, c) <= EPS_AREA) continue;          // reflex/flat
            bool blocked = false;
            for (int j = 0; j < n && !blocked; j++) {
                if (j == i || j == (i + n - 1) % n || j == (i + 1) % n) continue;
                Vector2 p = poly[idx[j]];
                if (SamePt(p, a) || SamePt(p, b) || SamePt(p, c)) continue;  // bridge dups
                if (PointInTri(p, a, b, c)) blocked = true;
            }
            if (!blocked) { clip = i; break; }
        }
        if (clip < 0) {
            // degenerate leftovers: force the most convex vertex so the loop
            // always terminates; slivers collapse to ~zero area anyway
            float bestc = -1e30f;
            for (int i = 0; i < n; i++) {
                float cr = Cross3(poly[idx[(i + n - 1) % n]], poly[idx[i]],
                                  poly[idx[(i + 1) % n]]);
                if (cr > bestc) { bestc = cr; clip = i; }
            }
            if (clip < 0) break;
        }
        out[tris * 3 + 0] = poly[idx[(clip + n - 1) % n]];
        out[tris * 3 + 1] = poly[idx[clip]];
        out[tris * 3 + 2] = poly[idx[(clip + 1) % n]];
        tris++;
        for (int i = clip; i < n - 1; i++) idx[i] = idx[i + 1];
        n--;
    }
    if (n == 3 && tris < max_tris) {
        out[tris * 3 + 0] = poly[idx[0]];
        out[tris * 3 + 1] = poly[idx[1]];
        out[tris * 3 + 2] = poly[idx[2]];
        tris++;
    }
    return tris;
}
