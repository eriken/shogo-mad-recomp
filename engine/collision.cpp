#include "engine/collision.h"

#include <algorithm>
#include <cmath>

namespace shogo {

Vec3 normalized(const Vec3 &v)
{
    float l = std::sqrt(dot(v, v));
    return l > 1e-12f ? v * (1.0f / l) : Vec3{0, 0, 0};
}
float length(const Vec3 &v) { return std::sqrt(dot(v, v)); }

namespace {
Vec3 vmin(const Vec3 &a, const Vec3 &b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
Vec3 vmax(const Vec3 &a, const Vec3 &b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
Vec3 mul(const Vec3 &a, const Vec3 &b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
Vec3 div(const Vec3 &a, const Vec3 &b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
bool overlaps(const Vec3 &amn, const Vec3 &amx, const Vec3 &bmn, const Vec3 &bmx)
{
    return amn.x <= bmx.x && amx.x >= bmn.x && amn.y <= bmx.y && amx.y >= bmn.y && amn.z <= bmx.z && amx.z >= bmn.z;
}
constexpr uint32_t kSolid = 0x1;
}  // namespace

// ---- BVH ---------------------------------------------------------------------------------------

void CollisionModel::build(const WorldBsp &bsp, size_t modelIndex)
{
    m_tris.clear();
    for (size_t pi = 0; pi < bsp.polys.size(); ++pi) {
        const WorldPoly &poly = bsp.polys[pi];
        if (poly.surface >= bsp.surfaces.size()) continue;
        const WorldSurface &s = bsp.surfaces[poly.surface];
        if (!(s.flags & kSolid) || s.plane >= bsp.planes.size()) continue;
        const WorldPlane &pl = bsp.planes[s.plane];
        size_t n = poly.verts.size();
        std::vector<Vec3> pts;
        for (size_t i = 0; i < n; ++i)
            if (poly.verts[i].point < bsp.points.size()) pts.push_back(bsp.points[poly.verts[i].point]);
        for (size_t i = 2; i < pts.size(); ++i) {
            Tri t;
            t.a = pts[0];
            t.b = pts[i - 1];
            t.c = pts[i];
            t.n = pl.normal;
            t.flags = s.flags;
            t.hpoly = polyHandle(modelIndex, pi);
            if (length(cross(t.b - t.a, t.c - t.a)) < 1e-4f) continue;  // degenerate
            m_tris.push_back(t);
        }
    }
    m_triMin.resize(m_tris.size());
    m_triMax.resize(m_tris.size());
    for (size_t i = 0; i < m_tris.size(); ++i) {
        m_triMin[i] = vmin(m_tris[i].a, vmin(m_tris[i].b, m_tris[i].c));
        m_triMax[i] = vmax(m_tris[i].a, vmax(m_tris[i].b, m_tris[i].c));
    }
    m_order.resize(m_tris.size());
    for (size_t i = 0; i < m_order.size(); ++i) m_order[i] = int(i);
    m_nodes.clear();
    if (!m_tris.empty()) buildNode(0, int(m_tris.size()), 0);
}

int CollisionModel::buildNode(int first, int count, int depth)
{
    Node node;
    node.mn = {1e30f, 1e30f, 1e30f};
    node.mx = {-1e30f, -1e30f, -1e30f};
    for (int i = first; i < first + count; ++i) {
        node.mn = vmin(node.mn, m_triMin[m_order[i]]);
        node.mx = vmax(node.mx, m_triMax[m_order[i]]);
    }
    int index = int(m_nodes.size());
    m_nodes.push_back(node);
    if (count <= 8 || depth > 40) {
        m_nodes[index].first = first;
        m_nodes[index].count = count;
        return index;
    }
    // Split at the median of the longest axis.
    Vec3 ext = node.mx - node.mn;
    int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    auto centre = [&](int t) {
        const Vec3 &a = m_triMin[t], &b = m_triMax[t];
        return axis == 0 ? a.x + b.x : axis == 1 ? a.y + b.y : a.z + b.z;
    };
    int mid = first + count / 2;
    std::nth_element(m_order.begin() + first, m_order.begin() + mid, m_order.begin() + first + count,
                     [&](int a, int b) { return centre(a) < centre(b); });
    int l = buildNode(first, mid - first, depth + 1);
    int r = buildNode(mid, first + count - mid, depth + 1);
    m_nodes[index].left = l;
    m_nodes[index].right = r;
    return index;
}

void CollisionModel::query(const Vec3 &mn, const Vec3 &mx, std::vector<int> &out) const
{
    if (m_nodes.empty()) return;
    int stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node &n = m_nodes[stack[--sp]];
        if (!overlaps(mn, mx, n.mn, n.mx)) continue;
        if (n.left < 0) {
            for (int i = n.first; i < n.first + n.count; ++i)
                if (overlaps(mn, mx, m_triMin[m_order[i]], m_triMax[m_order[i]])) out.push_back(m_order[i]);
        } else if (sp + 2 <= 128) {
            stack[sp++] = n.left;
            stack[sp++] = n.right;
        }
    }
}

// ---- swept unit sphere vs. triangle (ellipsoid space) -----------------------------------------

namespace {

struct ESpaceHit {
    bool found = false;
    float t = 0;       // fraction of the velocity
    Vec3 point;        // eSpace contact point
    int index = -1;
};

bool lowestRoot(float a, float b, float c, float maxR, float *root)
{
    float det = b * b - 4 * a * c;
    if (det < 0 || std::fabs(a) < 1e-12f) return false;
    float s = std::sqrt(det);
    float r1 = (-b - s) / (2 * a), r2 = (-b + s) / (2 * a);
    if (r1 > r2) std::swap(r1, r2);
    if (r1 > 0 && r1 < maxR) {
        *root = r1;
        return true;
    }
    if (r2 > 0 && r2 < maxR) {
        *root = r2;
        return true;
    }
    return false;
}

bool pointInTriangle(const Vec3 &p, const Vec3 &a, const Vec3 &b, const Vec3 &c)
{
    Vec3 v0 = c - a, v1 = b - a, v2 = p - a;
    float d00 = dot(v0, v0), d01 = dot(v0, v1), d02 = dot(v0, v2), d11 = dot(v1, v1), d12 = dot(v1, v2);
    float den = d00 * d11 - d01 * d01;
    if (std::fabs(den) < 1e-12f) return false;
    float inv = 1.0f / den;
    float u = (d11 * d02 - d01 * d12) * inv, v = (d00 * d12 - d01 * d02) * inv;
    return u >= -1e-5f && v >= -1e-5f && u + v <= 1.0f + 1e-5f;
}

void sweepTriangle(const Vec3 &p1, const Vec3 &p2, const Vec3 &p3, const Vec3 &n, const Vec3 &base, const Vec3 &vel,
                   int index, ESpaceHit &best)
{
    // One-sided: only triangles facing against the motion.
    float nDotVel = dot(n, vel);
    if (nDotVel >= 0) return;
    float planeD = -dot(n, p1);
    float dist = dot(n, base) + planeD;
    float t0, t1;
    bool embedded = false;
    if (std::fabs(nDotVel) < 1e-9f) {
        if (std::fabs(dist) >= 1.0f) return;
        embedded = true;
        t0 = 0;
        t1 = 1;
    } else {
        t0 = (-1.0f - dist) / nDotVel;
        t1 = (1.0f - dist) / nDotVel;
        if (t0 > t1) std::swap(t0, t1);
        if (t0 > 1.0f || t1 < 0.0f) return;
        t0 = std::clamp(t0, 0.0f, 1.0f);
        t1 = std::clamp(t1, 0.0f, 1.0f);
    }
    // Behind the plane by more than the radius: the sphere started on the far side.
    if (dist < -1.0f) return;

    bool found = false;
    float t = 1.0f;
    Vec3 point;
    if (!embedded) {
        Vec3 planeHit = base - n + vel * t0;
        if (pointInTriangle(planeHit, p1, p2, p3)) {
            found = true;
            t = t0;
            point = planeHit;
        }
    }
    if (!found) {
        float velSq = dot(vel, vel);
        float r;
        for (const Vec3 *p : {&p1, &p2, &p3}) {
            float b = 2.0f * dot(vel, base - *p);
            float c = dot(*p - base, *p - base) - 1.0f;
            // Already over the corner and leaving it: the root ahead is where
            // we come off it, not a hit (an object pushed into a lever's edge
            // could never walk away).
            if (c < 0 && b >= 0) continue;
            if (lowestRoot(velSq, b, c, t, &r)) {
                t = r;
                found = true;
                point = *p;
            }
        }
        const Vec3 *edges[3][2] = {{&p1, &p2}, {&p2, &p3}, {&p3, &p1}};
        for (auto &e : edges) {
            Vec3 edge = *e[1] - *e[0], btv = *e[0] - base;
            float edgeSq = dot(edge, edge), edgeDotVel = dot(edge, vel), edgeDotBtv = dot(edge, btv);
            float a = edgeSq * -velSq + edgeDotVel * edgeDotVel;
            float b = edgeSq * (2 * dot(vel, btv)) - 2 * edgeDotVel * edgeDotBtv;
            float c = edgeSq * (1 - dot(btv, btv)) + edgeDotBtv * edgeDotBtv;
            // Already over the edge's line (c > 0 here) and leaving it: as above.
            if (c > 0 && b <= 0) continue;
            if (lowestRoot(a, b, c, t, &r)) {
                float f = (edgeDotVel * r - edgeDotBtv) / edgeSq;
                if (f >= 0 && f <= 1) {
                    t = r;
                    found = true;
                    point = *e[0] + edge * f;
                }
            }
        }
    }
    if (found && (!best.found || t < best.t)) {
        best.found = true;
        best.t = t;
        best.point = point;
        best.index = index;
    }
}

struct ETri {
    Vec3 a, b, c, n;
};

ESpaceHit sweepAll(const std::vector<ETri> &etris, const Vec3 &base, const Vec3 &vel)
{
    ESpaceHit best;
    for (size_t i = 0; i < etris.size(); ++i)
        sweepTriangle(etris[i].a, etris[i].b, etris[i].c, etris[i].n, base, vel, int(i), best);
    return best;
}

std::vector<ETri> toESpace(const std::vector<SweepTri> &tris, const Vec3 &radii)
{
    std::vector<ETri> out;
    out.reserve(tris.size());
    for (const SweepTri &s : tris) {
        ETri e;
        e.a = div(s.tri.a, radii);
        e.b = div(s.tri.b, radii);
        e.c = div(s.tri.c, radii);
        // Normals scale by the inverse transpose of the space change: multiply by the radii.
        e.n = normalized(mul(s.tri.n, radii));
        out.push_back(e);
    }
    return out;
}
}  // namespace

SweepHit sweepEllipsoid(const std::vector<SweepTri> &tris, const Vec3 &radii, const Vec3 &from, const Vec3 &move)
{
    SweepHit hit;
    std::vector<ETri> etris = toESpace(tris, radii);
    Vec3 base = div(from, radii), vel = div(move, radii);
    ESpaceHit h = sweepAll(etris, base, vel);
    if (!h.found) return hit;
    hit.hit = true;
    hit.distance = h.t * length(move);
    hit.point = mul(h.point, radii);
    Vec3 centre = base + vel * h.t;
    hit.normal = normalized(div(centre - h.point, radii));
    hit.owner = tris[h.index].owner;
    hit.flags = tris[h.index].tri.flags;
    hit.hpoly = tris[h.index].tri.hpoly;
    return hit;
}

Vec3 collideAndSlide(const std::vector<SweepTri> &tris, const Vec3 &radii, const Vec3 &from, const Vec3 &move,
                     std::vector<SweepHit> *hits, float wallNormalY)
{
    std::vector<ETri> etris = toESpace(tris, radii);
    Vec3 pos = div(from, radii), vel = div(move, radii);
    const float veryClose = 0.005f;
    Vec3 firstPlaneNormal{0, 0, 0};
    for (int depth = 0; depth < 5; ++depth) {
        if (length(vel) < veryClose * 0.1f) break;
        ESpaceHit h = sweepAll(etris, pos, vel);
        if (!h.found) {
            pos = pos + vel;
            break;
        }
        Vec3 dest = pos + vel;
        float velLen = length(vel);
        float dist = h.t * velLen;
        Vec3 newBase = pos;
        Vec3 dir = vel * (1.0f / velLen);
        Vec3 contact = h.point;
        if (dist >= veryClose) {
            newBase = pos + dir * (dist - veryClose);
            contact = contact - dir * veryClose;
        }
        Vec3 slideNormal = normalized(newBase - contact);
        // Walkers: a steep upward face is a wall, slid along horizontally
        // (otherwise sliding creeps up it and gravity pulls back down).
        if (wallNormalY > 0) {
            Vec3 world = normalized(div(slideNormal, radii));
            if (world.y > 0 && world.y < wallNormalY) {
                Vec3 flat = normalized(Vec3{world.x, 0, world.z});
                if (length(flat) > 0.5f) slideNormal = normalized(mul(flat, radii));
            }
        }
        if (hits) {
            SweepHit sh;
            sh.hit = true;
            sh.distance = dist;
            sh.point = mul(h.point, radii);
            sh.normal = normalized(div(slideNormal, radii));
            sh.face = tris[h.index].tri.n;
            sh.owner = tris[h.index].owner;
            sh.flags = tris[h.index].tri.flags;
            sh.hpoly = tris[h.index].tri.hpoly;
            hits->push_back(sh);
        }
        // Project the rest of the move onto the sliding plane.
        // (The rest of the move, relative to where we stopped, minus its part
        // along the normal. Same as Fauerby's newDest - contact, but also right
        // when the normal was flattened and isn't along newBase - contact.)
        Vec3 rest = dest - newBase;
        Vec3 newVel = rest - slideNormal * dot(rest, slideNormal);
        // Two planes: slide along their crease instead of bouncing between them.
        if (depth == 0) {
            firstPlaneNormal = slideNormal;
        } else if (depth == 1) {
            Vec3 crease = normalized(cross(firstPlaneNormal, slideNormal));
            if (length(crease) > 0.5f) newVel = crease * dot(newVel, crease);
        }
        pos = newBase;
        vel = newVel;
    }
    return mul(pos, radii);
}

bool segmentTriangle(const Tri &t, const Vec3 &from, const Vec3 &to, bool twoSided, float *frac)
{
    Vec3 dir = to - from;
    float denom = dot(t.n, dir);
    if (!twoSided && denom >= 0) return false;
    if (std::fabs(denom) < 1e-9f) return false;
    float d = dot(t.n, t.a);
    float f = (d - dot(t.n, from)) / denom;
    if (f < 0 || f > 1) return false;
    Vec3 p = from + dir * f;
    if (!pointInTriangle(p, t.a, t.b, t.c)) return false;
    *frac = f;
    return true;
}

namespace {

// The walkable floor (normal y over 0.7) just past a contact on an edge, in
// direction `dir`: the edge is shared with the riser, so the hit's own triangle
// may be either. Returns its normal, or y = 0 when there is none.
Vec3 floorPast(const std::vector<SweepTri> &tris, const Vec3 &point, const Vec3 &dir)
{
    Vec3 p = point + dir * 0.5f;
    for (const SweepTri &t : tris) {
        float f;
        if (t.tri.n.y > 0.7f && segmentTriangle(t.tri, p + Vec3{0, 1, 0}, p - Vec3{0, 1, 0}, false, &f)) return t.tri.n;
    }
    return {0, 0, 0};
}

}  // namespace

Vec3 walkMove(const std::vector<SweepTri> &tris, const Vec3 &radii, const Vec3 &from, const Vec3 &move,
              float stepHeight, std::vector<SweepHit> &hits)
{
    Vec3 end = collideAndSlide(tris, radii, from, move, &hits, 0.5f);
    Vec3 horiz{move.x, 0, move.z};
    const float wanted = length(horiz);
    const Vec3 dir = wanted > 0.01f ? horiz * (1.0f / wanted) : Vec3{0, 0, 0};

    // Stairs: if a wall stopped us, try stepping up, across and back down.
    bool wall = false;
    for (const SweepHit &h : hits)
        if (std::fabs(h.normal.y) < 0.7f) wall = true;
    if (wall && wanted > 0.01f) {
        std::vector<SweepHit> stepHits;
        Vec3 up = collideAndSlide(tris, radii, from, {0, stepHeight, 0}, nullptr);
        // Level across: riding up the curve of a lip would climb past the
        // step height.
        Vec3 across = collideAndSlide(tris, radii, up, horiz, nullptr, 1.01f);
        float drop = (up.y - from.y) + std::max(0.0f, -move.y) + 0.5f;
        // Straight down to the first contact (sliding would roll us back off
        // a lip).
        SweepHit landing = sweepEllipsoid(tris, radii, across, {0, -drop, 0});
        Vec3 down = across - Vec3{0, landing.hit ? std::max(0.0f, landing.distance - 0.05f) : drop, 0};
        if (landing.hit) stepHits.push_back(landing);
        // Landed on something flat, or on the lip of a floor above our feet:
        // the rounded bottom meets a step's edge before its top, and once the
        // riser has stopped us a frame's move (a couple of units) never
        // carries the middle over it. Either way the floor's plane is what we
        // stand on: the curve of the lip would turn the speed we had into a
        // hop up off the step.
        // (A lip is touched with the bottom raised over it, so: its height
        // too must be within a step of our feet.)
        bool landed = false, onStep = false;
        const float feet = from.y - radii.y;
        for (SweepHit &h : stepHits) {
            if (h.point.y > feet + stepHeight + 0.5f) continue;
            Vec3 floor = h.face.y > 0.7f ? h.face : floorPast(tris, h.point, dir);
            bool higher = h.point.y > feet + 1.0f;
            if (h.normal.y > 0.7f || (floor.y > 0.7f && higher)) landed = true;
            if (floor.y > 0.7f && higher) onStep = true;
            if (floor.y > 0.7f) h.normal = h.face = floor;
        }
        Vec3 gotStep = down - from, gotSlide = end - from;
        float stepDist = std::sqrt(gotStep.x * gotStep.x + gotStep.z * gotStep.z);
        float slideDist = std::sqrt(gotSlide.x * gotSlide.x + gotSlide.z * gotSlide.z);
        // Up a step: when it gets further than the slide, or as far but onto
        // the step (the slide would stop at its riser).
        if (landed && (stepDist > slideDist + 0.5f || (onStep && stepDist + 0.01f >= slideDist))) {
            end = down;
            hits = std::move(stepHits);
        }
    }

    // Walking over a floor's lip: the rounded bottom rides up the curve of the
    // edge, and bouncing off that slanted contact turns the speed into a hop.
    // Walkers stand on the floor's own plane, so: stop only what goes into the
    // floor, and settle back down onto it.
    bool lip = false;
    for (SweepHit &h : hits) {
        if (h.normal.y <= 0.0f || dot(h.normal, h.face) > 0.999f) continue;  // flat on a face
        Vec3 floor = h.face.y > 0.7f || wanted <= 0.01f ? h.face : floorPast(tris, h.point, dir);
        if (floor.y > 0.7f) {
            h.normal = floor;
            lip = true;
        }
    }
    if (lip && move.y <= 0.0f && end.y > from.y + 0.5f) {
        std::vector<SweepHit> settleHits;
        Vec3 settled = collideAndSlide(tris, radii, end, {0, from.y - end.y, 0}, &settleHits);
        for (const SweepHit &h : settleHits)
            if (h.face.y > 0.7f) {
                end = settled;
                break;
            }
    }
    return end;
}

}  // namespace shogo
