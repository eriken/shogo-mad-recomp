#include "engine/gpu_screen.h"

#include "engine/gpu_present.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace shogo {

namespace {

// One shader for every 2D operation: the vertex stage covers a destination
// rect (in pixels), the fragment stage decides each pixel the way the CPU
// loops in surfaces.cpp do, reading source texels exactly (no filtering).
const char *kShader2D = R"(
#include <metal_stdlib>
using namespace metal;

struct RectU { float4 rect; float4 size; };
struct VOut { float4 pos [[position]]; };

vertex VOut vs_2d(uint vid [[vertex_id]], constant RectU &u [[buffer(0)]])
{
    float2 k = float2((0x16u >> vid) & 1u, (0x34u >> vid) & 1u);
    float2 p = mix(u.rect.xy, u.rect.zw, k);
    VOut o;
    o.pos = float4(p.x / u.size.x * 2.0 - 1.0, 1.0 - p.y / u.size.y * 2.0, 0.0, 1.0);
    return o;
}

struct Op {
    int4 a;      // kind, mode, source w, h
    uint4 b;     // key, fill, mask w, h
    int4 i0;     // blit/masked: dx, dy, source left, top; scale: dest left, top, w, h
    int4 i1;     // scale: source left, top, w, h; warp/transform: point count
    float4 f0;   // warp: a, b, c; transform: pivot x, y, cos, sin
    float4 f1;   // warp: d, e, f; transform: scale x, y, half w, h
    float4 add;  // add: rgb / 255
    float4 poly[5];
};

constant int kFill = 0, kBlit = 1, kScale = 2, kWarp = 3, kTransform = 4, kMasked = 5, kAdd = 6;
constant int kCopy = 0, kTransparent = 1;

uint pack(float4 c)
{
    uint4 v = uint4(round(saturate(c) * 255.0));
    return v.a << 24 | v.r << 16 | v.g << 8 | v.b;
}
float4 unpack(uint p) { return float4((p >> 16) & 255u, (p >> 8) & 255u, p & 255u, p >> 24) / 255.0; }
bool same565(uint a, uint b) { return ((a ^ b) & 0xF8FCF8u) == 0u; }

float2 corner(constant Op &o, int i) { float4 q = o.poly[i / 2]; return (i & 1) ? q.zw : q.xy; }

// rasterConvex's rule: the pixel centre is inside the polygon's span on its row.
bool covered(constant Op &o, float cx, float cy, int x)
{
    float xl = 1e30, xr = -1e30;
    int n = o.i1.x;
    for (int i = 0; i < n; ++i) {
        float2 a = corner(o, i), b = corner(o, (i + 1) % n);
        if ((cy < a.y) == (cy < b.y)) continue;
        float ex = a.x + (cy - a.y) * (b.x - a.x) / (b.y - a.y);
        xl = min(xl, ex);
        xr = max(xr, ex);
    }
    if (xl > xr) return false;
    return x >= int(ceil(xl - 0.5)) && x <= int(floor(xr - 0.5));
}

fragment float4 fs_2d(VOut in [[stage_in]], constant Op &o [[buffer(0)]],
                      texture2d<float> src [[texture(0)]], sampler s0 [[sampler(0)]],
                      texture2d<float> msk [[texture(1)]], sampler s1 [[sampler(1)]])
{
    int x = int(in.pos.x), y = int(in.pos.y);
    int kind = o.a.x, w = o.a.z, h = o.a.w;
    if (kind == kFill) return unpack(o.b.y);
    if (kind == kAdd) return o.add;
    int sx = 0, sy = 0;
    if (kind == kBlit || kind == kMasked) {
        sx = x - o.i0.x + o.i0.z;
        sy = y - o.i0.y + o.i0.w;
    } else if (kind == kScale) {
        sy = o.i1.y + ((y - o.i0.y) * o.i1.w) / o.i0.w;
        sx = o.i1.x + ((x - o.i0.x) * o.i1.z) / o.i0.z;
        if (sy < 0 || sy >= h || sx < 0 || sx >= w) discard_fragment();
    } else {
        float fx = in.pos.x, fy = in.pos.y;
        if (!covered(o, fx, fy, x)) discard_fragment();
        if (kind == kWarp) {
            sx = clamp(int(o.f0.x * fx + o.f0.y * fy + o.f0.z), 0, w - 1);
            sy = clamp(int(o.f1.x * fx + o.f1.y * fy + o.f1.z), 0, h - 1);
        } else {
            float rx = fx - o.f0.x, ry = fy - o.f0.y, cs = o.f0.z, sn = o.f0.w;
            float ux = (rx * cs + ry * sn) / o.f1.x + o.f1.z, uy = (-rx * sn + ry * cs) / o.f1.y + o.f1.w;
            sx = clamp(int(ux), 0, w - 1);
            sy = clamp(int(uy), 0, h - 1);
        }
    }
    uint s = pack(src.read(uint2(sx, sy)));
    if (kind == kMasked) {
        if (same565(s, o.b.x)) discard_fragment();
        return msk.read(uint2(uint(x - o.i0.x) % o.b.z, uint(y - o.i0.y) % o.b.w));
    }
    if (o.a.y == kCopy) return unpack(s);
    if (same565(s, o.b.x)) discard_fragment();
    return o.a.y == kTransparent ? unpack(s) : unpack(o.b.y);
}
)";

enum Kind { kFill, kBlit, kScale, kWarp, kTransform, kMasked, kAdd };

const SDL_GPUTextureFormat kFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;  // XRGB8888 in memory
const uint64_t kEvictAfter = 120;                                          // frames unused

DRect whole(const Surface &s) { return DRect{0, 0, s.width, s.height}; }

DRect clampRect(DRect r, const Surface &s)
{
    r.left = std::max(r.left, 0);
    r.top = std::max(r.top, 0);
    r.right = std::min(r.right, s.width);
    r.bottom = std::min(r.bottom, s.height);
    return r;
}

int modeOf(const BlitParams &p) { return p.mode == BlitMode::Copy ? 0 : p.mode == BlitMode::Transparent ? 1 : 2; }

}  // namespace

struct GpuScreen::Op {
    int32_t a[4] = {};
    uint32_t b[4] = {};
    int32_t i0[4] = {};
    int32_t i1[4] = {};
    float f0[4] = {};
    float f1[4] = {};
    float add[4] = {};
    float poly[20] = {};
};

std::unique_ptr<GpuScreen> GpuScreen::create(GpuPresenter *presenter)
{
    std::unique_ptr<GpuScreen> s(new GpuScreen());
    if (!s->init(presenter)) {
        SDL_Log("GPU screen: %s", SDL_GetError());
        return nullptr;
    }
    return s;
}

bool GpuScreen::init(GpuPresenter *presenter)
{
    m_trace = getenv("SHOGO_TRACE_SCREEN") != nullptr;
    m_presenter = presenter;
    m_device = presenter ? presenter->device() : nullptr;
    if (!m_device) {
        if (presenter) return false;
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
        m_ownsDevice = true;
        m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
        if (!m_device) return false;
    }
    if (!(SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL)) {
        SDL_SetError("the GPU device doesn't take MSL shaders");
        return false;
    }
    SDL_GPUShaderCreateInfo si{};
    si.code = reinterpret_cast<const Uint8 *>(kShader2D);
    si.code_size = strlen(kShader2D);
    si.format = SDL_GPU_SHADERFORMAT_MSL;
    si.entrypoint = "vs_2d";
    si.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    si.num_uniform_buffers = 1;
    SDL_GPUShader *vs = SDL_CreateGPUShader(m_device, &si);
    si.entrypoint = "fs_2d";
    si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    si.num_samplers = 2;
    SDL_GPUShader *fs = SDL_CreateGPUShader(m_device, &si);
    if (vs && fs) {
        SDL_GPUColorTargetDescription ct{};
        ct.format = kFormat;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs;
        pi.fragment_shader = fs;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        m_copy = SDL_CreateGPUGraphicsPipeline(m_device, &pi);
        // Saturating add of the colour, the frame's alpha kept.
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        m_add = SDL_CreateGPUGraphicsPipeline(m_device, &pi);
    }
    if (vs) SDL_ReleaseGPUShader(m_device, vs);
    if (fs) SDL_ReleaseGPUShader(m_device, fs);
    if (!m_copy || !m_add) return false;

    SDL_GPUSamplerCreateInfo smp{};  // unused (texels are read directly), but Metal binds one per texture
    smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_NEAREST;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_sampler = SDL_CreateGPUSampler(m_device, &smp);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    m_dummy = SDL_CreateGPUTexture(m_device, &ti);
    return m_sampler && m_dummy;
}

GpuScreen::~GpuScreen()
{
    if (m_device) {
        endPass();
        if (m_cmd) SDL_SubmitGPUCommandBuffer(m_cmd);
        SDL_WaitForGPUIdle(m_device);
        if (m_lastFrame) SDL_ReleaseGPUFence(m_device, m_lastFrame);
        for (auto &kv : m_cache) SDL_ReleaseGPUTexture(m_device, kv.second.tex);
        if (m_tex) SDL_ReleaseGPUTexture(m_device, m_tex);
        if (m_up) SDL_ReleaseGPUTransferBuffer(m_device, m_up);
        if (m_down) SDL_ReleaseGPUTransferBuffer(m_device, m_down);
        if (m_dummy) SDL_ReleaseGPUTexture(m_device, m_dummy);
        if (m_sampler) SDL_ReleaseGPUSampler(m_device, m_sampler);
        if (m_copy) SDL_ReleaseGPUGraphicsPipeline(m_device, m_copy);
        if (m_add) SDL_ReleaseGPUGraphicsPipeline(m_device, m_add);
        if (m_ownsDevice) SDL_DestroyGPUDevice(m_device);
    }
    if (m_ownsDevice) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

SDL_GPUCommandBuffer *GpuScreen::cmd()
{
    if (!m_cmd) m_cmd = SDL_AcquireGPUCommandBuffer(m_device);
    return m_cmd;
}

void GpuScreen::endPass()
{
    if (!m_pass) return;
    SDL_EndGPURenderPass(m_pass);
    m_pass = nullptr;
    m_bound = nullptr;
}

void GpuScreen::upload(SDL_GPUTexture *tex, const Surface &s)
{
    const Uint32 bytes = Uint32(s.width) * Uint32(s.height) * 4;
    SDL_GPUTransferBuffer *tb = nullptr;
    if (tex == m_tex) {
        tb = m_up;
    } else {
        SDL_GPUTransferBufferCreateInfo bi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
        tb = SDL_CreateGPUTransferBuffer(m_device, &bi);
    }
    if (!tb || !cmd()) return;
    if (void *p = SDL_MapGPUTransferBuffer(m_device, tb, tex == m_tex)) {
        memcpy(p, s.pixels.data(), bytes);
        SDL_UnmapGPUTransferBuffer(m_device, tb);
    }
    endPass();
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(m_cmd);
    SDL_GPUTextureTransferInfo from{tb, 0, Uint32(s.width), Uint32(s.height)};
    SDL_GPUTextureRegion to{};
    to.texture = tex;
    to.w = Uint32(s.width);
    to.h = Uint32(s.height);
    to.d = 1;
    SDL_UploadToGPUTexture(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    if (tb != m_up) SDL_ReleaseGPUTransferBuffer(m_device, tb);  // freed once the GPU is done with it
}

void GpuScreen::sync(Surface &screen)
{
    if (screen.id != m_screenId || screen.width != m_w || screen.height != m_h) {
        // A new screen (a mode change): start from its CPU contents.
        endPass();
        if (m_cmd) {
            SDL_SubmitGPUCommandBuffer(m_cmd);
            m_cmd = nullptr;
        }
        SDL_WaitForGPUIdle(m_device);
        if (m_tex) SDL_ReleaseGPUTexture(m_device, m_tex);
        if (m_up) SDL_ReleaseGPUTransferBuffer(m_device, m_up);
        if (m_down) SDL_ReleaseGPUTransferBuffer(m_device, m_down);
        m_tex = nullptr;
        m_up = m_down = nullptr;
        m_screenId = screen.id;
        m_w = screen.width;
        m_h = screen.height;
        m_state = State::CPU;
        if (m_w <= 0 || m_h <= 0) return;
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = kFormat;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = Uint32(m_w);
        ti.height = Uint32(m_h);
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        m_tex = SDL_CreateGPUTexture(m_device, &ti);
        const Uint32 bytes = Uint32(m_w) * Uint32(m_h) * 4;
        SDL_GPUTransferBufferCreateInfo bi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
        m_up = SDL_CreateGPUTransferBuffer(m_device, &bi);
        bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        m_down = SDL_CreateGPUTransferBuffer(m_device, &bi);
        if (!m_tex || !m_up || !m_down) SDL_Log("GPU screen texture: %s", SDL_GetError());
    }
    if (m_state == State::CPU && m_tex && m_up) {
        upload(m_tex, screen);
        ++m_screenUploads;
        m_state = State::Both;
    }
}

SDL_GPUTexture *GpuScreen::source(const Surface &s)
{
    if (s.width <= 0 || s.height <= 0) return nullptr;
    Cached &c = m_cache[s.id];
    if (!c.tex || c.w != s.width || c.h != s.height) {
        if (c.tex) SDL_ReleaseGPUTexture(m_device, c.tex);
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = kFormat;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = Uint32(s.width);
        ti.height = Uint32(s.height);
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        c.tex = SDL_CreateGPUTexture(m_device, &ti);
        c.w = s.width;
        c.h = s.height;
        c.version = ~s.version;  // force the upload
        if (!c.tex) {
            m_cache.erase(s.id);
            return nullptr;
        }
    }
    if (c.version != s.version) {
        upload(c.tex, s);
        ++m_uploads;
        c.version = s.version;
    }
    c.lastUse = m_frame;
    return c.tex;
}

void GpuScreen::draw(Surface &screen, const DRect &r, const Op &op, const Surface *src, const Surface *mask,
                     bool additive)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    sync(screen);
    if (!m_tex) return;
    SDL_GPUTexture *srcTex = src ? source(*src) : m_dummy;
    SDL_GPUTexture *maskTex = mask ? source(*mask) : m_dummy;
    if (!srcTex || !maskTex || !cmd()) return;
    if (!m_pass) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = m_tex;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        m_pass = SDL_BeginGPURenderPass(m_cmd, &ct, 1, nullptr);
        if (!m_pass) return;
    }
    SDL_GPUGraphicsPipeline *pipe = additive ? m_add : m_copy;
    if (pipe != m_bound) {
        SDL_BindGPUGraphicsPipeline(m_pass, pipe);
        m_bound = pipe;
    }
    const float rect[8] = {float(r.left), float(r.top), float(r.right), float(r.bottom), float(m_w), float(m_h), 0, 0};
    SDL_PushGPUVertexUniformData(m_cmd, 0, rect, sizeof rect);
    SDL_PushGPUFragmentUniformData(m_cmd, 0, &op, sizeof op);
    SDL_GPUTextureSamplerBinding sb[2] = {{srcTex, m_sampler}, {maskTex, m_sampler}};
    SDL_BindGPUFragmentSamplers(m_pass, 0, sb, 2);
    SDL_DrawGPUPrimitives(m_pass, 6, 1, 0, 0);
    ++m_draws;
    m_state = State::GPU;
}

void GpuScreen::fill(Surface &screen, const DRect *rect, uint32_t color)
{
    Op op;
    op.a[0] = kFill;
    op.b[1] = color;
    draw(screen, clampRect(rect ? *rect : whole(screen), screen), op, nullptr, nullptr);
}

void GpuScreen::add(Surface &screen, const DRect &rect, const uint32_t rgb[3])
{
    Op op;
    op.a[0] = kAdd;
    for (int i = 0; i < 3; ++i) op.add[i] = float(rgb[i]) / 255.0f;
    draw(screen, clampRect(rect, screen), op, nullptr, nullptr, true);
}

void GpuScreen::blit(Surface &screen, const Surface &src, const DRect *srcRect, int dx, int dy, const BlitParams &p)
{
    DRect r = clampRect(srcRect ? *srcRect : whole(src), src);
    if (srcRect) {
        dx += r.left - srcRect->left;
        dy += r.top - srcRect->top;
    }
    int w = r.right - r.left, h = r.bottom - r.top;
    if (dx < 0) { r.left -= dx; w += dx; dx = 0; }
    if (dy < 0) { r.top -= dy; h += dy; dy = 0; }
    w = std::min(w, screen.width - dx);
    h = std::min(h, screen.height - dy);
    Op op;
    op.a[0] = kBlit;
    op.a[1] = modeOf(p);
    op.a[2] = src.width;
    op.a[3] = src.height;
    op.b[0] = p.key;
    op.b[1] = p.fill;
    op.i0[0] = dx;
    op.i0[1] = dy;
    op.i0[2] = r.left;
    op.i0[3] = r.top;
    draw(screen, DRect{dx, dy, dx + w, dy + h}, op, &src, nullptr);
}

void GpuScreen::scaleBlit(Surface &screen, const Surface &src, const DRect *dstRect, const DRect *srcRect,
                          const BlitParams &p)
{
    DRect sr = srcRect ? *srcRect : whole(src);
    DRect dr = dstRect ? *dstRect : whole(screen);
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top, dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    Op op;
    op.a[0] = kScale;
    op.a[1] = modeOf(p);
    op.a[2] = src.width;
    op.a[3] = src.height;
    op.b[0] = p.key;
    op.b[1] = p.fill;
    op.i0[0] = dr.left;
    op.i0[1] = dr.top;
    op.i0[2] = dw;
    op.i0[3] = dh;
    op.i1[0] = sr.left;
    op.i1[1] = sr.top;
    op.i1[2] = sw;
    op.i1[3] = sh;
    draw(screen, clampRect(dr, screen), op, &src, nullptr);
}

namespace {
// The pixels rasterConvex visits lie inside this rect.
DRect polyBounds(const float *poly, int n, const Surface &dst)
{
    float minX = poly[0], maxX = poly[0], minY = poly[1], maxY = poly[1];
    for (int i = 1; i < n; ++i) {
        minX = std::min(minX, poly[2 * i]);
        maxX = std::max(maxX, poly[2 * i]);
        minY = std::min(minY, poly[2 * i + 1]);
        maxY = std::max(maxY, poly[2 * i + 1]);
    }
    DRect r{(int)std::ceil(minX - 0.5f), (int)std::ceil(minY - 0.5f), (int)std::floor(maxX - 0.5f) + 1,
            (int)std::floor(maxY - 0.5f) + 1};
    return clampRect(r, dst);
}
}  // namespace

void GpuScreen::warpBlit(Surface &screen, const Surface &src, const DWarpPt *pts, int n, const BlitParams &p)
{
    if (n < 3 || n > 10 || src.width <= 0 || src.height <= 0) return;
    float x0 = pts[0].dest_x, y0 = pts[0].dest_y, x1 = pts[1].dest_x, y1 = pts[1].dest_y, x2 = pts[2].dest_x,
          y2 = pts[2].dest_y;
    float det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(det) < 1e-6f) return;
    auto solve = [&](float s0, float s1, float s2, float *k) {
        k[0] = ((s1 - s0) * (y2 - y0) - (s2 - s0) * (y1 - y0)) / det;
        k[1] = ((s2 - s0) * (x1 - x0) - (s1 - s0) * (x2 - x0)) / det;
        k[2] = s0 - k[0] * x0 - k[1] * y0;
    };
    Op op;
    op.a[0] = kWarp;
    op.a[1] = modeOf(p);
    op.a[2] = src.width;
    op.a[3] = src.height;
    op.b[0] = p.key;
    op.b[1] = p.fill;
    op.i1[0] = n;
    solve(pts[0].source_x, pts[1].source_x, pts[2].source_x, op.f0);
    solve(pts[0].source_y, pts[1].source_y, pts[2].source_y, op.f1);
    for (int i = 0; i < n; ++i) {
        op.poly[2 * i] = pts[i].dest_x;
        op.poly[2 * i + 1] = pts[i].dest_y;
    }
    draw(screen, polyBounds(op.poly, n, screen), op, &src, nullptr);
}

void GpuScreen::transformBlit(Surface &screen, const Surface &src, const DFloatPt *origin, int dx, int dy, float angle,
                              float sx, float sy, const BlitParams &p)
{
    if (sx == 0 || sy == 0 || src.width == 0 || src.height == 0) return;
    float px = origin ? origin->x : (float)dx, py = origin ? origin->y : (float)dy;
    float cs = std::cos(angle), sn = std::sin(angle);
    float hw = src.width * 0.5f, hh = src.height * 0.5f;
    Op op;
    op.a[0] = kTransform;
    op.a[1] = modeOf(p);
    op.a[2] = src.width;
    op.a[3] = src.height;
    op.b[0] = p.key;
    op.b[1] = p.fill;
    op.i1[0] = 4;
    const float cx[4] = {-hw, hw, hw, -hw}, cyy[4] = {-hh, -hh, hh, hh};
    for (int i = 0; i < 4; ++i) {
        float x = cx[i] * sx, y = cyy[i] * sy;
        op.poly[2 * i] = px + x * cs - y * sn;
        op.poly[2 * i + 1] = py + x * sn + y * cs;
    }
    const float f0[4] = {px, py, cs, sn}, f1[4] = {sx, sy, hw, hh};
    std::copy(f0, f0 + 4, op.f0);
    std::copy(f1, f1 + 4, op.f1);
    draw(screen, polyBounds(op.poly, 4, screen), op, &src, nullptr);
}

void GpuScreen::maskedBlit(Surface &screen, const Surface &src, const Surface &mask, const DRect *srcRect, int dx,
                           int dy, uint32_t key)
{
    if (mask.width == 0 || mask.height == 0) return;
    DRect r = clampRect(srcRect ? *srcRect : whole(src), src);
    Op op;
    op.a[0] = kMasked;
    op.a[2] = src.width;
    op.a[3] = src.height;
    op.b[0] = key;
    op.b[2] = uint32_t(mask.width);
    op.b[3] = uint32_t(mask.height);
    op.i0[0] = dx;
    op.i0[1] = dy;
    op.i0[2] = r.left;
    op.i0[3] = r.top;
    DRect d{dx, dy, dx + (r.right - r.left), dy + (r.bottom - r.top)};
    draw(screen, clampRect(d, screen), op, &src, &mask);
}

SDL_GPUCommandBuffer *GpuScreen::beginExternal(Surface &screen, SDL_GPUTexture **texture)
{
    sync(screen);
    endPass();
    *texture = m_tex;
    if (!m_tex) return nullptr;
    m_state = State::GPU;
    return cmd();
}

void GpuScreen::toCPU(Surface &screen)
{
    sync(screen);
    if (m_state != State::GPU || !m_tex || !m_down || !cmd()) return;
    endPass();
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(m_cmd);
    SDL_GPUTextureRegion from{};
    from.texture = m_tex;
    from.w = Uint32(m_w);
    from.h = Uint32(m_h);
    from.d = 1;
    SDL_GPUTextureTransferInfo to{m_down, 0, Uint32(m_w), Uint32(m_h)};
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd);
    m_cmd = nullptr;
    if (fence) {
        SDL_WaitForGPUFences(m_device, true, &fence, 1);
        SDL_ReleaseGPUFence(m_device, fence);
    }
    if (const void *p = SDL_MapGPUTransferBuffer(m_device, m_down, false)) {
        memcpy(screen.pixels.data(), p, size_t(m_w) * size_t(m_h) * 4);
        SDL_UnmapGPUTransferBuffer(m_device, m_down);
    }
    screen.touch();
    m_state = State::Both;
    ++m_readbacks;
}

void GpuScreen::cpuWrites(Surface &screen)
{
    toCPU(screen);
    m_state = State::CPU;
}

void GpuScreen::present(Surface &screen)
{
    sync(screen);
    endPass();
    if (!cmd()) return;
    if (m_presenter && m_tex) m_presenter->showTexture(m_cmd, m_tex, m_w, m_h);
    // At most one frame in flight: wait for the previous one before queueing.
    if (m_lastFrame) {
        SDL_WaitForGPUFences(m_device, true, &m_lastFrame, 1);
        SDL_ReleaseGPUFence(m_device, m_lastFrame);
    }
    m_lastFrame = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd);
    m_cmd = nullptr;

    if (m_trace)
        fprintf(stderr, "[screen] frame %llu: %d 2D draws, %d source uploads, %d read-backs, %d screen uploads\n",
                (unsigned long long)m_frame, m_draws, m_uploads, m_readbacks, m_screenUploads);
    m_draws = m_uploads = m_readbacks = m_screenUploads = 0;
    ++m_frame;
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (m_frame - it->second.lastUse > kEvictAfter) {
            SDL_ReleaseGPUTexture(m_device, it->second.tex);
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace shogo
