// shogo-render-compare: draws one synthetic scene that uses every polygon
// feature (vertex light, lightmaps, light scale, dynamic lights, world
// translucency, masks, color keys, alpha and additive blending, soft
// particles, no-depth polygons, a depth clear, the screen tint) with the
// software and the GPU backend, and checks they agree.
//
//   shogo-render-compare [--out prefix]   writes prefix_sw.png and prefix_gpu.png
//
// Exits 0 when at most 0.5% of pixels differ by more than 8 (of 255) in any
// channel, 1 when more do, and 77 when there's no GPU to compare with. Needs
// no game data.
#include "engine/image_write.h"
#include "engine/render_backend.h"
#include "engine/render_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace shogo;

namespace {

RenderTexture checker(int w, int h, uint32_t a, uint32_t b, bool mask)
{
    RenderTexture t;
    t.w = w;
    t.h = h;
    t.hasAlpha = mask;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t c = ((x / 4 + y / 4) & 1) ? a : b;
            uint32_t alpha = mask ? uint32_t(255 * x / (w - 1)) : 255;  // a ramp through 0 and 128
            t.px.push_back((c & 0xFFFFFF) | alpha << 24);
        }
    return t;
}

// A quad facing the camera (at the origin, looking down +Z) at depth z.
DrawPoly quad(float x0, float y0, float x1, float y1, float z, int tex, float tw, float th)
{
    DrawPoly p;
    p.tex = tex;
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y1, y1, y0, y0}, us[4] = {0, tw, tw, 0}, vs[4] = {0, 0, th, th};
    for (int i = 0; i < 4; ++i) {
        DrawVert v;
        v.p = {xs[i], ys[i], z};
        v.u = us[i];
        v.v = vs[i];
        v.r = 0.6f + 0.1f * float(i);
        v.g = 0.7f;
        v.b = 0.9f - 0.1f * float(i);
        p.verts.push_back(v);
    }
    p.sphereCenter = {(x0 + x1) / 2, (y0 + y1) / 2, z};
    p.sphereRadius = std::hypot(x1 - x0, y1 - y0) / 2;
    p.normal = {0, 0, -1};
    return p;
}

}  // namespace

int main(int argc, char **argv)
{
    const char *out = nullptr;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];

    RenderResources res;
    res.generation = 1;
    res.textures.push_back(checker(32, 32, 0xC08040, 0x4080C0, false));  // 0: opaque
    res.textures.push_back(checker(64, 32, 0xFFE0A0, 0x80FF80, true));   // 1: alpha ramp mask
    res.textures.push_back(checker(16, 16, 0x000000, 0xFF4040, false));  // 2: black squares
    RenderLightmap lm;
    lm.w = 4;
    lm.h = 3;
    for (int i = 0; i < 12; ++i) {
        lm.rgb.push_back(0.2f + 0.06f * float(i));
        lm.rgb.push_back(0.9f - 0.05f * float(i));
        lm.rgb.push_back(0.5f);
    }
    res.lightmaps.push_back(lm);

    std::vector<DrawPoly> world, dynamic, blended, close;
    DrawPoly wall = quad(-400, -300, 400, 300, 400, 0, 256, 192);  // lit: light scale and dynamic lights
    wall.lit = true;
    world.push_back(wall);
    DrawPoly panel = quad(-380, 120, -200, 280, 390, 0, 64, 64);
    panel.lit = true;
    panel.lightmap = 0;
    for (int i = 0; i < 4; ++i) {
        panel.verts[size_t(i)].lu = (i == 1 || i == 2) ? 3.0f : 0.0f;
        panel.verts[size_t(i)].lv = i >= 2 ? 2.0f : 0.0f;
    }
    world.push_back(panel);
    DrawPoly glass = quad(-150, -50, 50, 150, 300, 0, 64, 64);  // world translucency
    glass.flags = kSurfTranslucent;
    world.push_back(glass);
    DrawPoly keyed = quad(100, 100, 300, 250, 300, 1, 64, 32);  // opaque, mask cuts below half
    keyed.colorKey = true;
    dynamic.push_back(keyed);
    DrawPoly black = quad(100, -250, 300, -50, 300, 2, 32, 32);  // black is see-through
    black.colorKey = true;
    dynamic.push_back(black);
    DrawPoly noz = quad(200, -20, 380, 80, 600, 0, 32, 32);  // behind the wall, drawn over it
    noz.noZ = true;
    dynamic.push_back(noz);
    DrawPoly ab = quad(-350, -250, -150, -80, 250, 1, 64, 32);  // alpha blended, masked
    ab.blend = Alpha;
    ab.alpha = 0.7f;
    blended.push_back(ab);
    DrawPoly add = quad(-100, -280, 60, -120, 250, -1, 1, 1);  // additive soft particle
    add.blend = Additive;
    add.round = true;
    for (DrawVert &v : add.verts) v.r = 1, v.g = 0.5f, v.b = 0.2f;
    blended.push_back(add);
    close.push_back(quad(-3, -3, 3, 3, 20, 0, 16, 16));  // after a depth clear

    std::vector<Light> lights = {{{-150, 50, 380}, 220, 0.8f, 0.2f, 0.1f, false},
                                 {{250, -150, 420}, 200, 0.1f, 0.3f, 0.9f, true}};  // behind: skipped
    RenderStats stats;
    RenderFrame frame;
    frame.res = &res;
    frame.lights = &lights;
    frame.lightScale[0] = 1.1f;
    frame.lightScale[1] = 0.9f;
    frame.stats = &stats;

    std::unique_ptr<RenderBackend> backends[2] = {makeSoftwareBackend(), makeGpuBackend()};
    if (!backends[1]) {
        printf("no GPU: skipped\n");
        return 77;
    }
    Surface shots[2] = {Surface(640, 480), Surface(640, 480)};
    for (int b = 0; b < 2; ++b) {
        Surface &dst = shots[b];
        std::fill(dst.pixels.begin(), dst.pixels.end(), 0x203040u);
        RenderBackend &r = *backends[b];
        r.begin(dst, frame);
        r.draw(world, {{0, 0, 0}, false, 1.0f, true});
        r.draw(dynamic, {{0, 0, 0}});
        r.draw(blended, {{0, 0, 0}});
        r.clearDepth();
        r.draw(close, {{0, 0, 0}, false, 0.05f});
        const float tint[3] = {0.05f, 0.0f, 0.02f};
        r.screenAdd(tint);
        r.end();
    }
    if (out) {
        writePNG(shots[0], (std::string(out) + "_sw.png").c_str());
        writePNG(shots[1], (std::string(out) + "_gpu.png").c_str());
    }

    size_t differ = 0, big = 0;
    uint32_t worst = 0;
    for (size_t i = 0; i < shots[0].pixels.size(); ++i) {
        uint32_t a = shots[0].pixels[i], b = shots[1].pixels[i], m = 0;
        for (int s = 0; s < 24; s += 8) m = std::max(m, uint32_t(std::abs(int((a >> s) & 255) - int((b >> s) & 255))));
        differ += m > 0;
        big += m > 8;
        worst = std::max(worst, m);
    }
    const double n = double(shots[0].pixels.size());
    printf("%.2f%% of pixels differ, %.2f%% by more than 8, worst %u\n", 100 * differ / n, 100 * big / n, worst);
    return big <= n * 0.005 ? 0 : 1;
}
