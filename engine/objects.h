// Engine objects: what HOBJECT (server) and HLOCALOBJ (client) point at.
//
// Client and server share one process, so a server object is also the
// client's view of it; objects the client creates itself (its camera, effects)
// are marked clientOnly. Object ids give messages a stable way to refer to
// objects that may be removed in between.
#ifndef SHOGO_ENGINE_OBJECTS_H
#define SHOGO_ENGINE_OBJECTS_H

#include "engine/api.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shogo {

struct ModelInfo;

struct EngineObject {
    static constexpr uint32_t kMagic = 0x4F424A31;  // "OBJ1"
    uint32_t magic = kMagic;
    uint32_t id = 0;
    bool clientOnly = false;
    bool removed = false;
    bool active = true;

    int type = OT_NORMAL;
    DDWORD flags = 0, userFlags = 0, clientFlags = 0;
    DVector pos{}, scale{}, dims{}, velocity{}, accel{};
    DRotation rot{0, 0, 0, 1};
    float color[4] = {1, 1, 1, 1};
    float mass = 1.0f, friction = 1.0f, forceIgnoreLimit = 0.0f;
    float lightColor[3] = {1, 1, 1};
    float lightRadius = 0;
    std::string filename, skin, name;
    uintptr_t userData = 0;  // ObjectCreateStruct::m_UserData

    // Server side.
    ClassDef *cls = nullptr;
    LPBASECLASS game = nullptr;  // the game's object (BaseClass*)
    double nextUpdate = 0;       // absolute server time, 0 = none
    float deactivationTime = 0;
    int state = 0;
    std::vector<uint8_t> sfx;    // special-effect message for the client
    bool sfxSent = false;
    std::vector<EngineObject *> links;  // inter-object links owned by this object
    D_WORD containerCode = 0;
    DBYTE blockingPriority = 0;

    // Models: the parsed .ABC (loaded on first use) and the animation clock.
    const ModelInfo *model = nullptr;
    std::string modelFile;  // filename `model` was loaded for
    bool modelLoaded = false;
    int anim = -1;          // current animation, -1 = none
    float animTime = 0;     // ms
    bool animLoop = true, animDone = false, animStarted = false;
    std::vector<bool> hiddenNodes;
    bool traced = false;
    int tracedState = 0;  // debug traces print an object once  // SetModelNodeHideStatus, by node index

    // Client side.
    void *clientUserData = nullptr;
    bool clientKnown = false;

    // World models: index into the world's BSP list, and where it was built.
    int bsp = -1;
    DVector basePos{};
    DRotation baseRot{0, 0, 0, 1};

    // Cameras.
    bool camFullscreen = true;
    int camRect[4] = {0, 0, 0, 0};
    float fovX = 1.5708f, fovY = 1.1781f;
    DVector lightAdd{};

    // Poly grids (client-side height fields, e.g. the menu background).
    std::vector<char> grid;
    uint32_t gridW = 0, gridH = 0;
    std::vector<PGColor> gridColors;
    std::string gridTexture;
    float gridPan[2] = {0, 0}, gridScale[2] = {1, 1};
};

inline EngineObject *toObject(HOBJECT h)
{
    auto *o = reinterpret_cast<EngineObject *>(h);
    return (o && o->magic == EngineObject::kMagic) ? o : nullptr;
}
inline HOBJECT toHObject(EngineObject *o) { return reinterpret_cast<HOBJECT>(o); }
inline HLOCALOBJ toHLocal(EngineObject *o) { return reinterpret_cast<HLOCALOBJ>(o); }

// All live objects, in creation order.
class ObjectTable {
public:
    EngineObject *create(bool clientOnly);
    void remove(EngineObject *o);          // frees the object (call when it's really gone)
    EngineObject *find(uint32_t id) const;
    const std::vector<EngineObject *> &all() const { return m_order; }
    void clear();

private:
    std::vector<std::unique_ptr<EngineObject>> m_objects;
    std::vector<EngineObject *> m_order;
    uint32_t m_nextId = 1;
};

ObjectTable &objects();

// Called by ObjectTable::remove before an object is freed (drops attachments).
void objectRemoved(EngineObject *o);

}  // namespace shogo

#endif
