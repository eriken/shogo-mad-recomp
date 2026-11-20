// Collision geometry: the solid polygons of a world's BSP models as triangles
// in a bounding-volume tree, and the queries movement and rays need.
//
// Moving objects are swept as ellipsoids with their dims as radii, using the
// swept-sphere method in "ellipsoid space" (positions divided by the radii):
// sweep a unit sphere against triangles, slide along what it hits.
#ifndef SHOGO_ENGINE_COLLISION_H
#define SHOGO_ENGINE_COLLISION_H

#include "engine/world_dat.h"

#include <cstdint>
#include <vector>

namespace shogo {

struct Tri {
    Vec3 a, b, c;
    Vec3 n;           // unit normal (the side the polygon faces)
    uint32_t flags = 0;  // surface flags
    uint32_t hpoly = 0xFFFFFFFF;  // HPOLY of the world polygon (see polyHandle), or INVALID_HPOLY
};

// HPOLY: world model index in the top 8 bits, polygon index below.
inline uint32_t polyHandle(size_t model, size_t poly) { return uint32_t(model) << 24 | uint32_t(poly & 0xFFFFFF); }

// One BSP model's solid triangles, in the coordinates the world was built in.
class CollisionModel {
public:
    void build(const WorldBsp &bsp, size_t modelIndex = 0);
    // Indices of triangles whose bounds overlap [mn, mx].
    void query(const Vec3 &mn, const Vec3 &mx, std::vector<int> &out) const;
    const std::vector<Tri> &tris() const { return m_tris; }
    bool empty() const { return m_tris.empty(); }

private:
    struct Node {
        Vec3 mn, mx;
        int left = -1, right = -1;  // children, or -1 for a leaf
        int first = 0, count = 0;   // leaf: range in m_order
    };
    int buildNode(int first, int count, int depth);
    std::vector<Tri> m_tris;
    std::vector<Vec3> m_triMin, m_triMax;
    std::vector<int> m_order;
    std::vector<Node> m_nodes;
};

// A triangle placed in the world for one move, with what it belongs to.
struct SweepTri {
    Tri tri;
    void *owner = nullptr;  // EngineObject*
};

struct SweepHit {
    bool hit = false;
    float distance = 0;  // along the move, in world units
    Vec3 point;          // world space
    Vec3 normal;         // world space, unit
    Vec3 face;           // the touched triangle's own normal (collideAndSlide only)
    void *owner = nullptr;
    uint32_t flags = 0;
    uint32_t hpoly = 0xFFFFFFFF;
};

// Moves an ellipsoid with radii `radii` from `from` by `move`, sliding along the
// triangles. Returns the end position; `hits` collects every blocking contact.
// wallNormalY > 0: upward faces with a normal y below it are slid along as
// vertical walls (for walking characters).
Vec3 collideAndSlide(const std::vector<SweepTri> &tris, const Vec3 &radii, const Vec3 &from, const Vec3 &move,
                     std::vector<SweepHit> *hits, float wallNormalY = 0);

// A walking character's move (FLAG_STAIRSTEP): slides, but not up faces
// steeper than 60 degrees; steps up onto floors up to stepHeight above its
// feet; and stands on a floor's own plane where its rounded bottom meets the
// floor's edge. `hits` gets the contacts to stop the velocity against.
Vec3 walkMove(const std::vector<SweepTri> &tris, const Vec3 &radii, const Vec3 &from, const Vec3 &move,
              float stepHeight, std::vector<SweepHit> &hits);

// First hit of the ellipsoid moving from `from` by `move` (no sliding).
SweepHit sweepEllipsoid(const std::vector<SweepTri> &tris, const Vec3 &radii, const Vec3 &from, const Vec3 &move);

// Segment [from, to] against triangles (front faces only unless twoSided).
// t is the fraction along the segment.
bool segmentTriangle(const Tri &t, const Vec3 &from, const Vec3 &to, bool twoSided, float *frac);

// Small vector helpers.
inline Vec3 operator+(const Vec3 &a, const Vec3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3 &a, const Vec3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3 &a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3 &a, const Vec3 &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 normalized(const Vec3 &v);
float length(const Vec3 &v);

}  // namespace shogo

#endif
