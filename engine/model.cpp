#include "engine/model.h"

#include "engine/api_common.h"
#include "engine/collision.h"
#include "engine/effects.h"
#include "engine/math3d.h"
#include "engine/objects.h"
#include "engine/rez_file.h"
#include "engine/stub_support.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unordered_map>

namespace shogo {

namespace {

struct Reader {
    const std::vector<uint8_t> &d;
    size_t p = 0;
    bool ok = true;
    explicit Reader(const std::vector<uint8_t> &data) : d(data) {}
    bool need(size_t n)
    {
        if (!ok || p + n > d.size()) ok = false;
        return ok;
    }
    uint32_t u32()
    {
        if (!need(4)) return 0;
        uint32_t v;
        memcpy(&v, &d[p], 4);
        p += 4;
        return v;
    }
    uint16_t u16()
    {
        if (!need(2)) return 0;
        uint16_t v;
        memcpy(&v, &d[p], 2);
        p += 2;
        return v;
    }
    float f32()
    {
        uint32_t u = u32();
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    std::string str()
    {
        uint16_t n = u16();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char *>(&d[p]), n);
        p += n;
        return s;
    }
    void skip(size_t n)
    {
        if (need(n)) p += n;
    }
};

// Nodes are stored depth first: bbox, name, index, flags, the vertices the
// node animates directly, child count.
bool readNodes(Reader &r, std::vector<ModelNode> &nodes, std::vector<int> &fileIndex, int parent, int depth)
{
    if (depth > 64 || nodes.size() > 4096) return false;
    r.skip(24);
    ModelNode n;
    n.name = r.str();
    int index = r.u16();
    r.skip(1);
    uint32_t nv = r.u32();
    if (!r.ok || nv > 65536) return false;
    for (uint32_t i = 0; i < nv; ++i) n.mdVerts.push_back(r.u16());
    n.parent = parent;
    int me = (int)nodes.size();
    nodes.push_back(std::move(n));
    fileIndex.push_back(index);
    uint32_t children = r.u32();
    if (!r.ok || children > 4096) return false;
    for (uint32_t i = 0; i < children; ++i)
        if (!readNodes(r, nodes, fileIndex, me, depth + 1)) return false;
    return r.ok;
}

Vec3 readVec(Reader &r)
{
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

const ResourceFS *g_fs = nullptr;

}  // namespace

int ModelInfo::find(const char *name) const
{
    if (!name) return -1;
    for (size_t i = 0; i < anims.size(); ++i)
        if (strcasecmp(anims[i].name.c_str(), name) == 0) return (int)i;
    return -1;
}

int ModelInfo::findNode(const char *name) const
{
    if (!name) return -1;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (strcasecmp(nodes[i].name.c_str(), name) == 0) return (int)i;
    return -1;
}

namespace {

// The keyframes around `timeMs` and how far between them it is.
void keysAround(const ModelAnim *a, float timeMs, size_t *k0, size_t *k1, float *f)
{
    *k0 = *k1 = 0;
    *f = 0;
    if (!a || a->keys.empty()) return;
    size_t n = a->keys.size();
    while (*k0 + 1 < n && (float)a->keys[*k0 + 1].time <= timeMs) ++*k0;
    *k1 = *k0 + 1 < n ? *k0 + 1 : *k0;
    float t0 = (float)a->keys[*k0].time, t1 = (float)a->keys[*k1].time;
    *f = t1 > t0 ? std::clamp((timeMs - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
}

// Each node's transform relative to its parent; nodes the animation doesn't
// move keep the identity (false when it moves none).
bool localPose(const ModelInfo &m, const ModelAnim *a, size_t k0, size_t k1, float f, std::vector<NodeTransform> &local)
{
    const size_t nNodes = m.nodes.size();
    if (!a || a->tracks.size() != nNodes) return false;
    for (size_t i = 0; i < nNodes; ++i) {
        if (a->tracks[i].empty()) continue;
        const NodeKey &p = a->tracks[i][k0], &q = a->tracks[i][k1];
        local[i].pos = p.pos + (q.pos - p.pos) * f;
        local[i].rot = f > 0 ? quatSlerp(p.rot, q.rot, f) : p.rot;
    }
    return true;
}

}  // namespace

void poseModel(const ModelInfo &m, int anim, float timeMs, std::vector<NodeTransform> *outNodes,
               std::vector<Vec3> *positions, std::vector<Vec3> *normals, const PoseBlend *from)
{
    const size_t nNodes = m.nodes.size();
    std::vector<NodeTransform> local(nNodes), world(nNodes);
    for (NodeTransform &t : local) t.rot = quatIdentity();
    const ModelAnim *a = anim >= 0 && anim < (int)m.anims.size() ? &m.anims[(size_t)anim] : nullptr;
    size_t k0, k1;
    float f;
    keysAround(a, timeMs, &k0, &k1, &f);
    localPose(m, a, k0, k1, f, local);
    // An animation transition: from the old pose to this one, node by node.
    if (from && from->weight > 0 && from->anim >= 0 && from->anim < (int)m.anims.size()) {
        const ModelAnim *b = &m.anims[(size_t)from->anim];
        std::vector<NodeTransform> old(nNodes);
        for (NodeTransform &t : old) t.rot = quatIdentity();
        size_t b0, b1;
        float bf;
        keysAround(b, from->timeMs, &b0, &b1, &bf);
        if (localPose(m, b, b0, b1, bf, old)) {
            const float w = std::min(from->weight, 1.0f);
            for (size_t i = 0; i < nNodes; ++i) {
                local[i].pos = local[i].pos + (old[i].pos - local[i].pos) * w;
                local[i].rot = quatSlerp(local[i].rot, old[i].rot, w);
            }
        }
    }
    for (size_t i = 0; i < nNodes; ++i) {
        int parent = m.nodes[i].parent;
        if (parent < 0) {
            world[i] = local[i];
            continue;
        }
        const NodeTransform &pt = world[(size_t)parent];
        DVector lp = quatRotate(pt.rot, DVector(local[i].pos.x, local[i].pos.y, local[i].pos.z));
        world[i].pos = pt.pos + Vec3{lp.x, lp.y, lp.z};
        world[i].rot = quatMul(pt.rot, local[i].rot);
    }
    if (positions || normals) {
        std::vector<Vec3> nodeLocal(m.verts.size());
        for (size_t v = 0; v < m.verts.size(); ++v) nodeLocal[v] = m.verts[v].pos;
        // Vertex animation: decoded positions from the nearest keyframe.
        if (a && a->vertexKeys.size() == nNodes) {
            size_t k = f < 0.5f ? k0 : k1;
            for (size_t i = 0; i < nNodes; ++i) {
                const std::vector<uint16_t> &md = m.nodes[i].mdVerts;
                if (md.empty() || a->vertexKeys[i].empty()) continue;
                const uint8_t *b = &a->vertexKeys[i][k * md.size() * 3];
                const Vec3 &sc = a->vertexScale[i], &tr = a->vertexTranslate[i];
                for (size_t j = 0; j < md.size(); ++j)
                    if (md[j] < nodeLocal.size())
                        nodeLocal[md[j]] = {b[j * 3] * sc.x + tr.x, b[j * 3 + 1] * sc.y + tr.y, b[j * 3 + 2] * sc.z + tr.z};
            }
        }
        if (positions) positions->resize(m.verts.size());
        if (normals) normals->resize(m.verts.size());
        for (size_t v = 0; v < m.verts.size(); ++v) {
            const NodeTransform &t = world[(size_t)m.verts[v].node];
            if (positions) {
                const Vec3 &l = nodeLocal[v];
                DVector r = quatRotate(t.rot, DVector(l.x, l.y, l.z));
                (*positions)[v] = t.pos + Vec3{r.x, r.y, r.z};
            }
            if (normals) {
                const Vec3 &n = m.verts[v].normal;
                DVector r = quatRotate(t.rot, DVector(n.x, n.y, n.z));
                (*normals)[v] = {r.x, r.y, r.z};
            }
        }
    }
    // The files are mirrored relative to the world (3ds Max's right-handed
    // space; the triangles wind the other way and characters face -z):
    // mirror z on the way out.
    if (positions)
        for (Vec3 &p : *positions) p.z = -p.z;
    if (normals)
        for (Vec3 &n : *normals) n.z = -n.z;
    if (outNodes) {
        for (NodeTransform &t : world) {
            t.pos.z = -t.pos.z;
            t.rot.m_Vec.x = -t.rot.m_Vec.x;
            t.rot.m_Vec.y = -t.rot.m_Vec.y;
        }
        *outNodes = std::move(world);
    }
}

std::unique_ptr<ModelInfo> parseModel(const std::vector<uint8_t> &data, std::string *err)
{
    auto fail = [&](const char *m) {
        if (err) *err = m;
        return nullptr;
    };
    // Find the sections.
    struct Section { size_t start = 0, end = 0; bool found = false; };
    Section header, geom, nodes, anim, dims;
    {
        Reader r(data);
        for (int guard = 0; guard < 64 && r.p < data.size(); ++guard) {
            std::string name = r.str();
            uint32_t next = r.u32();
            if (!r.ok) return fail("truncated section header");
            size_t end = next == 0xFFFFFFFFu ? data.size() : next;
            if (end < r.p || end > data.size()) return fail("bad section offset");
            Section s{r.p, end, true};
            if (name == "Header") header = s;
            else if (name == "Geometry") geom = s;
            else if (name == "Nodes") nodes = s;
            else if (name == "Animation") anim = s;
            else if (name == "AnimDims") dims = s;
            if (next == 0xFFFFFFFFu) break;
            r.p = next;
        }
    }
    if (!nodes.found || !anim.found) return fail("no Nodes/Animation section");

    auto info = std::make_unique<ModelInfo>();
    if (header.found) {
        // Header: the exporter's version string, then the command string.
        Reader h(data);
        h.p = header.start;
        h.str();
        std::string cmd = h.str();
        if (h.ok && h.p <= header.end) info->commandString = cmd;
    }
    std::vector<int> fileIndex;  // tree order -> the node's index field
    {
        Reader r(data);
        r.p = nodes.start;
        if (!readNodes(r, info->nodes, fileIndex, -1, 0)) return fail("bad node tree");
    }
    const size_t nNodes = info->nodes.size();

    // Geometry: bbox, a LOD table (u32 n, n + 1 u16), triangles (three UV
    // pairs, three u16 vertex indices, a packed normal), then u32 vertex
    // count, u32 count of vertices the full-detail mesh uses, and 20-byte
    // vertices (position, packed normal, node index, a LOD link).
    if (geom.found) {
        Reader g(data);
        g.p = geom.start;
        g.skip(24);
        uint32_t nLod = g.u32();
        if (nLod > 65536) return fail("bad LOD table");
        g.skip(((size_t)nLod + 1) * 2);
        uint32_t nTris = g.u32();
        if (nTris > 1000000) return fail("bad triangle count");
        info->tris.resize(nTris);
        for (ModelTri &t : info->tris) {
            for (float &f : t.uv) f = g.f32();
            for (uint16_t &v : t.v) v = g.u16();
            g.skip(3);
        }
        uint32_t nVerts = g.u32(), nUsed = g.u32();
        if (!g.ok || nVerts > 1000000 || nUsed > nVerts) return fail("bad vertex count");
        std::vector<int> byIndex(256, 0);
        for (size_t i = 0; i < nNodes; ++i)
            if (fileIndex[i] >= 0 && fileIndex[i] < 256) byIndex[(size_t)fileIndex[i]] = (int)i;
        info->verts.resize(nUsed);
        for (uint32_t i = 0; i < nVerts && g.ok; ++i) {
            Vec3 pos = readVec(g);
            if (!g.need(8)) break;
            const int8_t n[3] = {(int8_t)data[g.p], (int8_t)data[g.p + 1], (int8_t)data[g.p + 2]};
            uint8_t node = data[g.p + 3];
            g.p += 8;
            if (i >= nUsed) continue;  // lower-detail vertices
            ModelVertex &v = info->verts[i];
            v.pos = pos;
            v.normal = {n[0] / 127.0f, n[1] / 127.0f, n[2] / 127.0f};
            v.node = byIndex[node];
        }
        if (!g.ok || g.p > geom.end) return fail("truncated geometry");
        for (const ModelTri &t : info->tris)
            for (uint16_t v : t.v)
                if (v >= nUsed) return fail("triangle uses a missing vertex");
    }

    Reader r(data);
    r.p = anim.start;
    uint32_t count = r.u32();
    if (count > 4096) return fail("bad animation count");
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        ModelAnim a;
        a.name = r.str();
        a.length = r.u32();
        r.skip(24);
        uint32_t nKeys = r.u32();
        if (nKeys > 100000) return fail("bad keyframe count");
        for (uint32_t k = 0; k < nKeys && r.ok; ++k) {
            ModelKey key;
            key.time = r.u32();
            r.skip(24);
            key.text = r.str();
            a.keys.push_back(std::move(key));
        }
        // Per node: per keyframe a position and a rotation (x y z w); then,
        // for every keyframe in turn, the node's animated vertices (3 bytes
        // each); then the vertex scale and translation.
        a.tracks.resize(nNodes);
        a.vertexKeys.resize(nNodes);
        a.vertexScale.resize(nNodes);
        a.vertexTranslate.resize(nNodes);
        for (size_t n = 0; n < nNodes && r.ok; ++n) {
            size_t nv = info->nodes[n].mdVerts.size();
            a.tracks[n].resize(nKeys);
            for (uint32_t k = 0; k < nKeys && r.ok; ++k) {
                NodeKey &key = a.tracks[n][k];
                key.pos = readVec(r);
                float x = r.f32(), y = r.f32(), z = r.f32(), w = r.f32();
                key.rot.m_Vec.x = -x;  // stored inverted
                key.rot.m_Vec.y = -y;
                key.rot.m_Vec.z = -z;
                key.rot.m_Spin = w;
            }
            const size_t bytes = (size_t)nKeys * nv * 3;
            if (nv && r.need(bytes)) {
                a.vertexKeys[n].assign(data.begin() + (ptrdiff_t)r.p, data.begin() + (ptrdiff_t)(r.p + bytes));
                r.p += bytes;
            }
            a.vertexScale[n] = readVec(r);
            a.vertexTranslate[n] = readVec(r);
        }
        if (r.p > anim.end) return fail("animation data overruns its section");
        info->anims.push_back(std::move(a));
    }
    if (!r.ok) return fail("truncated animation data");

    if (dims.found) {
        Reader d(data);
        d.p = dims.start;
        for (ModelAnim &a : info->anims) {
            if (d.p + 12 > dims.end) break;
            a.dims.x = d.f32();
            a.dims.y = d.f32();
            a.dims.z = d.f32();
        }
    }
    return info;
}

void setModelFS(const ResourceFS *fs) { g_fs = fs; }

const ModelInfo *objectModel(EngineObject *o)
{
    if (!o || o->type != OT_MODEL) return nullptr;
    if (o->modelLoaded && o->modelFile == o->filename) return o->model;

    static std::unordered_map<std::string, std::unique_ptr<ModelInfo>> cache;
    o->modelLoaded = true;
    o->modelFile = o->filename;
    o->model = nullptr;
    if (!o->filename.empty() && g_fs) {
        std::string key = RezFile::key(o->filename);
        auto it = cache.find(key);
        if (it == cache.end()) {
            std::unique_ptr<ModelInfo> m;
            if (auto data = g_fs->read(o->filename)) {
                std::string err;
                m = parseModel(*data, &err);
                if (!m) fprintf(stderr, "[model] %s: %s\n", o->filename.c_str(), err.c_str());
            }
            it = cache.emplace(key, std::move(m)).first;
        }
        o->model = it->second.get();
    }
    o->anim = o->model && !o->model->anims.empty() ? 0 : -1;
    if (o->anim >= 0 && !(o->clientFlags & CF_DONTSETDIMS)) o->dims = o->model->anims[0].dims;
    o->animTime = 0;
    o->animDone = false;
    o->animStarted = false;
    return o->model;
}

namespace {

void fireKey(EngineObject *o, const std::string &text, const ModelKeyFn &onKey)
{
    if (text.empty() || !onKey) return;
    std::vector<char> cmd(text.begin(), text.end());
    cmd.push_back(0);
    char *pos = cmd.data();
    for (int guard = 0; guard < 16 && pos && *pos; ++guard) {
        char buf[512];
        char *args[32];
        int n = 0;
        char *next = nullptr;
        int more = parseCommand(pos, &next, buf, args, &n);
        if (n > 0) {
            ArgList list{args, n};
            onKey(o, &list);
            if (o->magic != EngineObject::kMagic || o->removed) return;
        }
        if (!more) break;
        pos = next;
    }
}

// Fires the keys with time in (from, to], or [from, to] when inclusive.
void fireRange(EngineObject *o, const ModelAnim &a, float from, float to, bool inclusive, const ModelKeyFn &onKey)
{
    for (const ModelKey &k : a.keys) {
        float t = (float)k.time;
        if ((inclusive ? t >= from : t > from) && t <= to) {
            fireKey(o, k.text, onKey);
            if (o->magic != EngineObject::kMagic || o->removed) return;
        }
    }
}

}  // namespace

void poseObject(EngineObject *o, std::vector<NodeTransform> *nodes, std::vector<Vec3> *positions,
                std::vector<Vec3> *normals)
{
    const ModelInfo *m = objectModel(o);
    if (!m) return;
    PoseBlend from;
    if (o->blendAnim >= 0 && o->blendElapsed < kAnimTransitionMs) {
        from.anim = o->blendAnim;
        from.timeMs = o->blendTime;
        from.weight = 1.0f - o->blendElapsed / kAnimTransitionMs;
    }
    poseModel(*m, o->anim, o->animTime, nodes, positions, normals, from.anim >= 0 ? &from : nullptr);
}

void animateObject(EngineObject *o, float dt, const ModelKeyFn &onKey)
{
    const ModelInfo *m = objectModel(o);
    if (!m || o->anim < 0 || o->anim >= (int)m->anims.size()) return;
    if (o->blendAnim >= 0) {
        o->blendElapsed += dt * 1000.0f;
        if (o->blendElapsed >= kAnimTransitionMs) o->blendAnim = -1;
    }
    const ModelAnim &a = m->anims[(size_t)o->anim];
    float len = (float)a.length;
    float from = o->animTime;
    bool inclusive = !o->animStarted;
    o->animStarted = true;
    if (o->animDone) return;

    float to = from + dt * 1000.0f;
    if (len <= 0) {
        o->animTime = 0;
        if (inclusive) fireRange(o, a, -1, 0, true, onKey);
        if (!o->animLoop) o->animDone = true;
        return;
    }
    if (to < len) {
        o->animTime = to;
        fireRange(o, a, from, to, inclusive, onKey);
        return;
    }
    if (!o->animLoop) {
        o->animTime = len;
        o->animDone = true;
        fireRange(o, a, from, len, inclusive, onKey);
        return;
    }
    // Looping: finish this pass, then wrap (possibly more than once on a long frame).
    fireRange(o, a, from, len, inclusive, onKey);
    to -= len;
    while (to >= len) to -= len;
    if (o->magic != EngineObject::kMagic || o->removed) return;
    fireRange(o, a, 0, to, true, onKey);
    o->animTime = to;
}


// ---- nodes and attachments -------------------------------------------------------------------

bool objectNodeTransform(EngineObject *o, int node, Vec3 *pos, DRotation *rot)
{
    const ModelInfo *m = objectModel(o);
    if (!m || node < 0 || node >= (int)m->nodes.size()) return false;
    std::vector<NodeTransform> nodes;
    poseObject(o, &nodes, nullptr, nullptr);
    DVector sc = o->scale;
    if (sc.x == 0 && sc.y == 0 && sc.z == 0) sc = DVector(1, 1, 1);
    const NodeTransform &t = nodes[(size_t)node];
    DVector p = quatRotate(o->rot, DVector(t.pos.x * sc.x, t.pos.y * sc.y, t.pos.z * sc.z));
    if (pos) *pos = {o->pos.x + p.x, o->pos.y + p.y, o->pos.z + p.z};
    if (rot) *rot = quatMul(o->rot, t.rot);
    return true;
}

namespace {

struct Attachment {
    static constexpr uint32_t kMagic = 0x41545431;  // "ATT1"
    uint32_t magic = kMagic;
    EngineObject *parent = nullptr;
    uint32_t child = 0;  // object id: the child may go away on its own
    int node = -1;
    DVector offset{};
    DRotation rotOffset{0, 0, 0, 1};
};
std::vector<std::unique_ptr<Attachment>> g_attachments;

Attachment *toAttachment(HATTACHMENT h)
{
    auto *a = reinterpret_cast<Attachment *>(h);
    for (const auto &p : g_attachments)
        if (p.get() == a) return a;
    return nullptr;
}

void placeAttachment(Attachment &a)
{
    EngineObject *child = objects().find(a.child);
    if (!child || child->removed || !a.parent || a.parent->removed) return;
    Vec3 base{a.parent->pos.x, a.parent->pos.y, a.parent->pos.z};
    DRotation rot = a.parent->rot;
    if (a.node >= 0) objectNodeTransform(a.parent, a.node, &base, &rot);
    DVector off = quatRotate(rot, a.offset);
    child->pos = DVector(base.x + off.x, base.y + off.y, base.z + off.z);
    child->rot = quatMul(rot, a.rotOffset);
}

int nodeIndex(EngineObject *o, const char *name)
{
    const ModelInfo *m = objectModel(o);
    return m ? m->findNode(name) : -1;
}

DRESULT createAttachment(HOBJECT hParent, HOBJECT hChild, char *node, DVector *offset, DRotation *rotOffset,
                         HATTACHMENT *out)
{
    if (out) *out = nullptr;
    EngineObject *parent = toObject(hParent), *child = toObject(hChild);
    if (!parent || !child) return DE_INVALIDPARAMS;
    auto a = std::make_unique<Attachment>();
    a->parent = parent;
    a->child = child->id;
    if (node && *node) {
        a->node = nodeIndex(parent, node);
        if (a->node < 0) return DE_NODENOTFOUND;
    }
    if (offset) a->offset = *offset;
    if (rotOffset) a->rotOffset = *rotOffset;
    if (a->rotOffset.m_Vec.x == 0 && a->rotOffset.m_Vec.y == 0 && a->rotOffset.m_Vec.z == 0 && a->rotOffset.m_Spin == 0)
        a->rotOffset = quatIdentity();
    placeAttachment(*a);
    if (out) *out = reinterpret_cast<HATTACHMENT>(a.get());
    g_attachments.push_back(std::move(a));
    return LT_OK;
}

}  // namespace

std::vector<AttachmentRecord> attachmentRecords()
{
    std::vector<AttachmentRecord> out;
    for (const auto &a : g_attachments) {
        if (!a->parent || a->parent->removed) continue;
        out.push_back({a->parent->id, a->child, a->node, a->offset, a->rotOffset});
    }
    return out;
}

void restoreAttachment(EngineObject *parent, EngineObject *child, const AttachmentRecord &r)
{
    if (!parent || !child) return;
    for (const auto &a : g_attachments)
        if (a->parent == parent && a->child == child->id) return;
    auto a = std::make_unique<Attachment>();
    a->parent = parent;
    a->child = child->id;
    const ModelInfo *m = objectModel(parent);
    a->node = (m && r.node < (int)m->nodes.size()) ? r.node : -1;
    a->offset = r.offset;
    a->rotOffset = r.rotOffset;
    placeAttachment(*a);
    g_attachments.push_back(std::move(a));
}

namespace {

DRESULT removeAttachment(HATTACHMENT h)
{
    Attachment *a = toAttachment(h);
    if (!a) return LT_ERROR;
    a->magic = 0;
    g_attachments.erase(std::remove_if(g_attachments.begin(), g_attachments.end(),
                                       [a](const std::unique_ptr<Attachment> &p) { return p.get() == a; }),
                        g_attachments.end());
    return LT_OK;
}

DRESULT findAttachment(HOBJECT hParent, HOBJECT hChild, HATTACHMENT *out)
{
    if (out) *out = nullptr;
    EngineObject *parent = toObject(hParent), *child = toObject(hChild);
    if (!parent || !child) return LT_ERROR;
    for (const auto &a : g_attachments)
        if (a->parent == parent && a->child == child->id) {
            if (out) *out = reinterpret_cast<HATTACHMENT>(a.get());
            return LT_OK;
        }
    return DE_NOTFOUND;
}

DRESULT getAttachedModelNodeTransform(HATTACHMENT h, char *childNode, DVector *pos, DRotation *rot)
{
    Attachment *a = toAttachment(h);
    if (!a) return LT_ERROR;
    placeAttachment(*a);
    EngineObject *child = objects().find(a->child);
    int n = nodeIndex(child, childNode);
    Vec3 p;
    DRotation r;
    if (n < 0 || !objectNodeTransform(child, n, &p, &r)) return DE_NODENOTFOUND;
    if (pos) *pos = DVector(p.x, p.y, p.z);
    if (rot) *rot = r;
    return LT_OK;
}

DRESULT getAttachments(HLOCALOBJ hObj, HLOCALOBJ *list, DDWORD listSize, DDWORD *outListSize, DDWORD *outNum)
{
    EngineObject *o = toObject(hObj);
    DDWORD n = 0, filled = 0;
    if (o)
        for (const auto &a : g_attachments) {
            if (a->parent != o) continue;
            EngineObject *child = objects().find(a->child);
            if (!child) continue;
            if (list && filled < listSize) list[filled++] = toHLocal(child);
            ++n;
        }
    if (outListSize) *outListSize = filled;
    if (outNum) *outNum = n;
    return LT_OK;
}

DRESULT vProcessAttachments(void *, HOBJECT h)
{
    EngineObject *o = toObject(h);
    if (!o) return LT_ERROR;
    updateAttachments(o);
    return LT_OK;
}

DBOOL getModelNodeTransform(HOBJECT h, char *name, DVector *pos, DRotation *rot)
{
    EngineObject *o = toObject(h);
    Vec3 p;
    DRotation r;
    if (!objectNodeTransform(o, nodeIndex(o, name), &p, &r)) return DFALSE;
    if (pos) *pos = DVector(p.x, p.y, p.z);
    if (rot) *rot = r;
    return DTRUE;
}

// Nodes are numbered in tree order; iteration starts from INVALID_MODEL_NODE.
DRESULT getNextModelNode(HOBJECT h, HMODELNODE node, HMODELNODE *next)
{
    const ModelInfo *m = objectModel(toObject(h));
    if (!m || !next) return LT_ERROR;
    HMODELNODE n = node == INVALID_MODEL_NODE ? 0 : node + 1;
    if (n >= m->nodes.size()) return DE_FINISHED;
    *next = n;
    return LT_OK;
}

DRESULT getModelNodeName(HOBJECT h, HMODELNODE node, char *name, DDWORD maxLen)
{
    const ModelInfo *m = objectModel(toObject(h));
    if (!m || node >= m->nodes.size() || !name || maxLen == 0) return LT_ERROR;
    snprintf(name, maxLen, "%s", m->nodes[node].name.c_str());
    return LT_OK;
}

DRESULT setModelNodeHideStatus(HOBJECT h, char *name, DBOOL hidden)
{
    EngineObject *o = toObject(h);
    const ModelInfo *m = objectModel(o);
    if (!m) return LT_ERROR;
    int n = m->findNode(name);
    if (n < 0) return DE_NODENOTFOUND;
    o->hiddenNodes.resize(m->nodes.size(), false);
    if (o->hiddenNodes[(size_t)n] == (hidden != 0)) return LT_NOCHANGE;
    o->hiddenNodes[(size_t)n] = hidden != 0;
    return LT_OK;
}

DRESULT getModelNodeHideStatus(HOBJECT h, char *name, DBOOL *hidden)
{
    EngineObject *o = toObject(h);
    const ModelInfo *m = objectModel(o);
    if (!m) return LT_ERROR;
    int n = m->findNode(name);
    if (n < 0) return DE_NODENOTFOUND;
    if (hidden) *hidden = (size_t)n < o->hiddenNodes.size() && o->hiddenNodes[(size_t)n];
    return LT_OK;
}

DRESULT getModelCommandString(HOBJECT h, char *str, DDWORD maxLen)
{
    const ModelInfo *m = objectModel(toObject(h));
    if (!m || !str || maxLen == 0) return LT_ERROR;
    snprintf(str, maxLen, "%s", m->commandString.c_str());
    return LT_OK;
}

}  // namespace

void updateAttachments(EngineObject *parent)
{
    // Children can be parents too (a gun on a hand on a mech): a few passes settle chains.
    for (int pass = 0; pass < 3; ++pass)
        for (const auto &a : g_attachments)
            if (!parent || a->parent == parent) placeAttachment(*a);
}

void objectRemoved(EngineObject *o)
{
    effectsObjectRemoved(o);
    // The engine detaches everything from a removed parent; a removed child's
    // attachments go too (its id would never come back).
    g_attachments.erase(std::remove_if(g_attachments.begin(), g_attachments.end(),
                                       [o](const std::unique_ptr<Attachment> &a) {
                                           if (a->parent != o && a->child != o->id) return false;
                                           a->magic = 0;
                                           return true;
                                       }),
                        g_attachments.end());
}

// The animation API (the same for the client and the server).
namespace {

const ModelAnim *currentAnim(EngineObject *o)
{
    const ModelInfo *m = objectModel(o);
    if (!m || o->anim < 0 || o->anim >= (int)m->anims.size()) return nullptr;
    return &m->anims[(size_t)o->anim];
}

HMODELANIM getAnimIndex(HOBJECT h, char *name)
{
    const ModelInfo *m = objectModel(toObject(h));
    int i = m ? m->find(name) : -1;
    return i < 0 ? (HMODELANIM)-1 : (HMODELANIM)i;
}

void setModelAnimation(HOBJECT h, HMODELANIM a)
{
    EngineObject *o = toObject(h);
    const ModelInfo *m = objectModel(o);
    static bool trace = getenv("SHOGO_TRACE_ANIM") != nullptr;
    if (trace && m && a < m->anims.size() && o->anim != (int)a)
        printf("[anim] %s%s -> %s%s\n", o->clientOnly ? "(client) " : "", o->filename.c_str(), m->anims[a].name.c_str(),
               (o->flags & FLAG_ANIMTRANSITION) && o->anim >= 0 ? " (200 ms transition)" : "");
    if (!m || a >= m->anims.size()) return;
    // Setting the animation it's already on doesn't restart it.
    if (o->anim == (int)a) return;
    // FLAG_ANIMTRANSITION: blend from where the old animation was over 200 ms.
    // (Mid-transition, the new one starts from the old animation's pose, not
    // the blend on screen.)
    if ((o->flags & FLAG_ANIMTRANSITION) && o->anim >= 0) {
        o->blendAnim = o->anim;
        o->blendTime = o->animTime;
        o->blendElapsed = 0;
    } else {
        o->blendAnim = -1;
    }
    o->anim = (int)a;
    // The engine resizes a client object to the animation's dims (without collision),
    // unless it opts out with CF_DONTSETDIMS. Server objects keep their dims: the game
    // sizes them itself, before it changes the animation (CBaseCharacter::SetAnimation),
    // and on foot the player is 1.1 times its animation's dims.
    if (o->clientOnly && !(o->clientFlags & CF_DONTSETDIMS)) o->dims = m->anims[a].dims;
    o->animTime = 0;
    o->animDone = false;
    o->animStarted = false;
}

HMODELANIM getModelAnimation(HOBJECT h)
{
    EngineObject *o = toObject(h);
    return objectModel(o) && o->anim >= 0 ? (HMODELANIM)o->anim : (HMODELANIM)-1;
}

DRESULT resetModelAnimation(HOBJECT h)
{
    EngineObject *o = toObject(h);
    if (!objectModel(o)) return LT_ERROR;
    o->animTime = 0;
    o->animDone = false;
    o->animStarted = false;
    return LT_OK;
}

DDWORD getModelPlaybackState(HOBJECT h)
{
    EngineObject *o = toObject(h);
    return currentAnim(o) && o->animDone ? MS_PLAYDONE : 0;
}

DBOOL getModelLooping(HOBJECT h)
{
    EngineObject *o = toObject(h);
    return o ? o->animLoop : DFALSE;
}

void setModelLooping(HOBJECT h, DBOOL loop)
{
    EngineObject *o = toObject(h);
    if (!o) return;
    o->animLoop = loop != 0;
    // A looping animation that had finished starts playing again.
    if (o->animLoop) o->animDone = false;
}

DRESULT vGetModelAnimUserDims(void *, HOBJECT h, DVector *dims, HMODELANIM a)
{
    return modelAnimUserDims(h, dims, a);
}

}  // namespace

DRESULT modelAnimUserDims(HOBJECT h, DVector *dims, HMODELANIM a)
{
    EngineObject *o = toObject(h);
    if (!o || !dims) return LT_ERROR;
    const ModelInfo *m = objectModel(o);
    if (!m || a >= m->anims.size()) {
        *dims = o->dims;
        return m ? LT_ERROR : LT_OK;
    }
    *dims = m->anims[a].dims;
    return LT_OK;
}

void installModelAPI(ServerDE *d)
{
    d->GetAnimIndex = getAnimIndex;
    d->SetModelAnimation = setModelAnimation;
    d->GetModelAnimation = getModelAnimation;
    d->ResetModelAnimation = resetModelAnimation;
    d->GetModelPlaybackState = getModelPlaybackState;
    d->GetModelLooping = getModelLooping;
    d->SetModelLooping = setModelLooping;
    d->CreateAttachment = createAttachment;
    d->RemoveAttachment = removeAttachment;
    d->FindAttachment = findAttachment;
    d->GetAttachedModelNodeTransform = getAttachedModelNodeTransform;
    d->GetModelNodeTransform = getModelNodeTransform;
    d->GetNextModelNode = getNextModelNode;
    d->GetModelNodeName = getModelNodeName;
    d->SetModelNodeHideStatus = setModelNodeHideStatus;
    d->GetModelNodeHideStatus = getModelNodeHideStatus;
    d->GetModelCommandString = getModelCommandString;
    shogo_set_virtual_hook("ServerDE::GetModelAnimUserDims", (void *)&vGetModelAnimUserDims);
}

void installModelAPI(ClientDE *c)
{
    c->GetAnimIndex = getAnimIndex;
    c->SetModelAnimation = [](HLOCALOBJ h, DDWORD a) { setModelAnimation(h, a); };
    c->GetModelAnimation = [](HLOCALOBJ h) -> DDWORD { return getModelAnimation(h); };
    c->ResetModelAnimation = resetModelAnimation;
    c->GetModelPlaybackState = getModelPlaybackState;
    c->GetModelLooping = getModelLooping;
    c->SetModelLooping = setModelLooping;
    c->GetAttachments = getAttachments;
    c->GetModelNodeTransform = getModelNodeTransform;
    c->GetNextModelNode = getNextModelNode;
    c->GetModelNodeName = getModelNodeName;
    c->SetModelNodeHideStatus = setModelNodeHideStatus;
    c->GetModelNodeHideStatus = getModelNodeHideStatus;
    shogo_set_virtual_hook("ClientDE::ProcessAttachments", (void *)&vProcessAttachments);
}

}  // namespace shogo
