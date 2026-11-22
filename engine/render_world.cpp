#include "engine/render_world.h"

#include "engine/dtx.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace shogo {

namespace {
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

bool endsWith(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

// Sprite files (.SPR) list DTX frames; for now a sprite texture shows its first frame.
std::string firstSpriteFrame(const std::vector<uint8_t> &d)
{
    if (d.size() < 22) return {};
    uint32_t frames;
    memcpy(&frames, d.data(), 4);
    uint16_t len;
    memcpy(&len, d.data() + 20, 2);
    if (frames == 0 || 22u + len > d.size()) return {};
    return std::string((const char *)d.data() + 22, len);
}

uint64_t nextGeneration()
{
    static uint64_t g = 0;
    return ++g;
}
}  // namespace

ViewBasis viewBasis(const Camera &cam)
{
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    ViewBasis b{{cy, 0, -sy}, {sy * sp, cp, cy * sp}, {sy * cp, -sp, cy * cp}};
    if (cam.roll != 0) {
        float cr = std::cos(cam.roll), sr = std::sin(cam.roll);
        Vec3 r2{b.right.x * cr + b.up.x * sr, b.right.y * cr + b.up.y * sr, b.right.z * cr + b.up.z * sr};
        Vec3 u2{b.up.x * cr - b.right.x * sr, b.up.y * cr - b.right.y * sr, b.up.z * cr - b.right.z * sr};
        b.right = r2;
        b.up = u2;
    }
    if (cam.hasBasis) b = {cam.right, cam.up, cam.forward};
    return b;
}

DRect viewport(const Camera &cam, int width, int height)
{
    DRect vp = cam.rect;
    if (vp.right <= vp.left || vp.bottom <= vp.top) vp = DRect{0, 0, width, height};
    vp.left = std::max(vp.left, 0);
    vp.top = std::max(vp.top, 0);
    vp.right = std::min(vp.right, width);
    vp.bottom = std::min(vp.bottom, height);
    return vp;
}

WorldRenderer::WorldRenderer() : m_backend(makeSoftwareBackend()) {}
WorldRenderer::~WorldRenderer() = default;

void WorldRenderer::setBackend(std::unique_ptr<RenderBackend> backend)
{
    m_backend = backend ? std::move(backend) : makeSoftwareBackend();
}

RenderFrame WorldRenderer::frame(const Camera &cam)
{
    RenderFrame f;
    f.cam = cam;
    f.res = &m_res;
    f.lights = &m_lights;
    for (int i = 0; i < 3; ++i) f.lightScale[i] = m_lightScale[i];
    f.panTex = m_panTex >= 0 && size_t(m_panTex) < m_res.textures.size() ? m_panTex : -1;
    for (int i = 0; i < 4; ++i) f.pan[i] = m_pan[i];
    f.stats = &m_stats;
    return f;
}

void WorldRenderer::clearDynamic()
{
    recycle(m_dynamic);
    recycle(m_blended);
    recycle(m_close);
}

void WorldRenderer::recycle(std::vector<DrawPoly> &list)
{
    // Not every polygon came from takeVerts (effects build their own lists),
    // so keeping them all would grow the pool every frame without end: an
    // hour in a level with water grids held tens of gigabytes. Keep enough
    // for a busy frame and let the rest go.
    constexpr size_t kMaxSpare = 16384;
    for (DrawPoly &p : list) {
        if (m_spareVerts.size() >= kMaxSpare) break;
        p.verts.clear();
        m_spareVerts.push_back(std::move(p.verts));
    }
    list.clear();
}

std::vector<WorldRenderer::DrawVert> WorldRenderer::takeVerts()
{
    if (m_spareVerts.empty()) return {};
    std::vector<DrawVert> v = std::move(m_spareVerts.back());
    m_spareVerts.pop_back();
    return v;
}

int WorldRenderer::textureIndex(const std::string &name, const ResourceFS &fs)
{
    std::string key = RezFile::key(name);
    auto it = m_textureIds.find(key);
    if (it != m_textureIds.end()) return it->second;

    int id = -1;
    std::string path = name;
    auto data = fs.read(path);
    if (data && endsWith(path, ".spr")) {
        path = firstSpriteFrame(*data);
        data = path.empty() ? std::nullopt : fs.read(path);
    }
    if (data) {
        if (auto t = decodeDTX(*data)) {
            RenderTexture tex;
            tex.w = t->width;
            tex.h = t->height;
            tex.px = std::move(t->mips[0]);
            tex.hasAlpha = !t->alpha.empty();
            for (size_t i = 0; i < tex.px.size(); ++i)
                tex.px[i] = (tex.px[i] & 0xFFFFFF) | uint32_t(tex.hasAlpha ? t->alpha[0][i] : 255) << 24;
            m_res.textures.push_back(std::move(tex));
            id = int(m_res.textures.size()) - 1;
        }
    }
    if (id < 0) ++m_stats.missingTextures;
    m_textureIds[key] = id;
    return id;
}

void WorldRenderer::addModel(const WorldBsp &bsp, std::vector<DrawPoly> &out, const ResourceFS &fs)
{
    for (const WorldPoly &poly : bsp.polys) {
        const WorldSurface &s = bsp.surfaces[poly.surface];
        static const bool showAll = getenv("SHOGO_SHOWALL") != nullptr;
        if ((s.flags & kSurfInvisible) && !showAll) continue;
        if (poly.verts.size() < 3) continue;
        DrawPoly dp;
        dp.flags = s.flags;
        // Translucent surfaces are drawn at half alpha with black see-through:
        // the flag covers both glass (no black texels) and cut-outs such as
        // barbed wire, girders, signs and the sky's moon (mostly black), whose
        // black would otherwise darken what's behind into a frame.
        static const bool noKey = getenv("SHOGO_NOTRANSLUCENTKEY") != nullptr;
        dp.colorKey = (s.flags & kSurfTranslucent) && !noKey;
        dp.tex = bsp.textures.empty() ? -1 : textureIndex(bsp.textures[s.texture], fs);
        static const bool noLightmaps = getenv("SHOGO_NOLIGHTMAPS") != nullptr;
        const bool lightmapped = !noLightmaps && (s.flags & kSurfLightmapped) && !(s.flags & kSurfFullbright) &&
                                 poly.lmW > 0 && poly.lmH > 0 && poly.lightmap.size() == size_t(poly.lmW) * poly.lmH;
        if (lightmapped) {
            RenderLightmap lm;
            lm.w = poly.lmW;
            lm.h = poly.lmH;
            lm.rgb.reserve(poly.lightmap.size() * 3);
            for (uint16_t c : poly.lightmap) {  // RGB565
                lm.rgb.push_back(float((c >> 11) & 31) * (1.0f / 31.0f));
                lm.rgb.push_back(float((c >> 5) & 63) * (1.0f / 63.0f));
                lm.rgb.push_back(float(c & 31) * (1.0f / 31.0f));
            }
            dp.lightmap = int(m_res.lightmaps.size());
            m_res.lightmaps.push_back(std::move(lm));
        }
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const WorldPolyVertex &v : poly.verts) {
            DrawVert dv;
            dv.p = bsp.points[v.point];
            Vec3 rel = sub(dv.p, s.O);
            dv.u = dot(rel, s.P);
            dv.v = dot(rel, s.Q);
            static const bool debugBright = getenv("SHOGO_FULLBRIGHT") != nullptr;
            bool bright = debugBright || (s.flags & kSurfFullbright) != 0;
            dv.r = bright ? 1.0f : v.r / 255.0f;
            dv.g = bright ? 1.0f : v.g / 255.0f;
            dv.b = bright ? 1.0f : v.b / 255.0f;
            if (lightmapped) {
                Vec3 lrel = sub(dv.p, poly.lmOrigin);
                dv.lu = dot(lrel, s.lmP) * (1.0f / kLightmapSpacing);
                dv.lv = dot(lrel, s.lmQ) * (1.0f / kLightmapSpacing);
            }
            dp.verts.push_back(dv);
            lo = {std::min(lo.x, dv.p.x), std::min(lo.y, dv.p.y), std::min(lo.z, dv.p.z)};
            hi = {std::max(hi.x, dv.p.x), std::max(hi.y, dv.p.y), std::max(hi.z, dv.p.z)};
        }
        static const bool debugBright2 = getenv("SHOGO_FULLBRIGHT") != nullptr;
        dp.lit = !debugBright2 && !(s.flags & (kSurfFullbright | kSurfSky));
        if (dp.verts.size() >= 3) {
            // World polygons wind so that this cross product points out of the front.
            Vec3 a = sub(dp.verts[1].p, dp.verts[0].p), b = sub(dp.verts[2].p, dp.verts[0].p);
            Vec3 n{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
            float len = std::sqrt(dot(n, n));
            if (len > 0) dp.normal = {n.x / len, n.y / len, n.z / len};
        }
        dp.sphereCenter = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        Vec3 ext = sub(hi, dp.sphereCenter);
        dp.sphereRadius = std::sqrt(dot(ext, ext));
        out.push_back(std::move(dp));
    }
}

void WorldRenderer::load(const WorldData &world, const ResourceFS &fs)
{
    // Everything but the backend starts over.
    m_res = RenderResources();
    m_res.generation = nextGeneration();
    m_textureIds.clear();
    m_world.clear();
    m_sky.clear();
    clearDynamic();
    m_hasSky = false;
    m_worldMin = m_worldMax = m_skyViewMin = m_skyViewMax = Vec3{};
    m_starts.clear();
    m_lights.clear();
    m_models.clear();
    m_placements.clear();
    m_moved.clear();
    m_lightScale[0] = m_lightScale[1] = m_lightScale[2] = 1;
    m_screenAdd[0] = m_screenAdd[1] = m_screenAdd[2] = 0;
    m_stats = Stats();
    m_fs = &fs;

    // Sky: SkyPointer objects name world models to draw as the sky box, and a
    // DemoSkyWorldModel is one itself, by its own name (see the SDK's
    // engineobjects.c); the one with SkyDims also defines the sky box.
    std::vector<std::string> skyNames;
    for (const WorldObject &o : world.objects) {
        const bool pointer = strcasecmp(o.className.c_str(), "SkyPointer") == 0;
        if (pointer || strcasecmp(o.className.c_str(), "DemoSkyWorldModel") == 0) {
            if (const WorldProp *p = o.find(pointer ? "SkyObjectName" : "Name")) skyNames.push_back(p->string);
            const WorldProp *dims = o.find("SkyDims");
            const WorldProp *pos = o.find("Pos");
            if (dims && pos && dims->vec[0] != 0 && dims->vec[1] != 0 && dims->vec[2] != 0) {
                auto pct = [&](const char *n) {
                    const WorldProp *p = o.find(n);
                    return p ? p->vec[0] : 0.1f;
                };
                Vec3 c{pos->vec[0], pos->vec[1], pos->vec[2]};
                Vec3 inner{dims->vec[0] * pct("InnerPercentX"), dims->vec[1] * pct("InnerPercentY"),
                           dims->vec[2] * pct("InnerPercentZ")};
                m_skyViewMin = sub(c, inner);
                m_skyViewMax = {c.x + inner.x, c.y + inner.y, c.z + inner.z};
                m_hasSky = true;
            }
        } else if (strcasecmp(o.className.c_str(), "GameStartPoint") == 0) {
            m_starts.push_back(o);
        }
    }

    m_models.assign(world.models.size(), {});
    for (size_t i = 0; i < world.models.size(); ++i) {
        const WorldBsp &bsp = world.models[i];
        bool isSky = false;
        for (const auto &n : skyNames)
            if (strcasecmp(n.c_str(), bsp.name.c_str()) == 0) isSky = true;
        addModel(bsp, isSky ? m_sky : i == 0 ? m_world : m_models[i], fs);
    }
    m_worldMin = world.models[0].boxMin;
    m_worldMax = world.models[0].boxMax;
    m_stats.polys = m_world.size();
    m_stats.skyPolys = m_sky.size();
    m_stats.textures = m_res.textures.size();
}

void WorldRenderer::renderDynamic(Surface &dst, const Camera &cam)
{
    m_backend->begin(dst, frame(cam));
    m_backend->draw(m_dynamic, {cam.pos});
    m_backend->draw(m_blended, {cam.pos});
    m_backend->draw(m_close, {cam.pos, false, 0.05f});
    m_backend->end();
    clearDynamic();
}

void WorldRenderer::addDynamic(DrawPoly poly, bool reallyClose)
{
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const DrawVert &v : poly.verts) {
        lo = {std::min(lo.x, v.p.x), std::min(lo.y, v.p.y), std::min(lo.z, v.p.z)};
        hi = {std::max(hi.x, v.p.x), std::max(hi.y, v.p.y), std::max(hi.z, v.p.z)};
    }
    poly.sphereCenter = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    Vec3 ext = sub(hi, poly.sphereCenter);
    poly.sphereRadius = std::sqrt(dot(ext, ext));
    // Blended polygons go after everything solid (they don't write depth).
    (reallyClose ? m_close : poly.blend != Opaque ? m_blended : m_dynamic).push_back(std::move(poly));
}

int WorldRenderer::texture(const std::string &name, int *w, int *h, bool *hasAlpha)
{
    int id = m_fs && !name.empty() ? textureIndex(name, *m_fs) : -1;
    if (w) *w = id >= 0 ? m_res.textures[(size_t)id].w : 1;
    if (h) *h = id >= 0 ? m_res.textures[(size_t)id].h : 1;
    if (hasAlpha) *hasAlpha = id >= 0 && m_res.textures[(size_t)id].hasAlpha;
    return id;
}

bool WorldRenderer::startPoint(Camera &cam) const
{
    if (m_starts.empty()) return false;
    const WorldObject &o = m_starts.front();
    if (const WorldProp *p = o.find("Pos")) cam.pos = {p->vec[0], p->vec[1], p->vec[2]};
    if (const WorldProp *r = o.find("Rotation")) {
        cam.pitch = r->vec[0];
        cam.yaw = r->vec[1];
        cam.roll = r->vec[2];
    }
    return true;
}

void WorldRenderer::render(Surface &dst, const Camera &cam)
{
    m_stats.drawnPolys = 0;
    m_stats.litPolys = 0;
    m_stats.lightmaps = 0;
    m_backend->begin(dst, frame(cam));

    if (m_hasSky && !m_sky.empty()) {
        // The viewer's place in the world maps into the small view box inside the sky.
        auto map = [](float v, float lo, float hi, float vlo, float vhi) {
            float t = hi > lo ? (v - lo) / (hi - lo) : 0.5f;
            return vlo + std::clamp(t, 0.0f, 1.0f) * (vhi - vlo);
        };
        Vec3 eye{map(cam.pos.x, m_worldMin.x, m_worldMax.x, m_skyViewMin.x, m_skyViewMax.x),
                 map(cam.pos.y, m_worldMin.y, m_worldMax.y, m_skyViewMin.y, m_skyViewMax.y),
                 map(cam.pos.z, m_worldMin.z, m_worldMax.z, m_skyViewMin.z, m_skyViewMax.z)};
        m_backend->draw(m_sky, {eye, true, 1.0f, true});
        m_backend->clearDepth();  // the sky is infinitely far away
    }
    const DrawPass still{cam.pos, false, 1.0f, true}, moving{cam.pos};
    m_backend->draw(m_world, still);

    // World models: where their objects are now, or where they were built.
    std::vector<const ModelPlacement *> placed(m_models.size(), nullptr);
    for (const ModelPlacement &p : m_placements)
        if (p.model > 0 && size_t(p.model) < m_models.size()) placed[size_t(p.model)] = &p;
    recycle(m_moved);
    for (size_t i = 1; i < m_models.size(); ++i) {
        const ModelPlacement *p = placed[i];
        if (!p) {
            m_backend->draw(m_models[i], still);
            continue;
        }
        if (!p->visible) continue;
        const float *m = p->rot;
        auto rotate = [m](const Vec3 &v) {
            return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                        m[6] * v.x + m[7] * v.y + m[8] * v.z};
        };
        const bool unmoved = m[0] == 1 && m[4] == 1 && m[8] == 1 && p->pos.x == p->basePos.x &&
                             p->pos.y == p->basePos.y && p->pos.z == p->basePos.z;
        if (unmoved) {
            m_backend->draw(m_models[i], still);
            continue;
        }
        for (const DrawPoly &src : m_models[i]) {
            DrawPoly dp;
            dp.verts = takeVerts();
            dp = src;  // into the reused list's room
            for (DrawVert &v : dp.verts) {
                Vec3 r = rotate(sub(v.p, p->basePos));
                v.p = {r.x + p->pos.x, r.y + p->pos.y, r.z + p->pos.z};
            }
            Vec3 c = rotate(sub(src.sphereCenter, p->basePos));
            dp.sphereCenter = {c.x + p->pos.x, c.y + p->pos.y, c.z + p->pos.z};
            dp.normal = rotate(src.normal);
            m_moved.push_back(std::move(dp));
        }
    }
    m_backend->draw(m_moved, moving);
    m_stats.dynamicPolys = m_dynamic.size() + m_blended.size() + m_close.size();
    m_backend->draw(m_dynamic, moving);
    m_backend->draw(m_blended, moving);
    if (!m_close.empty()) {
        m_backend->clearDepth();
        m_backend->draw(m_close, {cam.pos, false, 0.05f});  // view weapons sit a few units away
    }
    clearDynamic();

    // Screen tint (damage flashes and the like) over the whole view.
    if (m_screenAdd[0] > 0 || m_screenAdd[1] > 0 || m_screenAdd[2] > 0) m_backend->screenAdd(m_screenAdd);
    m_backend->end();
}

}  // namespace shogo
