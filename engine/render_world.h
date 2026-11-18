// Draws LithTech worlds: loads a world's polygons, textures and lightmaps,
// places its world models, and each frame sorts what's visible into passes
// (sky, world, models, dynamic polygons, the view weapon) for a RenderBackend
// to draw. The default backend is the software rasterizer, which is simple
// and deterministic so it can be checked headless.
#ifndef SHOGO_ENGINE_RENDER_WORLD_H
#define SHOGO_ENGINE_RENDER_WORLD_H

#include "engine/render_backend.h"
#include "engine/rez_file.h"
#include "engine/surfaces.h"
#include "engine/world_dat.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace shogo {

class WorldRenderer {
public:
    using DrawVert = shogo::DrawVert;
    using DrawPoly = shogo::DrawPoly;
    using Light = shogo::Light;
    using Blend = shogo::Blend;
    static constexpr Blend Opaque = shogo::Opaque, Alpha = shogo::Alpha, Additive = shogo::Additive;
    using Stats = RenderStats;

    WorldRenderer();
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer &) = delete;
    WorldRenderer &operator=(const WorldRenderer &) = delete;
    // Who draws the pixels (the software backend unless replaced).
    void setBackend(std::unique_ptr<RenderBackend> backend);

    // Dynamic lights for the next render().
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
    // Draws just the dynamic polygons queued since the last render, over what
    // dst already shows, with a fresh depth buffer (RenderObjects).
    void renderDynamic(Surface &dst, const Camera &cam);
    // A texture (DTX or SPR) for dynamic polygons; -1 if missing. Size in w/h.
    int texture(const std::string &name, int *w = nullptr, int *h = nullptr);

    // The player start (first GameStartPoint), handy for viewers.
    bool startPoint(Camera &cam) const;

    const Stats &stats() const { return m_stats; }

private:
    int textureIndex(const std::string &name, const ResourceFS &fs);
    void addModel(const WorldBsp &bsp, std::vector<DrawPoly> &out, const ResourceFS &fs);
    RenderFrame frame(const Camera &cam);
    void clearDynamic();

    std::unique_ptr<RenderBackend> m_backend;
    RenderResources m_res;
    std::map<std::string, int> m_textureIds;
    std::vector<DrawPoly> m_world, m_sky, m_dynamic, m_blended, m_close;
    const ResourceFS *m_fs = nullptr;
    bool m_hasSky = false;
    Vec3 m_worldMin, m_worldMax, m_skyViewMin, m_skyViewMax;
    std::vector<WorldObject> m_starts;
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
