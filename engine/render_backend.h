// What the world renderer hands to whatever draws pixels: polygons, textures,
// lightmaps and lights, and the RenderBackend interface that draws them.
//
// WorldRenderer (render_world.h) builds the scene: it loads the world's
// polygons and textures, places world models, sorts the frame into passes and
// calls a backend for each. The software backend rasterizes on the CPU; a GPU
// backend can implement the same calls.
#ifndef SHOGO_ENGINE_RENDER_BACKEND_H
#define SHOGO_ENGINE_RENDER_BACKEND_H

#include "engine/surfaces.h"
#include "engine/world_dat.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace shogo {

// Surface flag bits as observed in the world files (names are our best reading).
enum : uint32_t {
    kSurfSolid = 0x1,
    kSurfInvisible = 0x4,
    kSurfTranslucent = 0x8,
    kSurfSky = 0x10,
    kSurfFullbright = 0x20,
    kSurfLightmapped = 0x80,
};

struct Camera {
    Vec3 pos;
    float pitch = 0, yaw = 0, roll = 0;  // radians; LithTech euler order, +pitch looks down
    float fovX = 1.5708f;                // horizontal field of view
    DRect rect{0, 0, 0, 0};              // viewport on the target (empty = whole surface)
    // When set, these axes are used instead of the Euler angles.
    bool hasBasis = false;
    Vec3 right, up, forward;
};

// The camera's axes (left-handed, +Y up, +Z forward).
struct ViewBasis {
    Vec3 right, up, forward;
};
ViewBasis viewBasis(const Camera &cam);
// cam.rect clipped to the target, or the whole target when it's empty.
DRect viewport(const Camera &cam, int width, int height);

struct DrawVert {
    Vec3 p;
    float u, v;     // texels
    float r, g, b;  // 0..1 light
    float lu = 0, lv = 0;  // lightmap sample coordinates (lightmapped polygons)
};

enum Blend : uint8_t { Opaque, Alpha, Additive };

struct DrawPoly {
    std::vector<DrawVert> verts;
    int tex = -1;
    uint32_t flags = 0;
    Blend blend = Opaque;   // Alpha/Additive don't write depth
    float alpha = 1.0f;
    bool colorKey = false;  // black texels are see-through
    bool noZ = false;       // no depth test
    bool round = false;     // soft round falloff across the texture (particles)
    Vec3 sphereCenter;
    float sphereRadius = 0;
    bool lit = false;  // world surface with baked light: takes dynamic lights and the light scale
    Vec3 normal;       // front-facing plane normal (lit polygons)
    int lightmap = -1; // baked light per pixel instead of per vertex
};

// A dynamic light: adds color × (1 - distance / radius).
struct Light {
    Vec3 pos;
    float radius = 0;
    float r = 1, g = 1, b = 1;
    bool skipBackfacing = false;  // FLAG_DONTLIGHTBACKFACING
};

struct RenderTexture {
    int w = 1, h = 1;
    std::vector<uint32_t> px;  // 0xAARRGGBB; alpha 255 unless the texture has a mask
    bool hasAlpha = false;
};

struct RenderLightmap {
    int w = 0, h = 0;
    std::vector<float> rgb;  // 0..1, w * h * 3
};

struct RenderStats {
    size_t polys = 0, skyPolys = 0, textures = 0, missingTextures = 0;
    size_t drawnPolys = 0, dynamicPolys = 0, litPolys = 0, lightmaps = 0;
};

// Textures and lightmaps by index (DrawPoly::tex, DrawPoly::lightmap).
// Textures are only ever added; both lists are replaced when a world loads,
// and `generation` changes then.
struct RenderResources {
    std::vector<RenderTexture> textures;
    std::vector<RenderLightmap> lightmaps;
    uint64_t generation = 0;
};

// What applies to every draw in a frame.
struct RenderFrame {
    Camera cam;
    const RenderResources *res = nullptr;
    const std::vector<Light> *lights = nullptr;  // for lit polygons
    float lightScale[3] = {1, 1, 1};             // multiplies lit polygons' light
    RenderStats *stats = nullptr;                // drawnPolys, litPolys, lightmaps are counted here
};

struct DrawPass {
    Vec3 eye;            // where the camera is for this pass (the sky has its own)
    bool sky = false;    // sky box: no lightmaps, lights or sky-portal skipping
    float nearZ = 1.0f;
    // The list's contents stay as they are until resources.generation changes,
    // so a backend may keep its own copy (world, sky, unmoved world models).
    bool stable = false;
};

class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    // Starts drawing into dst with an empty depth buffer.
    virtual void begin(Surface &dst, const RenderFrame &frame) = 0;
    virtual void clearDepth() = 0;
    virtual void draw(const std::vector<DrawPoly> &polys, const DrawPass &pass) = 0;
    // Adds rgb (0..1) to every pixel of the viewport.
    virtual void screenAdd(const float rgb[3]) = 0;
    virtual void end() = 0;
};

std::unique_ptr<RenderBackend> makeSoftwareBackend();

}  // namespace shogo

#endif
