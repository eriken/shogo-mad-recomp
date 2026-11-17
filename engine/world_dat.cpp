#include "engine/world_dat.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace shogo {

namespace {
struct Reader {
    const std::vector<uint8_t> &d;
    size_t p = 0;

    void need(size_t n) const
    {
        if (p + n > d.size() || p + n < p) throw std::runtime_error("unexpected end of file at " + std::to_string(p));
    }
    template <class T> T get()
    {
        need(sizeof(T));
        T v;
        memcpy(&v, &d[p], sizeof(T));
        p += sizeof(T);
        return v;
    }
    uint8_t u8() { return get<uint8_t>(); }
    uint16_t u16() { return get<uint16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    float f32() { return get<float>(); }
    Vec3 vec() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    std::string str(size_t n)
    {
        need(n);
        std::string s((const char *)&d[p], n);
        p += n;
        return s;
    }
    void seek(size_t to)
    {
        if (to > d.size()) throw std::runtime_error("bad offset " + std::to_string(to));
        p = to;
    }
};

WorldProp readProp(Reader &r)
{
    WorldProp prop;
    prop.name = r.str(r.u16());
    prop.type = r.u8();
    prop.flags = r.u32();
    uint16_t len = r.u16();
    size_t end = r.p + len;
    switch (prop.type) {
    case WorldProp::String: prop.string = r.str(r.u16()); break;
    case WorldProp::Vector:
    case WorldProp::Color:
        for (int i = 0; i < 3; ++i) prop.vec[i] = r.f32();
        break;
    case WorldProp::Real:
    case WorldProp::LongInt: prop.vec[0] = r.f32(); break;
    case WorldProp::Flags: prop.bits = r.u32(); break;
    case WorldProp::Bool: prop.bits = r.u8(); break;
    case WorldProp::Rotation:
        for (int i = 0; i < 4; ++i) prop.vec[i] = r.f32();
        break;
    default: break;
    }
    r.seek(end);
    return prop;
}

// After the points, up to the block's end: a grid of polygon trees, one point
// per polygon (its lightmap origin), then the lightmaps. If anything doesn't
// add up the lightmaps are left out (vertex light still works).
void readLighting(Reader &r, WorldBsp &w, size_t end)
{
    try {
        uint32_t nx = r.u32(), ny = r.u32(), nz = r.u32();
        r.vec();  // cell size
        r.vec();  // grid origin
        uint64_t cells = uint64_t(nx) * ny * nz;
        if (cells > 1000000) return;
        for (uint64_t c = 0; c < cells; ++c) {
            uint16_t n = r.u16();
            r.u16();                 // root entry
            r.need(6 * size_t(n));   // per entry: u16 poly, i16 child[2] (-1/-2 empty)
            r.p += 6 * size_t(n);
        }
        r.u32();  // unidentified (0)
        r.u32();  // unidentified count
        std::vector<Vec3> origins(w.polys.size());
        for (Vec3 &o : origins) o = r.vec();
        uint32_t size = r.u32();
        if (r.p + size != end) return;
        size_t at = r.p;
        for (WorldPoly &poly : w.polys)
            if (w.surfaces[poly.surface].flags & 0x80) {
                if (at + 2 > end) return;
                size_t n = size_t(r.d[at]) * r.d[at + 1];
                if (at + 2 + 2 * n > end) return;
                at += 2 + 2 * n;
            }
        if (at != end) return;
        r.p = end - size;
        for (size_t i = 0; i < w.polys.size(); ++i) {
            WorldPoly &poly = w.polys[i];
            poly.lmOrigin = origins[i];
            if (!(w.surfaces[poly.surface].flags & 0x80)) continue;
            poly.lmW = r.u8();
            poly.lmH = r.u8();
            poly.lightmap.resize(size_t(poly.lmW) * poly.lmH);
            for (uint16_t &c : poly.lightmap) c = r.u16();
        }
        w.hasLightmaps = true;
    } catch (const std::exception &) {
    }
}

void readBsp(Reader &r, WorldBsp &w, size_t end)
{
    r.p += 32;  // reserved
    w.flags = r.u32();
    w.name = r.str(r.u16());
    r.u32();  // unidentified offset/size
    uint32_t nPoints = r.u32(), nPlanes = r.u32(), nSurfaces = r.u32(), nUserPortals = r.u32(), nPolies = r.u32(),
             nLeafs = r.u32(), nVerts = r.u32();
    r.u32();  // total vis list size
    r.u32();  // leaf lists
    uint32_t nNodes = r.u32();
    r.u32();  // unidentified
    (void)nVerts;
    w.boxMin = r.vec();
    w.boxMax = r.vec();
    w.translation = r.vec();

    uint32_t namesLen = r.u32(), nTextures = r.u32();
    size_t namesEnd = r.p + namesLen;
    r.need(namesLen);
    for (uint32_t i = 0; i < nTextures; ++i) {
        size_t start = r.p;
        while (r.p < namesEnd && r.d[r.p]) ++r.p;
        w.textures.emplace_back((const char *)&r.d[start], r.p - start);
        ++r.p;
    }
    r.seek(namesEnd);

    std::vector<std::pair<uint8_t, uint8_t>> polyCounts(nPolies);
    for (auto &c : polyCounts) {
        c.first = r.u8();
        c.second = r.u8();
    }

    // Leaves: portal/vis lists, then the polygons they touch. Not needed for drawing yet.
    for (uint32_t i = 0; i < nLeafs; ++i) {
        uint16_t nLists = r.u16();
        if (nLists == 0xFFFF) {
            r.u16();
        } else {
            for (uint16_t j = 0; j < nLists; ++j) {
                r.u16();
                uint16_t size = r.u16();
                r.p += size;
            }
        }
        uint16_t nPolyRefs = r.u16();
        r.p += 4 * size_t(nPolyRefs) + 4;
    }

    w.planes.resize(nPlanes);
    for (auto &pl : w.planes) {
        pl.normal = r.vec();
        pl.dist = r.f32();
    }

    w.surfaces.resize(nSurfaces);
    for (auto &s : w.surfaces) {
        s.O = r.vec();
        s.P = r.vec();
        s.Q = r.vec();
        s.lmP = r.vec();
        s.lmQ = r.vec();
        for (float &f : s.unknown) f = r.f32();
        s.texture = r.u16();
        s.plane = r.u32();
        s.flags = r.u32();
        s.unknown2 = r.u32();
        if (r.u8()) {  // texture effect, e.g. "pan" with "8 8" (scrolling sky)
            s.effect = r.str(r.u16());
            s.effectParams = r.str(r.u16());
        }
        s.tail[0] = r.u8();  // looks like an ambient/brightness byte
        s.tail[1] = r.u8();
    }

    w.polys.resize(nPolies);
    for (uint32_t i = 0; i < nPolies; ++i) {
        WorldPoly &poly = w.polys[i];
        poly.hdr[0] = r.u16();
        poly.hdr[1] = r.u16();
        r.p += 8;  // runtime pointers in the original engine
        poly.surface = r.u32();
        size_t n = size_t(polyCounts[i].first) + polyCounts[i].second;
        poly.verts.resize(n);
        for (auto &v : poly.verts) {
            v.point = r.u16();
            v.r = r.u8();
            v.g = r.u8();
            v.b = r.u8();
        }
        poly.verts.resize(polyCounts[i].first);
        if (poly.surface >= nSurfaces) throw std::runtime_error("model '" + w.name + "' polygon " + std::to_string(i) + " surface " + std::to_string(poly.surface) + " out of range at " + std::to_string(r.p));
    }

    w.nodes.resize(nNodes);
    for (auto &n : w.nodes) {
        n.plane = r.u32();
        n.poly = r.u32();
        n.leaf = r.u16();
        n.sides[0] = (int32_t)r.u32();
        n.sides[1] = (int32_t)r.u32();
        for (float &f : n.sphere) f = r.f32();
    }
    // The root is the one node no other node points to (not always node 0).
    {
        std::vector<uint8_t> referenced(w.nodes.size(), 0);
        for (const auto &n : w.nodes)
            for (int32_t c : n.sides)
                if (c >= 0 && size_t(c) < referenced.size()) referenced[size_t(c)] = 1;
        for (size_t i = 0; i < referenced.size(); ++i)
            if (!referenced[i]) {
                w.rootNode = int32_t(i);
                break;
            }
    }

    // User portals (named portals the game can open/close): name, flags, index, centre, dims.
    w.portals.resize(nUserPortals);
    for (auto &up : w.portals) {
        up.name = r.str(r.u16());
        up.flags = r.u32();
        up.index = r.u16();
        up.center = r.vec();
        up.dims = r.vec();
    }

    w.points.resize(nPoints);
    for (auto &pt : w.points) pt = r.vec();

    for (auto &poly : w.polys)
        for (auto &v : poly.verts)
            if (v.point >= nPoints) throw std::runtime_error("polygon point index out of range");
    for (auto &s : w.surfaces)
        if (s.texture >= w.textures.size() && !w.textures.empty())
            throw std::runtime_error("surface texture index out of range");
    readLighting(r, w, end);
}
}  // namespace

const WorldProp *WorldObject::find(const char *name) const
{
    for (const auto &p : props)
        if (strcasecmp(p.name.c_str(), name) == 0) return &p;
    return nullptr;
}

const WorldBsp *WorldData::model(const std::string &name) const
{
    for (const auto &m : models)
        if (strcasecmp(m.name.c_str(), name.c_str()) == 0) return &m;
    return nullptr;
}

std::unique_ptr<WorldData> WorldData::parse(const std::vector<uint8_t> &data, std::string *err)
{
    auto w = std::make_unique<WorldData>();
    try {
        Reader r{data};
        w->version = r.u32();
        if (w->version != 56) throw std::runtime_error("unsupported world version " + std::to_string(w->version));
        uint32_t objectPos = r.u32(), renderPos = r.u32();
        w->info = r.str(r.u32());

        r.seek(objectPos);
        uint32_t nObjects = r.u32();
        w->objects.resize(nObjects);
        for (auto &obj : w->objects) {
            uint16_t len = r.u16();
            size_t end = r.p + len;
            obj.className = r.str(r.u16());
            uint32_t nProps = r.u32();
            for (uint32_t i = 0; i < nProps; ++i) obj.props.push_back(readProp(r));
            r.seek(end);
        }

        // Main BSP, then a counted list of world models; each block starts with the offset of the next.
        r.seek(renderPos);
        uint32_t next = r.u32();
        w->models.emplace_back();
        readBsp(r, w->models.back(), next);
        r.seek(next);
        uint32_t nModels = r.u32();
        for (uint32_t i = 0; i < nModels; ++i) {
            next = r.u32();
            w->models.emplace_back();
            readBsp(r, w->models.back(), next);
            r.seek(next);
        }
    } catch (const std::exception &e) {
        if (err) *err = e.what();
        return nullptr;
    }
    return w;
}

bool WorldBsp::pointInside(const Vec3 &p) const
{
    int n = nodes.empty() ? -2 : rootNode;
    for (size_t guard = 0; n >= 0 && size_t(n) < nodes.size() && guard <= nodes.size(); ++guard) {
        const WorldNode &nd = nodes[size_t(n)];
        if (nd.plane >= planes.size()) return false;
        const WorldPlane &pl = planes[nd.plane];
        float d = pl.normal.x * p.x + pl.normal.y * p.y + pl.normal.z * p.z - pl.dist;
        n = d < 0 ? nd.sides[0] : nd.sides[1];
    }
    return n == -1;
}

}  // namespace shogo
