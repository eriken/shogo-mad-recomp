// The software backend: perspective-correct textured polygons with baked
// vertex or lightmap light, dynamic lights per pixel, a 1/z depth buffer and
// alpha/additive blending, all on the CPU into a Surface.
#include "engine/render_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace shogo {

namespace {
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct ViewVert {
    float x, y, z;  // view space
    float u, v, r, g, b, lu, lv;
};

class SoftwareBackend final : public RenderBackend {
public:
    void begin(Surface &dst, const RenderFrame &frame) override
    {
        m_dst = &dst;
        m_frame = frame;
        dst.touch();
        m_depth.assign(size_t(dst.width) * size_t(dst.height), 0.0f);
    }
    void clearDepth() override { std::fill(m_depth.begin(), m_depth.end(), 0.0f); }
    void draw(const std::vector<DrawPoly> &polys, const DrawPass &pass) override;
    void screenAdd(const float rgb[3]) override;
    void end() override { m_dst = nullptr; }

private:
    Surface *m_dst = nullptr;
    RenderFrame m_frame;
    std::vector<float> m_depth;  // 1/z per pixel
    std::vector<ViewVert> m_in, m_clipped;
    std::vector<const DrawPoly *> m_order;
};

void SoftwareBackend::screenAdd(const float rgb[3])
{
    Surface &dst = *m_dst;
    const DRect vp = viewport(m_frame.cam, dst.width, dst.height);
    const uint32_t add[3] = {uint32_t(std::clamp(rgb[2], 0.0f, 1.0f) * 255),
                             uint32_t(std::clamp(rgb[1], 0.0f, 1.0f) * 255),
                             uint32_t(std::clamp(rgb[0], 0.0f, 1.0f) * 255)};
    for (int y = vp.top; y < vp.bottom; ++y) {
        uint32_t *row = dst.row(y);
        for (int x = vp.left; x < vp.right; ++x) {
            uint32_t px = row[x], out = px & 0xFF000000;
            for (int c = 0; c < 3; ++c) out |= std::min(((px >> (c * 8)) & 0xFF) + add[c], 255u) << (c * 8);
            row[x] = out;
        }
    }
}

void SoftwareBackend::draw(const std::vector<DrawPoly> &polys, const DrawPass &pass)
{
    if (!m_dst || polys.empty()) return;
    Surface &dst = *m_dst;
    const Camera &cam = m_frame.cam;
    const RenderResources &resources = *m_frame.res;
    RenderStats &stats = *m_frame.stats;
    const Vec3 &eye = pass.eye;
    const bool sky = pass.sky;
    const float nearZ = pass.nearZ;
    const float *lightScale = m_frame.lightScale;

    const DRect vp = viewport(cam, dst.width, dst.height);
    const float vw = float(vp.right - vp.left), vh = float(vp.bottom - vp.top);
    if (vw <= 0 || vh <= 0) return;

    const ViewBasis basis = viewBasis(cam);
    const Vec3 right = basis.right, up = basis.up, fwd = basis.forward;
    const float focal = (vw * 0.5f) / std::tan(cam.fovX * 0.5f);
    const float cx = vp.left + vw * 0.5f, cyScreen = vp.top + vh * 0.5f;
    const float tanX = (vw * 0.5f) / focal, tanY = (vh * 0.5f) / focal;

    std::vector<ViewVert> &in = m_in, &clipped = m_clipped;
    struct ScreenVert {
        float sx, sy, iz, uz, vz, rz, gz, bz, luz, lvz;
    };
    std::vector<ScreenVert> sv;

    // Opaque polygons first, then the blended ones (world translucency
    // included) in list order: blending doesn't write depth, so an opaque
    // polygon drawn later would cover a translucent one in front of it.
    m_order.clear();
    for (int blended = 0; blended < 2; ++blended)
        for (const DrawPoly &poly : polys)
            if ((poly.blend != Opaque || (poly.flags & kSurfTranslucent)) == (blended != 0)) m_order.push_back(&poly);
    for (const DrawPoly *pp : m_order) {
        const DrawPoly &poly = *pp;
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
        ++stats.drawnPolys;

        const RenderTexture *tex = poly.tex >= 0 ? &resources.textures[size_t(poly.tex)] : nullptr;
        const RenderLightmap *lm = poly.lightmap >= 0 && !sky ? &resources.lightmaps[size_t(poly.lightmap)] : nullptr;
        if (lm) ++stats.lightmaps;
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
        if (poly.lit && !sky && m_frame.lights) {
            for (const Light &L : *m_frame.lights) {
                if (nLights == 8) break;
                Vec3 d = sub(L.pos, poly.sphereCenter);
                float reach = L.radius + poly.sphereRadius;
                if (dot(d, d) > reach * reach) continue;
                float side = dot(sub(L.pos, poly.verts[0].p), poly.normal);
                if (std::fabs(side) > L.radius) continue;
                if (L.skipBackfacing && side < 0) continue;
                polyLights[nLights++] = &L;
            }
            if (nLights) ++stats.litPolys;
        }
        const bool scaled = poly.lit && !sky && (lightScale[0] != 1 || lightScale[1] != 1 || lightScale[2] != 1);

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
                        r *= lightScale[0];
                        g *= lightScale[1];
                        b *= lightScale[2];
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
}  // namespace

std::unique_ptr<RenderBackend> makeSoftwareBackend() { return std::make_unique<SoftwareBackend>(); }

}  // namespace shogo
