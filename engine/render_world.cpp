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

struct ViewVert {
    float x, y, z;     // view space
    float u, v, r, g, b, lu, lv;
};
}  // namespace

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
            Tex tex;
            tex.w = t->width;
            tex.h = t->height;
            tex.px = std::move(t->mips[0]);
            tex.hasAlpha = !t->alpha.empty();
            for (size_t i = 0; i < tex.px.size(); ++i)
                tex.px[i] = (tex.px[i] & 0xFFFFFF) | uint32_t(tex.hasAlpha ? t->alpha[0][i] : 255) << 24;
            m_textures.push_back(std::move(tex));
            id = int(m_textures.size()) - 1;
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
        dp.tex = bsp.textures.empty() ? -1 : textureIndex(bsp.textures[s.texture], fs);
        static const bool noLightmaps = getenv("SHOGO_NOLIGHTMAPS") != nullptr;
        const bool lightmapped = !noLightmaps && (s.flags & kSurfLightmapped) && !(s.flags & kSurfFullbright) &&
                                 poly.lmW > 0 && poly.lmH > 0 && poly.lightmap.size() == size_t(poly.lmW) * poly.lmH;
        if (lightmapped) {
            Lightmap lm;
            lm.w = poly.lmW;
            lm.h = poly.lmH;
            lm.rgb.reserve(poly.lightmap.size() * 3);
            for (uint16_t c : poly.lightmap) {  // RGB565
                lm.rgb.push_back(float((c >> 11) & 31) * (1.0f / 31.0f));
                lm.rgb.push_back(float((c >> 5) & 63) * (1.0f / 63.0f));
                lm.rgb.push_back(float(c & 31) * (1.0f / 31.0f));
            }
            dp.lightmap = int(m_lightmaps.size());
            m_lightmaps.push_back(std::move(lm));
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
    *this = WorldRenderer();
    m_fs = &fs;

    // Sky: SkyPointer objects name world models to draw as the sky box (see the
    // SDK's engineobjects.c); the one with SkyDims also defines the sky box.
    std::vector<std::string> skyNames;
    for (const WorldObject &o : world.objects) {
        if (strcasecmp(o.className.c_str(), "SkyPointer") == 0) {
            if (const WorldProp *p = o.find("SkyObjectName")) skyNames.push_back(p->string);
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
    m_lightmaps.clear();
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
    m_stats.textures = m_textures.size();
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

int WorldRenderer::texture(const std::string &name, int *w, int *h)
{
    int id = m_fs && !name.empty() ? textureIndex(name, *m_fs) : -1;
    if (w) *w = id >= 0 ? m_textures[(size_t)id].w : 1;
    if (h) *h = id >= 0 ? m_textures[(size_t)id].h : 1;
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
    DRect vp = cam.rect;
    if (vp.right <= vp.left || vp.bottom <= vp.top) vp = DRect{0, 0, dst.width, dst.height};
    m_depth.assign(size_t(dst.width) * size_t(dst.height), 0.0f);
    m_stats.drawnPolys = 0;
    m_stats.litPolys = 0;
    m_stats.lightmaps = 0;

    if (m_hasSky && !m_sky.empty()) {
        // The viewer's place in the world maps into the small view box inside the sky.
        auto map = [](float v, float lo, float hi, float vlo, float vhi) {
            float t = hi > lo ? (v - lo) / (hi - lo) : 0.5f;
            return vlo + std::clamp(t, 0.0f, 1.0f) * (vhi - vlo);
        };
        Vec3 eye{map(cam.pos.x, m_worldMin.x, m_worldMax.x, m_skyViewMin.x, m_skyViewMax.x),
                 map(cam.pos.y, m_worldMin.y, m_worldMax.y, m_skyViewMin.y, m_skyViewMax.y),
                 map(cam.pos.z, m_worldMin.z, m_worldMax.z, m_skyViewMin.z, m_skyViewMax.z)};
        drawPolys(dst, cam, eye, m_sky, true);
        std::fill(m_depth.begin(), m_depth.end(), 0.0f);  // the sky is infinitely far away
    }
    drawPolys(dst, cam, cam.pos, m_world, false);

    // World models: where their objects are now, or where they were built.
    std::vector<const ModelPlacement *> placed(m_models.size(), nullptr);
    for (const ModelPlacement &p : m_placements)
        if (p.model > 0 && size_t(p.model) < m_models.size()) placed[size_t(p.model)] = &p;
    m_moved.clear();
    for (size_t i = 1; i < m_models.size(); ++i) {
        const ModelPlacement *p = placed[i];
        if (!p) {
            drawPolys(dst, cam, cam.pos, m_models[i], false);
            continue;
        }
        if (!p->visible) continue;
        const float *m = p->rot;
        auto rotate = [m](const Vec3 &v) {
            return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                        m[6] * v.x + m[7] * v.y + m[8] * v.z};
        };
        const bool still = m[0] == 1 && m[4] == 1 && m[8] == 1 && p->pos.x == p->basePos.x &&
                           p->pos.y == p->basePos.y && p->pos.z == p->basePos.z;
        if (still) {
            drawPolys(dst, cam, cam.pos, m_models[i], false);
            continue;
        }
        for (const DrawPoly &src : m_models[i]) {
            DrawPoly dp = src;
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
    drawPolys(dst, cam, cam.pos, m_moved, false);
    m_stats.dynamicPolys = m_dynamic.size() + m_blended.size() + m_close.size();
    drawPolys(dst, cam, cam.pos, m_dynamic, false);
    drawPolys(dst, cam, cam.pos, m_blended, false);
    if (!m_close.empty()) {
        std::fill(m_depth.begin(), m_depth.end(), 0.0f);
        drawPolys(dst, cam, cam.pos, m_close, false, 0.05f);  // view weapons sit a few units away
    }
    m_dynamic.clear();
    m_blended.clear();
    m_close.clear();

    // Screen tint (damage flashes and the like) over the whole view.
    if (m_screenAdd[0] > 0 || m_screenAdd[1] > 0 || m_screenAdd[2] > 0) {
        const uint32_t add[3] = {uint32_t(std::clamp(m_screenAdd[2], 0.0f, 1.0f) * 255),
                                 uint32_t(std::clamp(m_screenAdd[1], 0.0f, 1.0f) * 255),
                                 uint32_t(std::clamp(m_screenAdd[0], 0.0f, 1.0f) * 255)};
        int x0 = std::max(vp.left, 0), x1 = std::min(vp.right, dst.width);
        int y0 = std::max(vp.top, 0), y1 = std::min(vp.bottom, dst.height);
        for (int y = y0; y < y1; ++y) {
            uint32_t *row = dst.row(y);
            for (int x = x0; x < x1; ++x) {
                uint32_t px = row[x], out = px & 0xFF000000;
                for (int c = 0; c < 3; ++c) out |= std::min(((px >> (c * 8)) & 0xFF) + add[c], 255u) << (c * 8);
                row[x] = out;
            }
        }
    }
}

void WorldRenderer::drawPolys(Surface &dst, const Camera &cam, const Vec3 &eye, const std::vector<DrawPoly> &polys,
                              bool sky, float nearZ)
{
    DRect vp = cam.rect;
    if (vp.right <= vp.left || vp.bottom <= vp.top) vp = DRect{0, 0, dst.width, dst.height};
    vp.left = std::max(vp.left, 0);
    vp.top = std::max(vp.top, 0);
    vp.right = std::min(vp.right, dst.width);
    vp.bottom = std::min(vp.bottom, dst.height);
    const float vw = float(vp.right - vp.left), vh = float(vp.bottom - vp.top);
    if (vw <= 0 || vh <= 0) return;

    // Camera basis (left-handed, +Y up, +Z forward).
    const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    Vec3 right{cy, 0, -sy}, up{sy * sp, cp, cy * sp}, fwd{sy * cp, -sp, cy * cp};
    if (cam.roll != 0) {
        float cr = std::cos(cam.roll), sr = std::sin(cam.roll);
        Vec3 r2{right.x * cr + up.x * sr, right.y * cr + up.y * sr, right.z * cr + up.z * sr};
        Vec3 u2{up.x * cr - right.x * sr, up.y * cr - right.y * sr, up.z * cr - right.z * sr};
        right = r2;
        up = u2;
    }
    if (cam.hasBasis) {
        right = cam.right;
        up = cam.up;
        fwd = cam.forward;
    }
    const float focal = (vw * 0.5f) / std::tan(cam.fovX * 0.5f);
    const float cx = vp.left + vw * 0.5f, cyScreen = vp.top + vh * 0.5f;
    const float tanX = (vw * 0.5f) / focal, tanY = (vh * 0.5f) / focal;

    std::vector<ViewVert> in, clipped;
    struct ScreenVert {
        float sx, sy, iz, uz, vz, rz, gz, bz, luz, lvz;
    };
    std::vector<ScreenVert> sv;

    for (const DrawPoly &poly : polys) {
        if (!sky && (poly.flags & kSurfSky)) continue;  // sky portals show the sky behind

        // Sphere vs. view frustum.
        Vec3 c = sub(poly.sphereCenter, eye);
        float cz = dot(c, fwd), cxv = dot(c, right), cyv = dot(c, up), rad = poly.sphereRadius;
        static const bool noCull = getenv("SHOGO_NOCULL") != nullptr;
        if (cz + rad < nearZ && !noCull) continue;
        if (!noCull && (cxv - rad > cz * tanX + rad * 1.5f || -cxv - rad > cz * tanX + rad * 1.5f)) continue;
        if (!noCull && (cyv - rad > cz * tanY + rad * 1.5f || -cyv - rad > cz * tanY + rad * 1.5f)) continue;

        in.clear();
        for (const DrawVert &v : poly.verts) {
            Vec3 d = sub(v.p, eye);
            in.push_back({dot(d, right), dot(d, up), dot(d, fwd), v.u, v.v, v.r, v.g, v.b, v.lu, v.lv});
        }
        // Clip against the near plane.
        clipped.clear();
        for (size_t i = 0; i < in.size(); ++i) {
            const ViewVert &a = in[i], &b = in[(i + 1) % in.size()];
            bool ain = a.z >= nearZ, bin = b.z >= nearZ;
            if (ain) clipped.push_back(a);
            if (ain != bin) {
                float t = (nearZ - a.z) / (b.z - a.z);
                auto lerp = [t](float x, float y) { return x + (y - x) * t; };
                clipped.push_back({lerp(a.x, b.x), lerp(a.y, b.y), nearZ, lerp(a.u, b.u), lerp(a.v, b.v),
                                   lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.lu, b.lu), lerp(a.lv, b.lv)});
            }
        }
        if (clipped.size() < 3) continue;

        sv.clear();
        for (const ViewVert &v : clipped) {
            float iz = 1.0f / v.z;
            sv.push_back({cx + v.x * focal * iz, cyScreen - v.y * focal * iz, iz, v.u * iz, v.v * iz, v.r * iz,
                          v.g * iz, v.b * iz, v.lu * iz, v.lv * iz});
        }
        ++m_stats.drawnPolys;

        const Tex *tex = poly.tex >= 0 ? &m_textures[size_t(poly.tex)] : nullptr;
        const Lightmap *lm = poly.lightmap >= 0 && !sky ? &m_lightmaps[size_t(poly.lightmap)] : nullptr;
        if (lm) ++m_stats.lightmaps;
        // World translucency (and model alpha) blends; additive adds light.
        Blend blend = poly.blend;
        float alpha = poly.alpha;
        if (blend == Opaque && (poly.flags & kSurfTranslucent)) {
            blend = Alpha;
            alpha = 0.5f;
        }
        const uint32_t a256 = (uint32_t)std::clamp(alpha * 256.0f, 0.0f, 256.0f);

        // Dynamic lights that reach this polygon (at most 8).
        const Light *polyLights[8];
        int nLights = 0;
        if (poly.lit && !sky) {
            for (const Light &L : m_lights) {
                if (nLights == 8) break;
                Vec3 d = sub(L.pos, poly.sphereCenter);
                float reach = L.radius + poly.sphereRadius;
                if (dot(d, d) > reach * reach) continue;
                float side = dot(sub(L.pos, poly.verts[0].p), poly.normal);
                if (std::fabs(side) > L.radius) continue;
                if (L.skipBackfacing && side < 0) continue;
                polyLights[nLights++] = &L;
            }
            if (nLights) ++m_stats.litPolys;
        }
        const bool scaled = poly.lit && !sky && (m_lightScale[0] != 1 || m_lightScale[1] != 1 || m_lightScale[2] != 1);

        // Fan into triangles and rasterize each with edge functions.
        for (size_t k = 1; k + 1 < sv.size(); ++k) {
            const ScreenVert &A = sv[0], &B = sv[k], &C = sv[k + 1];
            float area = (B.sx - A.sx) * (C.sy - A.sy) - (B.sy - A.sy) * (C.sx - A.sx);
            if (std::fabs(area) < 1e-6f) continue;
            // No backface culling: the BSP keeps one-sided polygons, but sky boxes are seen from inside.
            int x0 = std::max(vp.left, (int)std::floor(std::min({A.sx, B.sx, C.sx})));
            int x1 = std::min(vp.right - 1, (int)std::ceil(std::max({A.sx, B.sx, C.sx})));
            int y0 = std::max(vp.top, (int)std::floor(std::min({A.sy, B.sy, C.sy})));
            int y1 = std::min(vp.bottom - 1, (int)std::ceil(std::max({A.sy, B.sy, C.sy})));
            if (x0 > x1 || y0 > y1) continue;
            float inv = 1.0f / area;
            for (int y = y0; y <= y1; ++y) {
                float py = y + 0.5f;
                uint32_t *row = dst.row(y);
                float *depth = &m_depth[size_t(y) * size_t(dst.width)];
                for (int x = x0; x <= x1; ++x) {
                    float px = x + 0.5f;
                    float w0 = ((B.sx - px) * (C.sy - py) - (B.sy - py) * (C.sx - px)) * inv;
                    float w1 = ((C.sx - px) * (A.sy - py) - (C.sy - py) * (A.sx - px)) * inv;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float iz = w0 * A.iz + w1 * B.iz + w2 * C.iz;
                    if (!poly.noZ && iz <= depth[x]) continue;
                    float z = 1.0f / iz;
                    float r = (w0 * A.rz + w1 * B.rz + w2 * C.rz) * z;
                    float g = (w0 * A.gz + w1 * B.gz + w2 * C.gz) * z;
                    float b = (w0 * A.bz + w1 * B.bz + w2 * C.bz) * z;
                    if (lm) {
                        // Bilinear between the four samples around this point.
                        float lu = std::clamp((w0 * A.luz + w1 * B.luz + w2 * C.luz) * z, 0.0f, float(lm->w - 1));
                        float lv = std::clamp((w0 * A.lvz + w1 * B.lvz + w2 * C.lvz) * z, 0.0f, float(lm->h - 1));
                        int i0 = std::min((int)lu, lm->w - 1), j0 = std::min((int)lv, lm->h - 1);
                        int i1 = std::min(i0 + 1, lm->w - 1), j1 = std::min(j0 + 1, lm->h - 1);
                        float fu = lu - float(i0), fv = lv - float(j0);
                        const float *s00 = &lm->rgb[size_t(j0 * lm->w + i0) * 3], *s10 = &lm->rgb[size_t(j0 * lm->w + i1) * 3];
                        const float *s01 = &lm->rgb[size_t(j1 * lm->w + i0) * 3], *s11 = &lm->rgb[size_t(j1 * lm->w + i1) * 3];
                        float c[3];
                        for (int k = 0; k < 3; ++k) {
                            float top = s00[k] + (s10[k] - s00[k]) * fu, bot = s01[k] + (s11[k] - s01[k]) * fu;
                            c[k] = top + (bot - top) * fv;
                        }
                        r = c[0];
                        g = c[1];
                        b = c[2];
                    }
                    if (scaled) {
                        r *= m_lightScale[0];
                        g *= m_lightScale[1];
                        b *= m_lightScale[2];
                    }
                    if (nLights) {
                        // This pixel's place in the world, back from view space.
                        float vx = (px - cx) * z / focal, vy = (cyScreen - py) * z / focal;
                        Vec3 wp{eye.x + right.x * vx + up.x * vy + fwd.x * z, eye.y + right.y * vx + up.y * vy + fwd.y * z,
                                eye.z + right.z * vx + up.z * vy + fwd.z * z};
                        for (int li = 0; li < nLights; ++li) {
                            const Light &L = *polyLights[li];
                            Vec3 d = sub(wp, L.pos);
                            float d2 = dot(d, d);
                            if (d2 >= L.radius * L.radius) continue;
                            float f = 1.0f - std::sqrt(d2) / L.radius;
                            r += L.r * f;
                            g += L.g * f;
                            b += L.b * f;
                        }
                    }
                    uint32_t texel = 0xFFFFFF;
                    float fade = 1.0f;
                    if (poly.round && !(tex && tex->hasAlpha)) {
                        // u, v span 0..texture size across the quad.
                        float tw = tex ? (float)tex->w : 1.0f, th = tex ? (float)tex->h : 1.0f;
                        float du = (w0 * A.uz + w1 * B.uz + w2 * C.uz) * z / tw * 2 - 1;
                        float dv = (w0 * A.vz + w1 * B.vz + w2 * C.vz) * z / th * 2 - 1;
                        fade = 1.0f - (du * du + dv * dv);
                        if (fade <= 0.0f) continue;
                    }
                    if (tex) {
                        float u = (w0 * A.uz + w1 * B.uz + w2 * C.uz) * z;
                        float v = (w0 * A.vz + w1 * B.vz + w2 * C.vz) * z;
                        int tu = (int)std::floor(u), tv = (int)std::floor(v);
                        tu %= tex->w;
                        tv %= tex->h;
                        if (tu < 0) tu += tex->w;
                        if (tv < 0) tv += tex->h;
                        texel = tex->px[size_t(tv) * size_t(tex->w) + size_t(tu)];
                        if (tex->hasAlpha && (poly.colorKey || poly.blend != Opaque)) {
                            // The mask shapes sprites and effects; world surfaces ignore it.
                            uint32_t ta = texel >> 24;
                            if (ta == 0 || (blend == Opaque && ta < 128)) continue;
                            if (blend != Opaque) fade *= float(ta) * (1.0f / 255.0f);
                        } else if (poly.colorKey && (texel & 0xFFFFFF) == 0) {
                            continue;
                        }
                    }
                    auto ch = [](uint32_t t, int shift, float l) {
                        float v = float((t >> shift) & 0xFF) * l;
                        return uint32_t(v > 255.0f ? 255.0f : v);
                    };
                    uint32_t pa256 = a256;
                    if (blend == Alpha) {
                        pa256 = uint32_t(float(a256) * fade);  // alpha blends fade by coverage
                    } else {
                        r *= fade;  // additive (and opaque) fade the color
                        g *= fade;
                        b *= fade;
                    }
                    uint32_t out = ch(texel, 16, r) << 16 | ch(texel, 8, g) << 8 | ch(texel, 0, b);
                    if (blend == Opaque) {
                        depth[x] = iz;
                    } else {
                        uint32_t old = row[x], res = 0;
                        for (int sh = 0; sh <= 16; sh += 8) {
                            uint32_t o = (old >> sh) & 0xFF, n = (out >> sh) & 0xFF;
                            uint32_t c = blend == Additive ? o + ((n * pa256) >> 8) : (o * (256 - pa256) + n * pa256) >> 8;
                            res |= std::min(c, 255u) << sh;
                        }
                        out = res;
                    }
                    row[x] = out;
                }
            }
        }
    }
}

}  // namespace shogo
