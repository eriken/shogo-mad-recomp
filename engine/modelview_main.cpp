// shogo-modelview: renders one model (.ABC) with its skin, posed at a time in
// an animation, to a PNG. For checking the model code without running the game.
//
// Usage: shogo-modelview MODEL.ABC [--skin SKIN.DTX] [--anim NAME] [--time MS]
//                        [--yaw DEGREES] [--list] --out OUT.PNG
//        shogo-modelview MODEL.ABC --scan
//   --scan: poses every animation every 10 ms, and every animation blended
//   into each one, and reports poses with a vertex far outside the model
//   (a stretched triangle).
// Run from the game folder (reads SHOGO.REZ and SHOGOP.REZ if present).
#include "engine/collision.h"
#include "engine/image_write.h"
#include "engine/math3d.h"
#include "engine/model.h"
#include "engine/render_world.h"
#include "engine/rez_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace shogo;

int main(int argc, char **argv)
{
    std::string model, skin, anim, out;
    float timeMs = 0, yawDeg = 0;
    bool list = false, scan = false;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (!strcmp(argv[i], "--skin")) skin = next();
        else if (!strcmp(argv[i], "--anim")) anim = next();
        else if (!strcmp(argv[i], "--time")) timeMs = (float)atof(next());
        else if (!strcmp(argv[i], "--yaw")) yawDeg = (float)atof(next());
        else if (!strcmp(argv[i], "--out")) out = next();
        else if (!strcmp(argv[i], "--list")) list = true;
        else if (!strcmp(argv[i], "--scan")) scan = true;
        else model = argv[i];
    }
    if (model.empty() || (out.empty() && !list && !scan)) {
        fprintf(stderr, "usage: %s MODEL.ABC [--skin SKIN.DTX] [--anim NAME] [--time MS] [--yaw DEG] [--list] --out OUT.PNG\n",
                argv[0]);
        return 2;
    }
    ResourceFS fs;
    std::string err;
    if (!fs.addArchive("SHOGO.REZ", &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    fs.addArchive("SHOGOP.REZ");
    auto data = fs.read(model);
    if (!data) {
        fprintf(stderr, "no such model: %s\n", model.c_str());
        return 1;
    }
    auto m = parseModel(*data, &err);
    if (!m) {
        fprintf(stderr, "%s: %s\n", model.c_str(), err.c_str());
        return 1;
    }
    if (list) {
        printf("%zu nodes, %zu vertices, %zu triangles, command string \"%s\"\n", m->nodes.size(), m->verts.size(),
               m->tris.size(), m->commandString.c_str());
        for (const ModelAnim &a : m->anims)
            printf("  %-16s %5u ms, %zu keys, dims %.0f %.0f %.0f\n", a.name.c_str(), a.length, a.keys.size(), a.dims.x,
                   a.dims.y, a.dims.z);
        if (out.empty() && !scan) return 0;
    }
    if (scan) {
        // How far a vertex reaches from the model's origin, posed.
        auto reach = [&](int ai, float t, const PoseBlend *from) {
            std::vector<Vec3> pos;
            poseModel(*m, ai, t, nullptr, &pos, nullptr, from);
            float far = 0;
            for (const Vec3 &p : pos) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return 1e30f;
                far = std::max(far, length(p));
            }
            return far;
        };
        float base = reach(0, 0, nullptr), limit = std::max(base * 3.0f, 100.0f);
        printf("rest pose reaches %.0f; flagging over %.0f\n", base, limit);
        int bad = 0;
        for (size_t ai = 0; ai < m->anims.size(); ++ai) {
            const ModelAnim &a = m->anims[ai];
            for (float t = 0; t <= (float)a.length; t += 10) {
                float r = reach((int)ai, t, nullptr);
                if (r > limit && bad++ < 50) printf("  %s at %.0f ms reaches %.0f\n", a.name.c_str(), t, r);
                for (size_t fi = 0; fi < m->anims.size(); ++fi) {
                    if (fi == ai) continue;
                    for (float ft : {0.0f, (float)m->anims[fi].length * 0.5f, (float)m->anims[fi].length}) {
                        PoseBlend from{(int)fi, ft, 0.5f};
                        float rb = reach((int)ai, t, &from);
                        if (rb > limit && bad++ < 50)
                            printf("  %s at %.0f ms, half blended from %s at %.0f ms, reaches %.0f\n", a.name.c_str(), t,
                                   m->anims[fi].name.c_str(), ft, rb);
                    }
                }
            }
        }
        printf("%d bad poses\n", bad);
        return bad ? 1 : 0;
    }
    int ai = anim.empty() ? 0 : m->find(anim.c_str());
    if (ai < 0) {
        fprintf(stderr, "no animation %s\n", anim.c_str());
        return 1;
    }

    std::vector<Vec3> pos, nrm;
    poseModel(*m, ai, timeMs, nullptr, &pos, &nrm);
    DRotation rot = quatEuler(0, yawDeg * 3.14159265f / 180.0f, 0);
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < pos.size(); ++i) {
        DVector p = quatRotate(rot, DVector(pos[i].x, pos[i].y, pos[i].z));
        DVector n = quatRotate(rot, DVector(nrm[i].x, nrm[i].y, nrm[i].z));
        pos[i] = {p.x, p.y, p.z};
        nrm[i] = {n.x, n.y, n.z};
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }

    WorldRenderer r;
    r.setResources(fs);
    int tw = 1, th = 1;
    int tex = skin.empty() ? -1 : r.texture(skin, &tw, &th);
    if (!skin.empty() && tex < 0) fprintf(stderr, "skin not found: %s\n", skin.c_str());
    const Vec3 light = normalized(Vec3{0.3f, 1.0f, -0.4f});
    for (const ModelTri &t : m->tris) {
        WorldRenderer::DrawPoly dp;
        dp.tex = tex;
        for (int k = 0; k < 3; ++k) {
            WorldRenderer::DrawVert v;
            v.p = pos[t.v[k]];
            v.u = t.uv[k * 2] * (float)tw;
            v.v = t.uv[k * 2 + 1] * (float)th;
            float l = 0.45f + 0.55f * std::max(0.0f, dot(nrm[t.v[k]], light));
            v.r = v.g = v.b = l;
            dp.verts.push_back(v);
        }
        r.addDynamic(std::move(dp), false);
    }

    // Models face +z: the camera stands in front of the model, looking back at it.
    Surface s(480, 480);
    std::fill(s.pixels.begin(), s.pixels.end(), 0x303844);
    Vec3 c{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
    float size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1.0f});
    Camera cam;
    cam.fovX = 0.8f;
    cam.pos = {c.x, c.y, c.z + size * 1.8f};
    cam.yaw = 3.14159265f;
    r.render(s, cam);
    if (!writePNG(s, out)) {
        fprintf(stderr, "can't write %s\n", out.c_str());
        return 1;
    }
    printf("%s: %s at %.0f ms, %zu triangles -> %s\n", model.c_str(), m->anims[(size_t)ai].name.c_str(), timeMs,
           m->tris.size(), out.c_str());
    return 0;
}
