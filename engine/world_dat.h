// LithTech 1.0 world files (.DAT, version 56): the compiled BSP geometry and the
// object list a level starts with.
//
// The layout was worked out from the game's own data files; see
// docs/world-format.md. Fields we don't understand yet are kept raw or skipped.
#ifndef SHOGO_ENGINE_WORLD_DAT_H
#define SHOGO_ENGINE_WORLD_DAT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shogo {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// One property of a level object, in the game's PropDef types (serverobj_de.h PT_*).
struct WorldProp {
    enum Type : uint8_t { String = 0, Vector = 1, Color = 2, Real = 3, Flags = 4, Bool = 5, LongInt = 6, Rotation = 7 };
    std::string name;
    uint8_t type = 0;
    uint32_t flags = 0;
    std::string string;      // String
    float vec[4] = {};       // Vector/Color (3), Real/LongInt (1), Rotation (euler x,y,z + one more)
    uint32_t bits = 0;       // Flags, Bool
};

struct WorldObject {
    std::string className;
    std::vector<WorldProp> props;
    const WorldProp *find(const char *name) const;
};

struct WorldPlane {
    Vec3 normal;
    float dist = 0;
};

struct WorldSurface {
    Vec3 O, P, Q;          // texture mapping: u = dot(p - O, P), v = dot(p - O, Q)
    Vec3 lmP, lmQ;         // lightmap axes: P and Q normalized
    float unknown[3] = {}; // an RGB-looking triple (not yet identified)
    uint16_t texture = 0;  // index into WorldBsp::textures
    uint32_t plane = 0;
    uint32_t flags = 0;
    uint32_t unknown2 = 0;
    std::string effect, effectParams;  // optional texture effect ("pan", "8 8")
    uint8_t tail[2] = {};
};

struct WorldPolyVertex {
    uint16_t point = 0;
    uint8_t r = 0, g = 0, b = 0;  // baked vertex light
};

// World units between lightmap samples.
constexpr float kLightmapSpacing = 20.0f;

struct WorldPoly {
    uint16_t hdr[2] = {};  // not yet identified
    uint32_t surface = 0;
    std::vector<WorldPolyVertex> verts;  // the first numVerts entries are the polygon
    // Lightmap (lightmapped surfaces): lmW x lmH RGB565 samples, sample (i, j) at
    // lmOrigin + i * kLightmapSpacing * lmP + j * kLightmapSpacing * lmQ.
    Vec3 lmOrigin;
    uint8_t lmW = 0, lmH = 0;
    std::vector<uint16_t> lightmap;
};

struct WorldNode {
    uint32_t plane = 0, poly = 0;
    uint16_t leaf = 0xFFFF;
    int32_t sides[2] = {};  // child node index, or -1 / -2 for in / out
    float sphere[4] = {};   // centre + radius
};

struct WorldUserPortal {
    std::string name;
    uint32_t flags = 0;
    uint16_t index = 0;
    Vec3 center, dims;
};

struct WorldBsp {
    std::string name;  // "" for the main world, else a world model ("Sky", doors...)
    uint32_t flags = 0;
    Vec3 boxMin, boxMax, translation;
    std::vector<std::string> textures;
    std::vector<WorldPlane> planes;
    std::vector<WorldSurface> surfaces;
    std::vector<WorldPoly> polys;
    std::vector<WorldNode> nodes;
    int32_t rootNode = 0;  // the node no other node points to
    std::vector<WorldUserPortal> portals;
    std::vector<Vec3> points;
    bool hasLightmaps = false;  // the lighting data decoded

    // True if p is in open space (an "in" leaf of the BSP), false in solid or
    // outside the level. Starts at rootNode; child [0] is the plane's negative side.
    bool pointInside(const Vec3 &p) const;
};

struct WorldData {
    uint32_t version = 0;
    std::string info;  // "MasterPalette ...;WorldName ..."
    std::vector<WorldObject> objects;
    std::vector<WorldBsp> models;  // [0] is the main world

    static std::unique_ptr<WorldData> parse(const std::vector<uint8_t> &data, std::string *err);
    const WorldBsp *model(const std::string &name) const;
};

}  // namespace shogo

#endif
