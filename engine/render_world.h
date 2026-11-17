// Draws LithTech worlds into a software surface: perspective-correct textured
// polygons with the baked vertex lighting, a z-buffer, and the sky box.
//
// This is the first renderer: simple and deterministic so it can be checked
// headless. A GPU renderer can replace it behind the same interface later.
#ifndef SHOGO_ENGINE_RENDER_WORLD_H
#define SHOGO_ENGINE_RENDER_WORLD_H

#include "engine/rez_file.h"
#include "engine/surfaces.h"
#include "engine/world_dat.h"

#include <map>
#include <memory>
#include <string>
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

class WorldRenderer {
public:
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

    // A dynamic light for the next render(): adds color × (1 - distance / radius).
    struct Light {
        Vec3 pos;
        float radius = 0;
        float r = 1, g = 1, b = 1;
        bool skipBackfacing = false;  // FLAG_DONTLIGHTBACKFACING
    };
    void setLights(std::vector<Light> lights) { m_lights = std::move(lights); }
    // Multiplies the world's baked light (SetGlobalLightScale).
    void setLightScale(float r, float g, float b) { m_lightScale[0] = r; m_lightScale[1] = g; m_lightScale[2] = b; }
    // Where a world model (door, lift, breakable glass...) is now. Models with a
    // placement are drawn there (or not at all when invisible); the others stay
    // where they were built. world = rot * (built - basePos) + pos, rot row-major.
    struct ModelPlacement {
        int model = 0;
        bool visible = true;
        Vec3 basePos, pos;
        float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    };
    void setModelPlacements(std::vector<ModelPlacement> p) { m_placements = std::move(p); }

    // Added to every pixel of the view after drawing (SetCameraLightAdd), 0..1.
    void setScreenAdd(float r, float g, float b) { m_screenAdd[0] = r; m_screenAdd[1] = g; m_screenAdd[2] = b; }

    // Builds draw lists from a parsed world; textures come from fs.
    void load(const WorldData &world, const ResourceFS &fs);
    void render(Surface &dst, const Camera &cam);

    // Without a world: just a place to load textures from (model viewer).
    void setResources(const ResourceFS &fs) { m_fs = &fs; }

    // Per-frame polygons on top of the world (models), drawn by the next
    // render() and then dropped. "Really close" ones (the player's view
    // weapon) are drawn last, over everything. UVs are in texels.
    void addDynamic(DrawPoly poly, bool reallyClose);
    // A texture (DTX or SPR) for dynamic polygons; -1 if missing. Size in w/h.
    int texture(const std::string &name, int *w = nullptr, int *h = nullptr);

    // The player start (first GameStartPoint), handy for viewers.
    bool startPoint(Camera &cam) const;

    struct Stats {
        size_t polys = 0, skyPolys = 0, textures = 0, missingTextures = 0;
        size_t drawnPolys = 0, dynamicPolys = 0, litPolys = 0, lightmaps = 0;
    };
    const Stats &stats() const { return m_stats; }

private:
    struct Tex {
        int w = 1, h = 1;
        std::vector<uint32_t> px;  // 0xAARRGGBB; alpha 255 unless the texture has a mask
        bool hasAlpha = false;
    };
    struct Lightmap {
        int w = 0, h = 0;
        std::vector<float> rgb;  // 0..1, w * h * 3
    };
    int textureIndex(const std::string &name, const ResourceFS &fs);
    void addModel(const WorldBsp &bsp, std::vector<DrawPoly> &out, const ResourceFS &fs);
    void drawPolys(Surface &dst, const Camera &cam, const Vec3 &eye, const std::vector<DrawPoly> &polys, bool sky,
                   float nearZ = 1.0f);

    std::vector<Tex> m_textures;
    std::vector<Lightmap> m_lightmaps;
    std::map<std::string, int> m_textureIds;
    std::vector<DrawPoly> m_world, m_sky, m_dynamic, m_blended, m_close;
    const ResourceFS *m_fs = nullptr;
    bool m_hasSky = false;
    Vec3 m_worldMin, m_worldMax, m_skyViewMin, m_skyViewMax;
    std::vector<WorldObject> m_starts;
    std::vector<float> m_depth;  // 1/z per pixel
    std::vector<Light> m_lights;
    std::vector<std::vector<DrawPoly>> m_models;  // world models 1.., by index (sky ones empty)
    std::vector<ModelPlacement> m_placements;
    std::vector<DrawPoly> m_moved;                // this frame's transformed copies
    float m_lightScale[3] = {1, 1, 1};
    float m_screenAdd[3] = {0, 0, 0};
    Stats m_stats;
};

}  // namespace shogo

#endif
