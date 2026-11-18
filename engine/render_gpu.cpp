#include "engine/render_gpu.h"

#include "engine/gpu_screen.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <tuple>

namespace shogo {

namespace {

// View space is x = right, y = up, z = forward from the pass's eye. Clip space
// keeps the software renderer's projection; depth is nearZ / z, so nearer is
// larger, cleared to 0 and tested GREATER like the software 1/z buffer, and
// the near plane is where depth reaches 1.
const char *kShaderMSL = R"(
#include <metal_stdlib>
using namespace metal;

// Polygon flags (mode.y).
constant uint kHasAlpha = 1;    // the texture has a mask
constant uint kColorKey = 2;    // black texels are see-through
constant uint kRound = 4;       // soft round falloff across the texture
constant uint kMasked = 8;      // the polygon asked for blending: the mask applies
constant uint kAlpha = 16;      // drawn alpha-blended
constant uint kAdditive = 32;   // drawn additively

struct VIn {
    float3 pos [[attribute(0)]];
    float2 uv [[attribute(1)]];      // texels
    float3 rgb [[attribute(2)]];
    float2 luv [[attribute(3)]];     // lightmap samples
    float4 lmRect [[attribute(4)]];  // lightmap's place in the atlas (texels); w = 0: none
    float3 misc [[attribute(5)]];    // texture w, h; lit (light scale and dynamic lights)
    float4 sphere [[attribute(6)]];  // bounding sphere (lit polygons pick lights by it)
    float4 plane [[attribute(7)]];   // normal, distance
    float2 mode [[attribute(8)]];    // alpha, flags
};
struct VOut {
    float4 pos [[position]];
    float3 world;
    float2 uv;
    float3 rgb;
    float2 luv;
    float4 lmRect [[flat]];
    float lit [[flat]];
    float4 sphere [[flat]];
    float4 plane [[flat]];
    float2 mode [[flat]];
};
struct VU {
    float4 right, up, fwd, eye;
    float4 proj;  // x scale, y scale, nearZ
};
struct Light {
    float4 pos;    // xyz, radius
    float4 color;  // rgb, skip backfacing
};
struct FU {
    float4 lightScale;
    float4 atlasInv;  // 1 / atlas w, h; light count
    Light lights[64];
};

vertex VOut vs_world(VIn in [[stage_in]], constant VU &u [[buffer(0)]])
{
    float3 d = in.pos - u.eye.xyz;
    float x = dot(d, u.right.xyz), y = dot(d, u.up.xyz), z = dot(d, u.fwd.xyz);
    VOut o;
    o.pos = float4(x * u.proj.x, y * u.proj.y, u.proj.z, z);
    o.world = in.pos;
    o.uv = in.uv / in.misc.xy;
    o.rgb = in.rgb;
    o.luv = in.luv;
    o.lmRect = in.lmRect;
    o.lit = in.misc.z;
    o.sphere = in.sphere;
    o.plane = in.plane;
    o.mode = in.mode;
    return o;
}

// The software rasterizer's per-pixel rules (render_software.cpp).
fragment float4 fs_world(VOut in [[stage_in]], constant FU &u [[buffer(0)]],
                         texture2d<float> tex [[texture(0)]], texture2d<float> lightmaps [[texture(1)]],
                         sampler texSampler [[sampler(0)]], sampler lmSampler [[sampler(1)]])
{
    const uint flags = uint(in.mode.y);
    const bool blended = (flags & (kAlpha | kAdditive)) != 0;
    float3 light = in.rgb;
    if (in.lmRect.z > 0) {
        // Bilinear between the samples, clamped to this lightmap's own texels.
        float2 c = clamp(in.luv, float2(0), in.lmRect.zw - 1);
        light = lightmaps.sample(lmSampler, (in.lmRect.xy + c + 0.5) * u.atlasInv.xy).rgb;
    }
    if (in.lit > 0) {
        light *= u.lightScale.rgb;
        // The first 8 lights that reach the polygon, each fading out with distance.
        int used = 0;
        const int count = int(u.atlasInv.z);
        for (int i = 0; i < count && used < 8; ++i) {
            const Light L = u.lights[i];
            float3 d = L.pos.xyz - in.sphere.xyz;
            float reach = L.pos.w + in.sphere.w;
            if (dot(d, d) > reach * reach) continue;
            float side = dot(L.pos.xyz, in.plane.xyz) - in.plane.w;
            if (fabs(side) > L.pos.w) continue;
            if (L.color.w > 0 && side < 0) continue;
            ++used;
            float3 e = in.world - L.pos.xyz;
            float d2 = dot(e, e);
            if (d2 >= L.pos.w * L.pos.w) continue;
            light += L.color.rgb * (1.0 - sqrt(d2) / L.pos.w);
        }
    }
    float fade = 1.0;
    if ((flags & kRound) && !(flags & kHasAlpha)) {
        float2 c = in.uv * 2.0 - 1.0;
        fade = 1.0 - dot(c, c);
        if (fade <= 0.0) discard_fragment();
    }
    float4 t = tex.sample(texSampler, in.uv);
    if ((flags & kHasAlpha) && (flags & (kColorKey | kMasked))) {
        // The mask shapes sprites and effects; world surfaces ignore it.
        float ta = t.a * 255.0;
        if (ta < 0.5 || (!blended && ta < 127.5)) discard_fragment();
        if (blended) fade *= t.a;
    } else if ((flags & kColorKey) && all(t.rgb * 255.0 < 0.5)) {
        discard_fragment();
    }
    float alpha = in.mode.x;
    if (flags & kAlpha) alpha *= fade;  // alpha blends fade by coverage
    else light *= fade;                 // additive (and opaque) fade the color
    // Truncated to 8 bits like the software renderer (stores round otherwise).
    float3 c = floor(min(t.rgb * light, 1.0) * 255.0 + 0.001) / 255.0;
    return float4(c, blended ? alpha : 0.0);
}
)";

struct GpuVert {
    float px, py, pz;
    float u, v;
    float r, g, b;
    float lu, lv;
    float lmx, lmy, lmw, lmh;
    float tw, th, lit;
    float sx, sy, sz, sr;  // bounding sphere
    float nx, ny, nz, nd;  // plane
    float alpha, flags;
};

enum : uint32_t { kHasAlpha = 1, kColorKey = 2, kRound = 4, kMasked = 8, kAlphaBlend = 16, kAdditiveBlend = 32 };
constexpr int kMaxLights = 64;

struct VertUniforms {
    float right[4], up[4], fwd[4], eye[4], proj[4];
};
struct GpuLight {
    float pos[4];    // xyz, radius
    float color[4];  // rgb, skip backfacing
};
struct FragUniforms {
    float lightScale[4];
    float atlasInv[4];  // 1 / atlas w, h; light count
    GpuLight lights[kMaxLights];
};

// Pipelines: blend mode × depth test.
int pipelineIndex(Blend blend, bool noZ) { return int(blend) * 2 + (noZ ? 1 : 0); }
constexpr int kPipelines = 6;

struct Batch {
    int tex;  // -1: untextured (white)
    int pipeline;
    Uint32 first, count;
};

struct Geometry {
    SDL_GPUBuffer *buffer = nullptr;
    std::vector<Batch> batches;
    size_t polys = 0, lightmapped = 0;
};

struct AtlasRect {
    int x = 0, y = 0, w = 0, h = 0;
};

class GpuBackend final : public RenderBackend {
public:
    ~GpuBackend() override;
    bool init(SDL_GPUDevice *device, GpuScreen *screen);

    void begin(Surface &dst, const RenderFrame &frame) override;
    void clearDepth() override { m_clearDepth = true; }
    void draw(const std::vector<DrawPoly> &polys, const DrawPass &pass) override;
    void screenAdd(const float rgb[3]) override
    {
        for (int i = 0; i < 3; ++i) m_screenAdd[i] = rgb[i];
        m_hasScreenAdd = true;
    }
    void end() override;

private:
    void releaseScene();
    void syncResources();
    void uploadTextures(size_t from);
    void buildAtlas();
    Geometry build(const std::vector<DrawPoly> &polys, const DrawPass &pass);
    SDL_GPUBuffer *upload(const std::vector<GpuVert> &verts);
    SDL_GPUTexture *makeTexture(SDL_GPUTextureFormat format, Uint32 usage, int w, int h);
    void uploadTexture(SDL_GPUTexture *tex, const void *data, int w, int h, int bytesPerTexel);
    void applyScreenAdd();

    SDL_GPUDevice *m_device = nullptr;
    bool m_ownsDevice = false;
    GpuScreen *m_screen = nullptr;
    bool m_direct = false;               // this frame draws into the GPU screen
    SDL_GPUTexture *m_target = nullptr;  // m_color, or the screen's texture
    SDL_GPUGraphicsPipeline *m_pipelines[kPipelines] = {};
    SDL_GPUSampler *m_texSampler = nullptr, *m_lmSampler = nullptr;
    SDL_GPUTexture *m_white = nullptr;

    // The frame being drawn.
    SDL_GPUCommandBuffer *m_cmd = nullptr;
    Surface *m_dst = nullptr;
    RenderFrame m_frame;
    SDL_GPUTexture *m_color = nullptr, *m_depth = nullptr;
    int m_w = 0, m_h = 0;
    bool m_clearDepth = false;
    bool m_hasScreenAdd = false;
    float m_screenAdd[3] = {0, 0, 0};

    // What the scene's resources look like on the GPU.
    uint64_t m_generation = ~uint64_t(0);
    std::vector<SDL_GPUTexture *> m_textures;
    SDL_GPUTexture *m_atlas = nullptr;
    int m_atlasW = 1, m_atlasH = 1;
    std::vector<AtlasRect> m_atlasRects;
    std::map<std::tuple<const void *, bool, size_t>, Geometry> m_stable;
};

GpuBackend::~GpuBackend()
{
    if (!m_device) {
        if (m_ownsDevice) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return;
    }
    SDL_WaitForGPUIdle(m_device);
    releaseScene();
    if (m_color) SDL_ReleaseGPUTexture(m_device, m_color);
    if (m_depth) SDL_ReleaseGPUTexture(m_device, m_depth);
    if (m_white) SDL_ReleaseGPUTexture(m_device, m_white);
    if (m_texSampler) SDL_ReleaseGPUSampler(m_device, m_texSampler);
    if (m_lmSampler) SDL_ReleaseGPUSampler(m_device, m_lmSampler);
    for (SDL_GPUGraphicsPipeline *p : m_pipelines)
        if (p) SDL_ReleaseGPUGraphicsPipeline(m_device, p);
    if (m_ownsDevice) {
        SDL_DestroyGPUDevice(m_device);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

bool GpuBackend::init(SDL_GPUDevice *device, GpuScreen *screen)
{
    m_device = device;
    m_screen = screen;
    if (!m_device) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;  // SDL's GPU devices need it
        m_ownsDevice = true;
        m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
        if (!m_device) return false;
    }
    if (!(SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL)) {
        SDL_SetError("the GPU device doesn't take MSL shaders");
        return false;
    }

    SDL_GPUShaderCreateInfo si{};
    si.code = reinterpret_cast<const Uint8 *>(kShaderMSL);
    si.code_size = strlen(kShaderMSL);
    si.format = SDL_GPU_SHADERFORMAT_MSL;
    si.entrypoint = "vs_world";
    si.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    si.num_uniform_buffers = 1;
    SDL_GPUShader *vs = SDL_CreateGPUShader(m_device, &si);
    si.entrypoint = "fs_world";
    si.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    si.num_samplers = 2;
    SDL_GPUShader *fs = SDL_CreateGPUShader(m_device, &si);
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(m_device, vs);
        if (fs) SDL_ReleaseGPUShader(m_device, fs);
        return false;
    }

    SDL_GPUVertexBufferDescription vb{0, sizeof(GpuVert), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    SDL_GPUVertexAttribute attrs[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVert, px)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVert, u)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVert, r)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVert, lu)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVert, lmx)},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GpuVert, tw)},
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVert, sx)},
        {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuVert, nx)},
        {8, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVert, alpha)},
    };
    SDL_GPUColorTargetDescription ct{};
    ct.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;  // the frame's XRGB8888
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs;
    pi.fragment_shader = fs;
    pi.vertex_input_state.vertex_buffer_descriptions = &vb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attrs;
    pi.vertex_input_state.num_vertex_attributes = SDL_arraysize(attrs);
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // sky boxes are seen from inside
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.has_depth_stencil_target = true;
    pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    bool ok = true;
    for (Blend blend : {Opaque, Alpha, Additive})
        for (bool noZ : {false, true}) {
            // Only opaque polygons write depth; noZ ones draw over everything.
            pi.depth_stencil_state.enable_depth_test = true;
            pi.depth_stencil_state.enable_depth_write = blend == Opaque;
            pi.depth_stencil_state.compare_op = noZ ? SDL_GPU_COMPAREOP_ALWAYS : SDL_GPU_COMPAREOP_GREATER;
            ct.blend_state = {};
            if (blend != Opaque) {
                ct.blend_state.enable_blend = true;
                ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
                ct.blend_state.dst_color_blendfactor =
                    blend == Additive ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;  // keep the frame's alpha
                ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            }
            SDL_GPUGraphicsPipeline *&p = m_pipelines[pipelineIndex(blend, noZ)];
            p = SDL_CreateGPUGraphicsPipeline(m_device, &pi);
            ok = ok && p;
        }
    SDL_ReleaseGPUShader(m_device, vs);
    SDL_ReleaseGPUShader(m_device, fs);
    if (!ok) return false;

    SDL_GPUSamplerCreateInfo smp{};
    smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_NEAREST;  // the software look, for now
    smp.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    m_texSampler = SDL_CreateGPUSampler(m_device, &smp);
    smp.min_filter = smp.mag_filter = SDL_GPU_FILTER_LINEAR;
    smp.address_mode_u = smp.address_mode_v = smp.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_lmSampler = SDL_CreateGPUSampler(m_device, &smp);

    m_white = makeTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, 1);
    const uint32_t white = 0xFFFFFFFF;
    if (m_white) uploadTexture(m_white, &white, 1, 1, 4);
    return m_texSampler && m_lmSampler && m_white;
}

SDL_GPUTexture *GpuBackend::makeTexture(SDL_GPUTextureFormat format, Uint32 usage, int w, int h)
{
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = format;
    ti.usage = usage;
    ti.width = Uint32(std::max(w, 1));
    ti.height = Uint32(std::max(h, 1));
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    return SDL_CreateGPUTexture(m_device, &ti);
}

// Uploads on a command buffer of its own, ahead of the frame's.
void GpuBackend::uploadTexture(SDL_GPUTexture *tex, const void *data, int w, int h, int bytesPerTexel)
{
    const Uint32 bytes = Uint32(w) * Uint32(h) * Uint32(bytesPerTexel);
    SDL_GPUTransferBufferCreateInfo bi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &bi);
    if (!tb) return;
    if (void *p = SDL_MapGPUTransferBuffer(m_device, tb, false)) {
        memcpy(p, data, bytes);
        SDL_UnmapGPUTransferBuffer(m_device, tb);
    }
    SDL_GPUCommandBuffer *cmd = m_cmd ? m_cmd : SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{tb, 0, Uint32(w), Uint32(h)};
    SDL_GPUTextureRegion dst{};
    dst.texture = tex;
    dst.w = Uint32(w);
    dst.h = Uint32(h);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    if (cmd != m_cmd) SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(m_device, tb);  // freed once the GPU is done with it
}

void GpuBackend::releaseScene()
{
    for (SDL_GPUTexture *t : m_textures)
        if (t) SDL_ReleaseGPUTexture(m_device, t);
    m_textures.clear();
    if (m_atlas) SDL_ReleaseGPUTexture(m_device, m_atlas);
    m_atlas = nullptr;
    m_atlasRects.clear();
    for (auto &kv : m_stable)
        if (kv.second.buffer) SDL_ReleaseGPUBuffer(m_device, kv.second.buffer);
    m_stable.clear();
}

void GpuBackend::syncResources()
{
    const RenderResources &res = *m_frame.res;
    if (res.generation != m_generation) {
        releaseScene();
        m_generation = res.generation;
        buildAtlas();
    }
    if (m_textures.size() < res.textures.size()) uploadTextures(m_textures.size());
}

void GpuBackend::uploadTextures(size_t from)
{
    const RenderResources &res = *m_frame.res;
    for (size_t i = from; i < res.textures.size(); ++i) {
        const RenderTexture &t = res.textures[i];
        // 0xAARRGGBB words are B, G, R, A in memory.
        SDL_GPUTexture *tex = makeTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, t.w, t.h);
        if (tex && t.px.size() == size_t(t.w) * size_t(t.h)) uploadTexture(tex, t.px.data(), t.w, t.h, 4);
        m_textures.push_back(tex);
    }
}

// Packs every lightmap into one RGBA16F texture, in shelves, a texel apart.
void GpuBackend::buildAtlas()
{
    const RenderResources &res = *m_frame.res;
    const size_t n = res.lightmaps.size();
    m_atlasRects.assign(n, {});
    m_atlasW = m_atlasH = 1;
    if (!n) return;
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return res.lightmaps[a].h > res.lightmaps[b].h; });
    size_t area = 0;
    for (const RenderLightmap &lm : res.lightmaps) area += size_t(lm.w + 1) * size_t(lm.h + 1);
    int width = 1024;
    while (width < 16384 && size_t(width) * size_t(width) < area) width *= 2;
    int x = 0, y = 0, shelf = 0;
    for (size_t i : order) {
        const RenderLightmap &lm = res.lightmaps[i];
        if (x + lm.w > width) {
            x = 0;
            y += shelf + 1;
            shelf = 0;
        }
        m_atlasRects[i] = {x, y, lm.w, lm.h};
        x += lm.w + 1;
        shelf = std::max(shelf, lm.h);
    }
    m_atlasW = width;
    m_atlasH = y + shelf;
    if (m_atlasH > 16384) {
        SDL_Log("lightmap atlas too tall (%d); lightmaps off", m_atlasH);
        m_atlasRects.assign(n, {});
        return;
    }
    std::vector<_Float16> texels(size_t(m_atlasW) * size_t(m_atlasH) * 4, _Float16(0));
    for (size_t i = 0; i < n; ++i) {
        const RenderLightmap &lm = res.lightmaps[i];
        const AtlasRect &r = m_atlasRects[i];
        for (int j = 0; j < lm.h; ++j)
            for (int k = 0; k < lm.w; ++k) {
                const float *s = &lm.rgb[(size_t(j) * size_t(lm.w) + size_t(k)) * 3];
                _Float16 *d = &texels[((size_t(r.y + j) * size_t(m_atlasW)) + size_t(r.x + k)) * 4];
                d[0] = _Float16(s[0]);
                d[1] = _Float16(s[1]);
                d[2] = _Float16(s[2]);
                d[3] = _Float16(1);
            }
    }
    m_atlas = makeTexture(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTUREUSAGE_SAMPLER, m_atlasW, m_atlasH);
    if (m_atlas) uploadTexture(m_atlas, texels.data(), m_atlasW, m_atlasH, 8);
}

// Triangles for a list: opaque polygons first, grouped by texture, then the
// blended ones in their own order.
Geometry GpuBackend::build(const std::vector<DrawPoly> &polys, const DrawPass &pass)
{
    const RenderResources &res = *m_frame.res;
    struct Item {
        const DrawPoly *poly;
        Blend blend;  // as drawn: world translucency blends at half alpha
        float alpha;
    };
    std::vector<Item> opaque, blended;
    for (const DrawPoly &p : polys) {
        if (p.verts.size() < 3) continue;
        if (!pass.sky && (p.flags & kSurfSky)) continue;  // sky portals show the sky behind
        Item it{&p, p.blend, p.alpha};
        if (it.blend == Opaque && (p.flags & kSurfTranslucent)) {
            it.blend = Alpha;
            it.alpha = 0.5f;
        }
        (it.blend == Opaque ? opaque : blended).push_back(it);
    }
    std::stable_sort(opaque.begin(), opaque.end(), [](const Item &a, const Item &b) { return a.poly->tex < b.poly->tex; });
    opaque.insert(opaque.end(), blended.begin(), blended.end());

    Geometry geo;
    std::vector<GpuVert> verts;
    for (const Item &it : opaque) {
        const DrawPoly *p = it.poly;
        const int tex = p->tex >= 0 && size_t(p->tex) < res.textures.size() ? p->tex : -1;
        const RenderTexture *t = tex >= 0 ? &res.textures[size_t(tex)] : nullptr;
        const float tw = t ? float(t->w) : 1.0f, th = t ? float(t->h) : 1.0f;
        AtlasRect lm;
        if (!pass.sky && p->lightmap >= 0 && size_t(p->lightmap) < m_atlasRects.size()) lm = m_atlasRects[size_t(p->lightmap)];
        const float lit = p->lit && !pass.sky ? 1.0f : 0.0f;
        uint32_t flags = 0;
        if (t && t->hasAlpha) flags |= kHasAlpha;
        if (p->colorKey) flags |= kColorKey;
        if (p->round) flags |= kRound;
        if (p->blend != Opaque) flags |= kMasked;
        if (it.blend == Alpha) flags |= kAlphaBlend;
        if (it.blend == Additive) flags |= kAdditiveBlend;
        const float alpha = std::clamp(it.alpha, 0.0f, 1.0f);
        const Vec3 &n = p->normal, &v0 = p->verts[0].p;
        const float nd = n.x * v0.x + n.y * v0.y + n.z * v0.z;
        const int pipe = pipelineIndex(it.blend, p->noZ);
        if (geo.batches.empty() || geo.batches.back().tex != tex || geo.batches.back().pipeline != pipe)
            geo.batches.push_back({tex, pipe, Uint32(verts.size()), 0});
        auto vert = [&](const DrawVert &v) {
            verts.push_back({v.p.x, v.p.y, v.p.z, v.u, v.v, v.r, v.g, v.b, v.lu, v.lv, float(lm.x), float(lm.y),
                             float(lm.w), float(lm.h), tw, th, lit, p->sphereCenter.x, p->sphereCenter.y,
                             p->sphereCenter.z, p->sphereRadius, n.x, n.y, n.z, nd, alpha, float(flags)});
        };
        for (size_t k = 1; k + 1 < p->verts.size(); ++k) {
            vert(p->verts[0]);
            vert(p->verts[k]);
            vert(p->verts[k + 1]);
        }
        geo.batches.back().count = Uint32(verts.size()) - geo.batches.back().first;
        ++geo.polys;
        if (lm.w > 0) ++geo.lightmapped;
    }
    geo.buffer = verts.empty() ? nullptr : upload(verts);
    return geo;
}

SDL_GPUBuffer *GpuBackend::upload(const std::vector<GpuVert> &verts)
{
    const Uint32 bytes = Uint32(verts.size() * sizeof(GpuVert));
    SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_VERTEX, bytes, 0};
    SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(m_device, &bi);
    SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &ti);
    if (!buf || !tb) {
        if (buf) SDL_ReleaseGPUBuffer(m_device, buf);
        if (tb) SDL_ReleaseGPUTransferBuffer(m_device, tb);
        return nullptr;
    }
    if (void *p = SDL_MapGPUTransferBuffer(m_device, tb, false)) {
        memcpy(p, verts.data(), bytes);
        SDL_UnmapGPUTransferBuffer(m_device, tb);
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(m_cmd);
    SDL_GPUTransferBufferLocation src{tb, 0};
    SDL_GPUBufferRegion dst{buf, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(m_device, tb);
    return buf;
}

void GpuBackend::begin(Surface &dst, const RenderFrame &frame)
{
    m_dst = &dst;
    m_frame = frame;
    m_hasScreenAdd = false;
    m_direct = m_screen && m_screen->isScreen(dst);
    if (m_direct) {
        m_cmd = m_screen->beginExternal(dst, &m_target);
    } else {
        m_cmd = SDL_AcquireGPUCommandBuffer(m_device);
    }
    if (!m_cmd) return;
    if (!m_color || m_w != dst.width || m_h != dst.height) {
        if (m_color) SDL_ReleaseGPUTexture(m_device, m_color);
        if (m_depth) SDL_ReleaseGPUTexture(m_device, m_depth);
        m_color = makeTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, dst.width, dst.height);
        m_depth = makeTexture(SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, dst.width,
                              dst.height);
        m_w = dst.width;
        m_h = dst.height;
    }
    m_clearDepth = true;
    if (m_direct) return;
    m_target = m_color;
    // What's already on the surface stays under the 3D.
    uploadTexture(m_color, dst.pixels.data(), dst.width, dst.height, 4);
}

void GpuBackend::draw(const std::vector<DrawPoly> &polys, const DrawPass &pass)
{
    if (!m_cmd || !m_target || !m_depth || polys.empty()) return;
    syncResources();
    const DRect vp = viewport(m_frame.cam, m_w, m_h);
    const float vw = float(vp.right - vp.left), vh = float(vp.bottom - vp.top);
    if (vw <= 0 || vh <= 0) return;

    Geometry temp;
    const Geometry *geo = &temp;
    if (pass.stable) {
        auto key = std::make_tuple(static_cast<const void *>(&polys), pass.sky, polys.size());
        auto it = m_stable.find(key);
        if (it == m_stable.end()) it = m_stable.emplace(key, build(polys, pass)).first;
        geo = &it->second;
    } else {
        temp = build(polys, pass);
    }
    if (m_frame.stats) {
        m_frame.stats->drawnPolys += geo->polys;
        m_frame.stats->lightmaps += geo->lightmapped;
    }
    if (!geo->buffer) return;

    const ViewBasis b = viewBasis(m_frame.cam);
    const float t = std::tan(m_frame.cam.fovX * 0.5f);
    VertUniforms vu = {{b.right.x, b.right.y, b.right.z, 0},
                       {b.up.x, b.up.y, b.up.z, 0},
                       {b.forward.x, b.forward.y, b.forward.z, 0},
                       {pass.eye.x, pass.eye.y, pass.eye.z, 0},
                       {1.0f / t, (vw / vh) / t, pass.nearZ, 0}};
    FragUniforms fu{};
    for (int i = 0; i < 3; ++i) fu.lightScale[i] = m_frame.lightScale[i];
    fu.lightScale[3] = 1;
    int nLights = 0;
    if (m_frame.lights)
        for (const Light &L : *m_frame.lights) {
            if (nLights == kMaxLights) break;
            fu.lights[nLights++] = {{L.pos.x, L.pos.y, L.pos.z, L.radius}, {L.r, L.g, L.b, L.skipBackfacing ? 1.0f : 0.0f}};
        }
    fu.atlasInv[0] = 1.0f / float(m_atlasW);
    fu.atlasInv[1] = 1.0f / float(m_atlasH);
    fu.atlasInv[2] = float(nLights);

    SDL_GPUColorTargetInfo ct{};
    ct.texture = m_target;
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = m_depth;
    dt.load_op = m_clearDepth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    dt.clear_depth = 0;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    m_clearDepth = false;

    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(m_cmd, &ct, 1, &dt);
    SDL_GPUViewport v{float(vp.left), float(vp.top), vw, vh, 0, 1};
    SDL_SetGPUViewport(rp, &v);
    SDL_Rect sc{vp.left, vp.top, vp.right - vp.left, vp.bottom - vp.top};
    SDL_SetGPUScissor(rp, &sc);
    SDL_PushGPUVertexUniformData(m_cmd, 0, &vu, sizeof vu);
    SDL_PushGPUFragmentUniformData(m_cmd, 0, &fu, sizeof fu);
    SDL_GPUBufferBinding vb{geo->buffer, 0};
    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    int bound = -1;
    for (const Batch &batch : geo->batches) {
        if (batch.pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(rp, m_pipelines[batch.pipeline]);
            bound = batch.pipeline;
        }
        SDL_GPUTexture *tex = batch.tex >= 0 && size_t(batch.tex) < m_textures.size() && m_textures[size_t(batch.tex)]
                                  ? m_textures[size_t(batch.tex)]
                                  : m_white;
        SDL_GPUTextureSamplerBinding sb[2] = {{tex, m_texSampler}, {m_atlas ? m_atlas : m_white, m_lmSampler}};
        SDL_BindGPUFragmentSamplers(rp, 0, sb, 2);
        SDL_DrawGPUPrimitives(rp, batch.count, 1, batch.first, 0);
    }
    SDL_EndGPURenderPass(rp);
    if (geo == &temp) SDL_ReleaseGPUBuffer(m_device, temp.buffer);  // freed once drawn
}

void GpuBackend::applyScreenAdd()
{
    Surface &dst = *m_dst;
    const DRect vp = viewport(m_frame.cam, dst.width, dst.height);
    const uint32_t add[3] = {uint32_t(std::clamp(m_screenAdd[2], 0.0f, 1.0f) * 255),
                             uint32_t(std::clamp(m_screenAdd[1], 0.0f, 1.0f) * 255),
                             uint32_t(std::clamp(m_screenAdd[0], 0.0f, 1.0f) * 255)};
    if (m_direct) {
        const uint32_t rgb[3] = {add[2], add[1], add[0]};
        m_screen->add(dst, vp, rgb);
        return;
    }
    for (int y = vp.top; y < vp.bottom; ++y) {
        uint32_t *row = dst.row(y);
        for (int x = vp.left; x < vp.right; ++x) {
            uint32_t px = row[x], out = px & 0xFF000000;
            for (int c = 0; c < 3; ++c) out |= std::min(((px >> (c * 8)) & 0xFF) + add[c], 255u) << (c * 8);
            row[x] = out;
        }
    }
}

void GpuBackend::end()
{
    if (!m_cmd) return;
    if (m_direct) {  // the screen submits its command buffer
        m_cmd = nullptr;
        if (m_hasScreenAdd) applyScreenAdd();
        m_dst = nullptr;
        return;
    }
    // Read the frame back into the surface.
    const Uint32 bytes = Uint32(m_w) * Uint32(m_h) * 4;
    SDL_GPUTransferBufferCreateInfo bi{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &bi);
    if (tb) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(m_cmd);
        SDL_GPUTextureRegion src{};
        src.texture = m_color;
        src.w = Uint32(m_w);
        src.h = Uint32(m_h);
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{tb, 0, Uint32(m_w), Uint32(m_h)};
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
    }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd);
    m_cmd = nullptr;
    if (fence) {
        SDL_WaitForGPUFences(m_device, true, &fence, 1);
        SDL_ReleaseGPUFence(m_device, fence);
    }
    if (tb) {
        if (const void *p = SDL_MapGPUTransferBuffer(m_device, tb, false)) {
            memcpy(m_dst->pixels.data(), p, bytes);
            SDL_UnmapGPUTransferBuffer(m_device, tb);
        }
        SDL_ReleaseGPUTransferBuffer(m_device, tb);
    }
    if (m_hasScreenAdd) applyScreenAdd();
    m_dst->touch();
    m_dst = nullptr;
}

}  // namespace

std::unique_ptr<RenderBackend> makeGpuBackend(SDL_GPUDevice *device, GpuScreen *screen)
{
    auto backend = std::make_unique<GpuBackend>();
    if (!backend->init(device, screen)) {
        SDL_Log("GPU renderer: %s", SDL_GetError());
        return nullptr;
    }
    return backend;
}

}  // namespace shogo
