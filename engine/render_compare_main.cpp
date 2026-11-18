// shogo-render-compare: draws one synthetic scene that uses every polygon
// feature (vertex light, lightmaps, light scale, dynamic lights, world
// translucency, masks, color keys, alpha and additive blending, soft
// particles, no-depth polygons, a depth clear, the screen tint) with the
// software and the GPU backend, and checks they agree. Then it draws the same
// scene straight into the GPU screen, with every 2D operation (blits in each
// mode, scaling, warps, rotations, masks, fills) on top, and compares that
// with the CPU doing the same, also with the GPU screen at 2.5 times the
// resolution (its 2D read back per screen pixel).
//
//   shogo-render-compare [--out prefix]   writes prefix_{sw,gpu,screen_sw,screen_gpu,screen_gpu_x2.5}.png
//
// Exits 0 when at most 0.5% of 3D pixels differ by more than 8 (of 255) in
// any channel and the GPU screen's 2D is exact, 1 otherwise, and 77 when
// there's no GPU to compare with. Needs no game data.
#include "engine/gpu_screen.h"
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

struct Diff {
    double differ, big;
    uint32_t worst;
};

Diff compare(const Surface &a, const Surface &b)
{
    size_t differ = 0, big = 0;
    uint32_t worst = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        uint32_t pa = a.pixels[i], pb = b.pixels[i], m = 0;
        for (int s = 0; s < 24; s += 8) m = std::max(m, uint32_t(std::abs(int((pa >> s) & 255) - int((pb >> s) & 255))));
        differ += m > 0;
        big += m > 8;
        worst = std::max(worst, m);
    }
    const double n = double(a.pixels.size());
    return {100 * double(differ) / n, 100 * double(big) / n, worst};
}

// A surface of w x h with a pattern, a key-coloured (black) border and corner.
Surface sprite(int w, int h, uint32_t seed)
{
    Surface s(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t v = (uint32_t(x) * 37u + uint32_t(y) * 91u + seed) * 2654435761u;
            bool key = x == 0 || y == 0 || (x < w / 3 && y < h / 3);
            s.pixels[size_t(y * w + x)] = key ? (v & 0x030103u) : (v >> 8 & 0xFFFFFF) | 0x80000000u;  // near-black is key
        }
    return s;
}

// Every 2D screen operation, through the CPU functions or a ScreenTarget.
template <class Ops> void draw2D(Ops &&o)
{
    Surface a = sprite(48, 40, 1), b = sprite(70, 33, 2), m = sprite(9, 7, 3);
    const BlitParams copy{}, key{BlitMode::Transparent, 0, 0}, solid{BlitMode::SolidColor, 0, 0xFF20C0};
    DRect r1{5, 4, 40, 30}, r2{-10, -6, 30, 50};
    o.blit(a, nullptr, 10, 10, copy);
    o.blit(a, &r1, 70, 12, key);
    o.blit(b, &r2, -8, 60, solid);   // source rect off the surface, destination off the screen
    o.blit(b, nullptr, 610, 455, key);  // clipped at the bottom right
    DRect d1{120, 10, 260, 90}, d2{280, 20, 310, 35}, s1{3, 2, 40, 31}, d3{600, 440, 700, 520};
    o.scale(a, &d1, nullptr, copy);
    o.scale(b, &d2, &s1, key);         // shrinking
    o.scale(a, &d3, &r2, solid);       // source partly outside: skipped texels
    DWarpPt q[4] = {{330, 30, 0, 0}, {420, 50, 47, 0}, {400, 140, 47, 39}, {320, 110, 0, 39}};
    o.warp(a, q, 4, copy);
    DWarpPt t[3] = {{440.3f, 20.7f, 2, 3}, {520.9f, 70.2f, 60, 10}, {450.5f, 130.4f, 10, 30}};
    o.warp(b, t, 3, key);
    DFloatPt pivot{560, 200};
    o.transform(a, nullptr, 100, 200, 0.6f, 1.5f, 1.25f, copy);
    o.transform(b, &pivot, 0, 0, -2.1f, 0.8f, 1.9f, key);
    o.transform(a, nullptr, 250, 330, 3.9f, -1.3f, 1.1f, solid);
    o.masked(b, m, nullptr, 330, 300, 0);
    o.masked(a, m, &r1, 630, 470, 0);  // clipped
    DRect f{400, 380, 460, 470};
    o.fill(&f, 0x00336699);
}

struct CpuOps {
    Surface &s;
    void blit(const Surface &src, const DRect *r, int x, int y, const BlitParams &p) { shogo::blit(s, src, r, x, y, p); }
    void scale(const Surface &src, const DRect *d, const DRect *r, const BlitParams &p) { scaleBlit(s, src, d, r, p); }
    void warp(const Surface &src, const DWarpPt *q, int n, const BlitParams &p) { warpBlit(s, src, q, n, p); }
    void transform(const Surface &src, const DFloatPt *o, int x, int y, float a, float sx, float sy, const BlitParams &p)
    {
        transformBlit(s, src, o, x, y, a, sx, sy, p);
    }
    void masked(const Surface &src, const Surface &m, const DRect *r, int x, int y, uint32_t k)
    {
        maskedBlit(s, src, m, r, x, y, k);
    }
    void fill(const DRect *r, uint32_t c) { fillRect(s, r, c); }
};

struct GpuOps {
    ScreenTarget &t;
    Surface &s;
    void blit(const Surface &src, const DRect *r, int x, int y, const BlitParams &p) { t.blit(s, src, r, x, y, p); }
    void scale(const Surface &src, const DRect *d, const DRect *r, const BlitParams &p) { t.scaleBlit(s, src, d, r, p); }
    void warp(const Surface &src, const DWarpPt *q, int n, const BlitParams &p) { t.warpBlit(s, src, q, n, p); }
    void transform(const Surface &src, const DFloatPt *o, int x, int y, float a, float sx, float sy, const BlitParams &p)
    {
        t.transformBlit(s, src, o, x, y, a, sx, sy, p);
    }
    void masked(const Surface &src, const Surface &m, const DRect *r, int x, int y, uint32_t k)
    {
        t.maskedBlit(s, src, m, r, x, y, k);
    }
    void fill(const DRect *r, uint32_t c) { t.fill(s, r, c); }
};

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

    std::unique_ptr<GpuScreen> screen = GpuScreen::create(nullptr);
    std::unique_ptr<RenderBackend> backends[3] = {makeSoftwareBackend(), nullptr, nullptr};
    if (screen) {
        backends[1] = makeGpuBackend(screen->device());
        backends[2] = makeGpuBackend(screen->device(), screen.get());
    }
    if (!backends[1] || !backends[2]) {
        printf("no GPU: skipped\n");
        return 77;
    }
    // 0: software, 1: GPU (read back), 2: software + CPU 2D, 3: GPU screen + GPU 2D,
    // 4: the same with the GPU screen at 2.5 times the resolution
    Surface shots[5] = {Surface(640, 480), Surface(640, 480), Surface(640, 480), Surface(640, 480), Surface(640, 480)};
    for (int b = 0; b < 5; ++b) {
        Surface &dst = shots[b];
        std::fill(dst.pixels.begin(), dst.pixels.end(), 0x203040u);
        if (b >= 3) screen->fill(dst, nullptr, 0x203040u);  // makes it the GPU screen
        RenderBackend &r = *backends[b >= 3 ? 2 : b & 1];
        r.begin(dst, frame);
        r.draw(world, {{0, 0, 0}, false, 1.0f, true});
        r.draw(dynamic, {{0, 0, 0}});
        r.draw(blended, {{0, 0, 0}});
        r.clearDepth();
        r.draw(close, {{0, 0, 0}, false, 0.05f});
        const float tint[3] = {0.05f, 0.0f, 0.02f};
        r.screenAdd(tint);
        r.end();
        if (b == 2) draw2D(CpuOps{dst});
        if (b >= 3) {
            draw2D(GpuOps{*screen, dst});
            screen->toCPU(dst);
            screen->setScale(2.5f);
            screen->present(dst);  // the new scale applies from the next screen on
        }
    }
    if (out) {
        const char *names[5] = {"_sw.png", "_gpu.png", "_screen_sw.png", "_screen_gpu.png", "_screen_gpu_x2.5.png"};
        for (int b = 0; b < 5; ++b) writePNG(shots[b], (std::string(out) + names[b]).c_str());
    }

    Diff d = compare(shots[0], shots[1]);
    printf("3D: %.2f%% of pixels differ, %.2f%% by more than 8, worst %u\n", d.differ, d.big, d.worst);
    bool ok = d.big <= 0.5;
    // On the GPU screen, 2D pixels must be exactly the CPU's, and the rest
    // exactly what the GPU backend drew through a CPU frame.
    size_t drawn2D = 0, wrong2D = 0, wrong3D = 0, wrongScaled = 0;
    for (size_t i = 0; i < shots[0].pixels.size(); ++i) {
        const uint32_t c = 0xFFFFFF;
        if ((shots[2].pixels[i] & c) != (shots[0].pixels[i] & c)) {
            ++drawn2D;
            wrong2D += (shots[3].pixels[i] & c) != (shots[2].pixels[i] & c);
            wrongScaled += (shots[4].pixels[i] & c) != (shots[2].pixels[i] & c);
        } else {
            wrong3D += (shots[3].pixels[i] & c) != (shots[1].pixels[i] & c);
        }
    }
    printf("GPU screen: %zu of %zu 2D pixels differ from the CPU's, %zu other pixels from the GPU backend's\n", wrong2D,
           drawn2D, wrong3D);
    printf("GPU screen at 2.5x (%s): %zu of %zu 2D pixels differ from the CPU's\n",
           screen->scale() == 2.5f ? "read back per screen pixel" : "NOT SCALED", wrongScaled, drawn2D);
    ok = ok && drawn2D > 0 && wrong2D == 0 && wrong3D == 0 && wrongScaled == 0 && screen->scale() == 2.5f;
    return ok ? 0 : 1;
}
