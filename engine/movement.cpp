#include "engine/movement.h"

#include "engine/math3d.h"
#include "engine/server.h"

#include <algorithm>
#include <strings.h>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace shogo {

namespace {

Vec3 v3(const DVector &v) { return {v.x, v.y, v.z}; }
DVector dv(const Vec3 &v) { return DVector(v.x, v.y, v.z); }
DRotation conj(const DRotation &q)
{
    DRotation r = q;
    r.m_Vec.x = -r.m_Vec.x;
    r.m_Vec.y = -r.m_Vec.y;
    r.m_Vec.z = -r.m_Vec.z;
    return r;
}

// How a world model's geometry (stored where it was built) is placed now:
// world = R * (built - basePos) + pos.
struct Placement {
    DRotation r, rInv;
    Vec3 basePos, pos;
    bool identity = true;
    Vec3 toWorld(const Vec3 &p) const
    {
        if (identity) return p - basePos + pos;
        return v3(quatRotate(r, dv(p - basePos))) + pos;
    }
    Vec3 toBuilt(const Vec3 &p) const
    {
        if (identity) return p - pos + basePos;
        return v3(quatRotate(rInv, dv(p - pos))) + basePos;
    }
    Vec3 normalToWorld(const Vec3 &n) const { return identity ? n : v3(quatRotate(r, dv(n))); }
};

Placement placementOf(const EngineObject *o)
{
    Placement p;
    p.basePos = v3(o->basePos);
    p.pos = v3(o->pos);
    p.r = quatNormalize(quatMul(o->rot, conj(o->baseRot)));
    p.rInv = conj(p.r);
    p.identity = std::fabs(p.r.m_Spin) > 0.999999f;
    return p;
}

Vec3 radiiOf(const EngineObject *o)
{
    if (o->flags & FLAG_POINTCOLLIDE) return {0.5f, 0.5f, 0.5f};
    return {std::max(o->dims.x, 0.5f), std::max(o->dims.y, 0.5f), std::max(o->dims.z, 0.5f)};
}

bool isWorldModel(const EngineObject *o) { return (o->type == OT_WORLDMODEL || o->type == OT_CONTAINER) && o->bsp >= 0; }

// Whether `other` blocks `mover`.
bool blocks(const EngineObject *mover, const EngineObject *other)
{
    if (other == mover || other->removed || !(other->flags & FLAG_SOLID)) return false;
    if (mover->clientOnly) {
        if (other->flags & FLAG_CLIENTNONSOLID) return false;
    } else if (other->clientOnly) {
        return false;  // the server never collides with the client's own objects
    }
    if (other->type == OT_CONTAINER) return false;
    return true;
}

void addBox(std::vector<SweepTri> &out, EngineObject *o)
{
    Vec3 c = v3(o->pos), d = v3(o->dims);
    Vec3 mn = c - d, mx = c + d;
    Vec3 p[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                 {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    struct Face {
        int a, b, c, d;
        Vec3 n;
    } faces[6] = {{0, 1, 2, 3, {0, 0, -1}}, {5, 4, 7, 6, {0, 0, 1}}, {4, 0, 3, 7, {-1, 0, 0}},
                  {1, 5, 6, 2, {1, 0, 0}},  {3, 2, 6, 7, {0, 1, 0}}, {4, 5, 1, 0, {0, -1, 0}}};
    for (const Face &f : faces) {
        SweepTri t1, t2;
        t1.tri = {p[f.a], p[f.b], p[f.c], f.n, 0x1};
        t2.tri = {p[f.a], p[f.c], p[f.d], f.n, 0x1};
        t1.owner = t2.owner = o;
        out.push_back(t1);
        out.push_back(t2);
    }
}

// Everything `mover` could hit within [mn, mx].
void gather(Server &s, EngineObject *mover, const Vec3 &mn, const Vec3 &mx, std::vector<SweepTri> &out)
{
    std::vector<int> idx;
    bool world = !(mover->flags & FLAG_GOTHRUWORLD);
    if (world && !s.collisionModels.empty()) {
        const CollisionModel &m = s.collisionModels[0];
        m.query(mn, mx, idx);
        for (int i : idx) out.push_back({m.tris()[i], s.worldObject});
    }
    for (EngineObject *o : objects().all()) {
        if (o == mover || o->removed) continue;
        if (isWorldModel(o)) {
            if (!world || o->bsp <= 0 || size_t(o->bsp) >= s.collisionModels.size()) continue;
            if (!(o->flags & FLAG_SOLID) || (o->flags & FLAG_SKYOBJECT) || o->type == OT_CONTAINER) continue;
            if (mover->clientOnly && (o->flags & FLAG_CLIENTNONSOLID)) continue;
            Placement p = placementOf(o);
            // Bounds of the query box in the model's build space (a sphere around it, when rotated).
            Vec3 c = (mn + mx) * 0.5f, e = (mx - mn) * 0.5f;
            Vec3 bc = p.toBuilt(c);
            Vec3 be = p.identity ? e : Vec3{length(e), length(e), length(e)};
            idx.clear();
            const CollisionModel &m = s.collisionModels[size_t(o->bsp)];
            m.query(bc - be, bc + be, idx);
            for (int i : idx) {
                const Tri &t = m.tris()[i];
                SweepTri st;
                st.tri = {p.toWorld(t.a), p.toWorld(t.b), p.toWorld(t.c), p.normalToWorld(t.n), t.flags, t.hpoly};
                st.owner = o;
                out.push_back(st);
            }
            continue;
        }
        if (!(mover->flags & FLAG_SOLID) || !blocks(mover, o)) continue;
        if (o->dims.x <= 0 && o->dims.y <= 0 && o->dims.z <= 0) continue;
        Vec3 omn = v3(o->pos) - v3(o->dims), omx = v3(o->pos) + v3(o->dims);
        if (omn.x > mx.x || omx.x < mn.x || omn.y > mx.y || omx.y < mn.y || omn.z > mx.z || omx.z < mn.z) continue;
        addBox(out, o);
    }
}

void queryBox(const Vec3 &from, const Vec3 &to, const Vec3 &radii, Vec3 *mn, Vec3 *mx)
{
    float slack = length(to - from) + 1.0f;
    Vec3 pad = radii + Vec3{slack, slack, slack};
    *mn = Vec3{std::min(from.x, to.x), std::min(from.y, to.y), std::min(from.z, to.z)} - pad;
    *mx = Vec3{std::max(from.x, to.x), std::max(from.y, to.y), std::max(from.z, to.z)} + pad;
}

void touch(Server &s, EngineObject *a, EngineObject *b)
{
    // a gets told it touched b.
    if (!a || a->clientOnly || a->removed || !(a->flags & FLAG_TOUCH_NOTIFY)) return;
    s.engineMessage(a, MID_TOUCHNOTIFY, b ? toHObject(b) : nullptr, 0.0f);
}

// Boxes this close count as touching. Collision stops a mover a hair short of
// the world, so an object whose box is flush with a wall or a piece of
// furniture (the ID card on the Quarters nightstand, its front at the
// nightstand's face) could never be touched walking straight at it.
constexpr float kTouchContact = 1.0f;

bool boxesOverlap(const EngineObject *a, const EngineObject *b)
{
    return std::fabs(a->pos.x - b->pos.x) <= a->dims.x + b->dims.x + kTouchContact &&
           std::fabs(a->pos.y - b->pos.y) <= a->dims.y + b->dims.y + kTouchContact &&
           std::fabs(a->pos.z - b->pos.z) <= a->dims.z + b->dims.z + kTouchContact;
}

// Touches from overlapping (triggers, pickups, projectiles passing through characters).
void overlapTouches(Server &s, EngineObject *mover)
{
    if (mover->clientOnly || mover->removed) return;
    std::vector<EngineObject *> touched;
    for (EngineObject *o : objects().all()) {
        if (o == mover || o->clientOnly || o->removed || isWorldModel(o)) continue;
        if (!(o->flags & (FLAG_TOUCH_NOTIFY | FLAG_SOLID))) continue;
        if (!boxesOverlap(mover, o)) continue;
        touched.push_back(o);
    }
    for (EngineObject *o : touched) {
        if (mover->removed) break;
        if ((o->flags & FLAG_TOUCH_NOTIFY) && (mover->flags & (FLAG_SOLID | FLAG_TOUCH_NOTIFY))) touch(s, o, mover);
        if ((mover->flags & FLAG_TOUCH_NOTIFY) && (o->flags & (FLAG_SOLID | FLAG_TOUCH_NOTIFY))) touch(s, mover, o);
    }
}

Vec3 slide(Server &s, EngineObject *o, const std::vector<SweepTri> &tris, const Vec3 &radii, const Vec3 &from,
           const Vec3 &move, std::vector<SweepHit> &hits)
{
    if (o->flags & FLAG_NOSLIDING) {
        SweepHit h = sweepEllipsoid(tris, radii, from, move);
        if (!h.hit) return from + move;
        hits.push_back(h);
        float len = length(move);
        float d = std::max(0.0f, h.distance - 0.05f);
        return from + move * (len > 0 ? d / len : 0);
    }
    (void)s;
    return collideAndSlide(tris, radii, from, move, &hits);
}

}  // namespace

void moveObject(Server &s, EngineObject *o, const DVector &target, bool teleport)
{
    if (!o || o->removed) return;
    Vec3 from = v3(o->pos), to = v3(target);
    bool collide = !teleport && (s.world != nullptr) && !isWorldModel(o);
    if (collide && (o->flags & FLAG_GOTHRUWORLD) && !(o->flags & FLAG_SOLID)) collide = false;
    if (!collide) {
        o->pos = target;
        if (!isWorldModel(o)) overlapTouches(s, o);
        return;
    }
    Vec3 move = to - from;
    if (dot(move, move) < 1e-8f) {
        overlapTouches(s, o);
        return;
    }
    Vec3 radii = radiiOf(o);

    // Leave room for a stair step in the gathered geometry.
    const float stepHeight = std::min(radii.y, 24.0f);
    Vec3 mn, mx;
    queryBox(from, to, radii, &mn, &mx);
    mx.y += stepHeight;
    std::vector<SweepTri> tris;
    gather(s, o, mn, mx, tris);

    std::vector<SweepHit> hits;
    Vec3 end = (o->flags & FLAG_STAIRSTEP) && !(o->flags & FLAG_NOSLIDING)
                   ? walkMove(tris, radii, from, move, stepHeight, hits)
                   : slide(s, o, tris, radii, from, move, hits);
    o->pos = dv(end);
    static bool trace = getenv("SHOGO_TRACE_MOVES") != nullptr;
    if (trace && o->clientOnly) {
        printf("[move] from %.2f %.2f %.2f move %.2f %.2f %.2f -> %.2f %.2f %.2f, %zu tris, hits:", from.x, from.y, from.z,
               move.x, move.y, move.z, end.x, end.y, end.z, tris.size());
        for (const SweepHit &h : hits) printf(" [d %.2f n %.2f %.2f %.2f]", h.distance, h.normal.x, h.normal.y, h.normal.z);
        printf("\n");
    }

    // Stop the velocity going into what we hit.
    for (const SweepHit &h : hits) {
        Vec3 v = v3(o->velocity);
        float into = dot(v, h.normal);
        if (into < 0) o->velocity = dv(v - h.normal * into);
    }
    if (!o->clientOnly) {
        std::vector<const SweepHit *> others;  // first hit per object
        for (const SweepHit &h : hits) {
            bool seen = false;
            for (const SweepHit *x : others) seen = seen || x->owner == h.owner;
            if (!seen) others.push_back(&h);
        }
        for (const SweepHit *h : others) {
            if (o->removed) break;
            auto *other = static_cast<EngineObject *>(h->owner);
            // GetLastCollision during MID_TOUCHNOTIFY: what was hit and its plane.
            CollisionInfo &c = s.lastCollision;
            c = CollisionInfo{};
            c.m_Plane.m_Normal = dv(h->normal);
            c.m_Plane.m_Dist = dot(h->normal, h->point);
            c.m_hObject = other ? toHObject(other) : nullptr;
            c.m_hPoly = h->hpoly;
            touch(s, o, other);
            if (other && other != s.worldObject) {
                c.m_Plane.m_Normal = dv(h->normal * -1.0f);
                c.m_Plane.m_Dist = -c.m_Plane.m_Dist;
                c.m_hObject = toHObject(o);
                c.m_hPoly = INVALID_HPOLY;
                touch(s, other, o);
            }
        }
        s.lastCollision = CollisionInfo{};  // overlaps have no plane
        s.lastCollision.m_hPoly = INVALID_HPOLY;
        if (!o->removed) overlapTouches(s, o);
    }
}

DVector integrateMotion(Server &s, EngineObject *o, float dt, float groundFriction)
{
    DVector a = o->accel;
    if (o->flags & FLAG_GRAVITY) {
        a.x += s.globalForce.x;
        a.y += s.globalForce.y;
        a.z += s.globalForce.z;
    }
    DVector v = o->velocity;
    DVector offset(v.x * dt + 0.5f * a.x * dt * dt, v.y * dt + 0.5f * a.y * dt * dt, v.z * dt + 0.5f * a.z * dt * dt);
    DVector nv(v.x + a.x * dt, v.y + a.y * dt, v.z + a.z * dt);
    if (groundFriction > 0.0f) {
        // Sideways: speed up, then lose groundFriction * dt of speed (stopping at
        // zero, so a push weaker than the friction doesn't move it at all), and
        // move by the average of the old and new velocity.
        float speed = std::sqrt(nv.x * nv.x + nv.z * nv.z), drop = groundFriction * dt;
        float keep = speed > drop ? 1.0f - drop / speed : 0.0f;
        nv.x *= keep;
        nv.z *= keep;
        offset.x = 0.5f * (v.x + nv.x) * dt;
        offset.z = 0.5f * (v.z + nv.z) * dt;
    }
    o->velocity = nv;
    return offset;
}

bool standingOn(Server &s, EngineObject *o, CollisionInfo *info)
{
    if (info) memset(info, 0, sizeof(*info));
    if (!o || !s.world || (o->flags & FLAG_GOTHRUWORLD)) return false;
    Vec3 radii = radiiOf(o), from = v3(o->pos), move{0, -2.0f, 0};
    // Look a little further down for the polygon under the middle.
    const float below = radii.y + 2.0f + 16.0f;
    Vec3 mn, mx;
    queryBox(from, from + Vec3{0, -18.0f, 0}, radii, &mn, &mx);
    std::vector<SweepTri> tris;
    gather(s, o, mn, mx, tris);
    SweepHit h = sweepEllipsoid(tris, radii, from, move);
    if (!h.hit || h.normal.y <= 0.0f) return false;
    // The plane stood on is the polygon's, not the curve of our ellipsoid
    // where it touches: on an edge (a slab's lip, the foot of a ramp) that
    // leans over to 45 degrees, which the game takes for a slope too steep
    // to stand on, and stops the player. So: the upward polygon straight
    // below the middle, else the contact.
    Vec3 normal = h.normal, point = h.point;
    void *owner = h.owner;
    uint32_t hpoly = INVALID_HPOLY;
    float nearest = 2.0f;
    for (const SweepTri &t : tris) {
        float f;
        if (t.tri.n.y > 0.0f && segmentTriangle(t.tri, from, from + Vec3{0, -below, 0}, false, &f) && f < nearest) {
            nearest = f;
            normal = t.tri.n;
            point = t.tri.a;
            owner = t.owner;
            hpoly = t.tri.hpoly;
        }
    }
    if (info) {
        info->m_Plane.m_Normal = dv(normal);
        info->m_Plane.m_Dist = dot(normal, point);
        auto *ob = static_cast<EngineObject *>(owner);
        info->m_hObject = ob ? toHObject(ob) : nullptr;
        info->m_hPoly = hpoly;
    }
    return true;
}

bool intersectSegment(Server &s, IntersectQuery *q, IntersectInfo *info, bool clientSide)
{
    if (!q || !info || !s.world) return false;
    Vec3 from = v3(q->m_From), to = v3(q->m_To);
    float best = 2.0f;
    Vec3 bestN;
    uint32_t bestFlags = 0, bestPoly = INVALID_HPOLY;
    EngineObject *bestObj = nullptr;
    bool ignoreNonsolid = (q->m_Flags & IGNORE_NONSOLID) != 0;

    auto consider = [&](const Tri &t, EngineObject *owner) {
        float f;
        if (ignoreNonsolid && !(t.flags & 0x1)) return;
        if (segmentTriangle(t, from, to, false, &f) && f < best) {
            best = f;
            bestN = t.n;
            bestFlags = t.flags;
            bestPoly = t.hpoly;
            bestObj = owner;
        }
    };
    Vec3 mn{std::min(from.x, to.x), std::min(from.y, to.y), std::min(from.z, to.z)};
    Vec3 mx{std::max(from.x, to.x), std::max(from.y, to.y), std::max(from.z, to.z)};
    std::vector<int> idx;
    if (!s.collisionModels.empty()) {
        s.collisionModels[0].query(mn, mx, idx);
        for (int i : idx) consider(s.collisionModels[0].tris()[i], s.worldObject);
    }
    for (EngineObject *o : objects().all()) {
        if (o->removed) continue;
        if (q->m_FilterFn && (isWorldModel(o) || (q->m_Flags & INTERSECT_OBJECTS)) &&
            !q->m_FilterFn(toHObject(o), q->m_pUserData))
            continue;
        if (isWorldModel(o)) {
            if (o->bsp <= 0 || size_t(o->bsp) >= s.collisionModels.size() || (o->flags & FLAG_SKYOBJECT) ||
                o->type == OT_CONTAINER)
                continue;
            if (!(o->flags & FLAG_SOLID) && ignoreNonsolid) continue;
            if (clientSide && (o->flags & FLAG_CLIENTNONSOLID)) continue;
            Placement p = placementOf(o);
            Vec3 bf = p.toBuilt(from), bt = p.toBuilt(to);
            idx.clear();
            s.collisionModels[size_t(o->bsp)].query(
                {std::min(bf.x, bt.x), std::min(bf.y, bt.y), std::min(bf.z, bt.z)},
                {std::max(bf.x, bt.x), std::max(bf.y, bt.y), std::max(bf.z, bt.z)}, idx);
            for (int i : idx) {
                const Tri &t = s.collisionModels[size_t(o->bsp)].tris()[i];
                float f;
                if (ignoreNonsolid && !(t.flags & 0x1)) continue;
                if (segmentTriangle(t, bf, bt, false, &f) && f < best) {
                    best = f;
                    bestN = p.normalToWorld(t.n);
                    bestFlags = t.flags;
                    bestPoly = t.hpoly;
                    bestObj = o;
                }
            }
            continue;
        }
        if (!(q->m_Flags & INTERSECT_OBJECTS)) continue;
        if (!(o->flags & FLAG_RAYHIT) && !(o->flags & FLAG_SOLID)) continue;
        if (ignoreNonsolid && !(o->flags & FLAG_SOLID)) continue;
        if (clientSide ? (o->flags & FLAG_CLIENTNONSOLID) != 0 : o->clientOnly) continue;
        if (o->dims.x <= 0 && o->dims.y <= 0 && o->dims.z <= 0) continue;
        Vec3 d = to - from, c = v3(o->pos), e = v3(o->dims);
        // A segment that starts inside a box doesn't hit it: the AI looks from
        // its eye level, inside its own box, and doesn't filter itself out, so
        // otherwise every sight check would see only the looker.
        if (std::fabs(from.x - c.x) < e.x && std::fabs(from.y - c.y) < e.y && std::fabs(from.z - c.z) < e.z)
            continue;
        // Slab test against the object's box.
        float t0 = 0, t1 = 1;
        Vec3 n{0, 0, 0};
        bool miss = false;
        for (int ax = 0; ax < 3 && !miss; ++ax) {
            float s0 = ax == 0 ? from.x : ax == 1 ? from.y : from.z;
            float dd = ax == 0 ? d.x : ax == 1 ? d.y : d.z;
            float lo = (ax == 0 ? c.x - e.x : ax == 1 ? c.y - e.y : c.z - e.z);
            float hi = (ax == 0 ? c.x + e.x : ax == 1 ? c.y + e.y : c.z + e.z);
            if (std::fabs(dd) < 1e-9f) {
                if (s0 < lo || s0 > hi) miss = true;
                continue;
            }
            float a = (lo - s0) / dd, b = (hi - s0) / dd;
            float sign = -1;
            if (a > b) {
                std::swap(a, b);
                sign = 1;
            }
            if (a > t0) {
                t0 = a;
                n = {0, 0, 0};
                (ax == 0 ? n.x : ax == 1 ? n.y : n.z) = sign;
            }
            t1 = std::min(t1, b);
            if (t0 > t1) miss = true;
        }
        if (!miss && t0 < best) {
            best = t0;
            bestN = n;
            bestFlags = 0;
            bestPoly = INVALID_HPOLY;
            bestObj = o;
        }
    }
    if (best > 1.0f) return false;
    Vec3 p = from + (to - from) * best;
    info->m_Point = dv(p);
    info->m_Plane.m_Normal = dv(bestN);
    info->m_Plane.m_Dist = dot(bestN, p);
    info->m_hObject = bestObj ? toHObject(bestObj) : nullptr;
    info->m_hPoly = bestPoly;
    info->m_SurfaceFlags = bestFlags;
    return true;
}

DDWORD polyTextureFlags(Server &s, HPOLY poly)
{
    if (!s.world || !s.fs || poly == INVALID_HPOLY) return 0;
    size_t model = poly >> 24, index = poly & 0xFFFFFF;
    if (model >= s.world->models.size()) return 0;
    const WorldBsp &b = s.world->models[model];
    if (index >= b.polys.size() || b.polys[index].surface >= b.surfaces.size()) return 0;
    const WorldSurface &surf = b.surfaces[b.polys[index].surface];
    if (surf.texture >= b.textures.size()) return 0;
    static std::unordered_map<std::string, DDWORD> cache;
    const std::string &name = b.textures[surf.texture];
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    DDWORD flags = 0;
    std::string path = name;
    auto d = s.fs->read(path);
    if (d && d->size() >= 22 && path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".spr") == 0) {
        // Animated textures: the first frame's DTX (after five u32 and a u16 length).
        uint16_t len;
        memcpy(&len, &(*d)[20], 2);
        if (d->size() >= 22u + len) d = s.fs->read(std::string((const char *)&(*d)[22], len));
        else d.reset();
    }
    if (d && d->size() >= 24) memcpy(&flags, &(*d)[20], 4);
    cache[name] = flags;
    return flags;
}

int pointContainers(Server &s, const DVector &p, EngineObject **out, int max)
{
    // Containers are world models of type OT_CONTAINER. For now a point is
    // inside one when it is inside the model's bounding box.
    int n = 0;
    if (!s.world) return 0;
    for (EngineObject *o : objects().all()) {
        if (o->removed || o->type != OT_CONTAINER || o->bsp < 0 || size_t(o->bsp) >= s.world->models.size()) continue;
        const WorldBsp &b = s.world->models[size_t(o->bsp)];
        Placement pl = placementOf(o);
        Vec3 q = pl.toBuilt(v3(p));
        if (q.x < b.boxMin.x || q.y < b.boxMin.y || q.z < b.boxMin.z || q.x > b.boxMax.x || q.y > b.boxMax.y ||
            q.z > b.boxMax.z)
            continue;
        if (n < max && out) out[n] = o;
        ++n;
    }
    return std::min(n, max);
}

}  // namespace shogo
