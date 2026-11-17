// Sprites (.SPR animations of DTX frames), particle systems and line systems:
// the state behind OT_SPRITE, OT_PARTICLESYSTEM and OT_LINESYSTEM objects,
// the client functions that fill them, and turning them into polygons.
//
// .SPR layout (from the game's files): u32 frame count, u32 frames per
// second, u32 transparent (black is see-through), u32 translucent (blended),
// u32 color key; then the frames' DTX names as str16.
#ifndef SHOGO_ENGINE_EFFECTS_H
#define SHOGO_ENGINE_EFFECTS_H

#include "engine/api.h"
#include "engine/world_dat.h"  // Vec3

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shogo {

class ResourceFS;
class WorldRenderer;
struct EngineObject;
struct Camera;

struct SpriteInfo {
    std::vector<std::string> frames;  // DTX names
    float fps = 0;
    bool transparent = false, translucent = false;
    uint32_t colorKey = 0;
};

// Parses an .SPR file; nullptr if malformed.
std::unique_ptr<SpriteInfo> parseSprite(const std::vector<uint8_t> &data);

// A sprite by filename (.spr, or a single .dtx frame), cached; nullptr if missing.
const SpriteInfo *loadSprite(const ResourceFS &fs, const std::string &name);

// World radius of a particle for a system's particleRadius (the game passes
// its own radius / 640).
constexpr float kParticleWorldScale = 2.0f;

// Advances sprite animations and particles by dt seconds (client side, every frame).
void updateEffects(float dt);

// Adds polygons for every visible sprite, particle system and line system.
void submitEffects(WorldRenderer &renderer, const ResourceFS &fs, const Vec3 &eye, const Vec3 &right, const Vec3 &up);

// Called when an object is freed.
void effectsObjectRemoved(EngineObject *o);

// Fills the particle and line system functions of the client interface.
void installEffectsAPI(ClientDE *client);

// Particle count in a system (tests).
size_t particleCount(EngineObject *o);

}  // namespace shogo

#endif
