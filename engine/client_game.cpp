#include "engine/client_game.h"
#include "engine/collision.h"
#include "engine/effects.h"
#include "engine/sound.h"
#include "engine/interfaces.h"
#include "engine/model.h"

#include "engine/api_common.h"
#include "engine/math3d.h"
#include "engine/movement.h"
#include "engine/stub_support.h"

#include <dirent.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace shogo {

GameSession *GameSession::instance = nullptr;

namespace {
GameSession &G() { return *GameSession::instance; }
EngineObject *O(HLOCALOBJ h) { return toObject(h); }

constexpr int kToServer = 100;  // Message::kind for client -> server messages

// ---- connection ----------------------------------------------------------------------------

DRESULT startGame(StartGameRequest *req)
{
    if (!req) return LT_ERROR;
    GameSession &g = G();
    g.gameMode = req->m_Type;
    if (req->m_pGameInfo && req->m_GameInfoLen)
        g.server.gameInfo.assign((const uint8_t *)req->m_pGameInfo,
                                 (const uint8_t *)req->m_pGameInfo + req->m_GameInfoLen);
    else
        g.server.gameInfo.clear();
    if (!g.connected) {
        g.connected = true;
        g.server.addClient();
    }
    if (req->m_WorldName[0]) {
        // The engine loads the world itself; check it exists now so a bad name fails here.
        std::string path = req->m_WorldName;
        for (char &c : path)
            if (c == '\\') c = '/';
        if (path.size() < 4 || strcasecmp(path.c_str() + path.size() - 4, ".dat") != 0) path += ".dat";
        if (!g.ctx->resources->exists(path)) {
            fprintf(stderr, "[client] StartGame: no world %s\n", path.c_str());
            return LT_ERROR;
        }
        g.pendingWorld = req->m_WorldName;
    }
    return LT_OK;
}
DRESULT getGameMode(int *mode)
{
    if (mode) *mode = G().gameMode;
    return LT_OK;
}
DRESULT getLocalClientID(DDWORD *id)
{
    if (!G().connected) return LT_ERROR;
    if (id) *id = 1;
    return LT_OK;
}
DBOOL isConnected() { return G().connected ? DTRUE : DFALSE; }
void disconnect() { G().connected = false; }

DRESULT getWorldInfoString(char *file, char *out, DDWORD maxLen, DDWORD *actual)
{
    if (actual) *actual = 0;
    if (!file || !out || !maxLen) return LT_ERROR;
    std::string path = file;
    for (char &c : path)
        if (c == '\\') c = '/';
    if (path.size() < 4 || strcasecmp(path.c_str() + path.size() - 4, ".dat") != 0) path += ".dat";
    auto data = G().ctx->resources->read(path);
    if (!data || data->size() < 16) return LT_ERROR;
    uint32_t len = 0;
    memcpy(&len, data->data() + 12, 4);
    if (16 + size_t(len) > data->size()) return LT_ERROR;
    size_t n = std::min<size_t>(len, maxLen - 1);
    memcpy(out, data->data() + 16, n);
    out[n] = 0;
    if (actual) *actual = (DDWORD)n;
    return LT_OK;
}

// ---- console variables ---------------------------------------------------------------------

HCONSOLEVAR getConsoleVar(char *name) { return reinterpret_cast<HCONSOLEVAR>(name ? G().vars.find(name) : nullptr); }
float getVarValueFloat(HCONSOLEVAR h)
{
    auto *v = reinterpret_cast<ConsoleVars::Var *>(h);
    return v ? v->number : 0;
}
char *getVarValueString(HCONSOLEVAR h)
{
    auto *v = reinterpret_cast<ConsoleVars::Var *>(h);
    return v ? v->value.data() : nullptr;
}
void runConsoleString(char *s)
{
    if (s) G().vars.run(s);
}
DRESULT vGetSConValueFloat(void *, char *name, float &val)
{
    ConsoleVars::Var *v = name ? G().vars.find(name) : nullptr;
    if (!v) return LT_ERROR;
    val = v->number;
    return LT_OK;
}
DRESULT vGetSConValueString(void *, char *name, char *buf, DDWORD len)
{
    ConsoleVars::Var *v = name ? G().vars.find(name) : nullptr;
    if (!v || !buf || !len) return LT_ERROR;
    snprintf(buf, len, "%s", v->value.c_str());
    return LT_OK;
}
float vGetServerConVarValueFloat(void *, char *name)
{
    ConsoleVars::Var *v = name ? G().vars.find(name) : nullptr;
    return v ? v->number : 0;
}
char *vGetServerConVarValueString(void *, char *name)
{
    ConsoleVars::Var *v = name ? G().vars.find(name) : nullptr;
    return v ? v->value.data() : nullptr;
}

// ---- input -------------------------------------------------------------------------------

DRESULT apiReadConfigFile(char *name) { return (name && G().readConfigFile(name)) ? LT_OK : LT_ERROR; }
DRESULT apiWriteConfigFile(char *name) { return (name && G().writeConfigFile(name)) ? LT_OK : LT_ERROR; }
DBOOL isCommandOn(int c)
{
    bool on = G().input.isCommandOn(c);
    static bool trace = getenv("SHOGO_TRACE_INPUT") != nullptr;
    if (trace && on) printf("[input] command %d on\n", c);
    return on ? DTRUE : DFALSE;
}
void getAxisOffsets(DFLOAT *offsets) { G().input.axisOffsets(offsets); }
void setInputState(DBOOL on)
{
    G().input.enabled = on != DFALSE;
    if (getenv("SHOGO_TRACE_INPUT")) printf("[input] SetInputState(%d)\n", on ? 1 : 0);
}
// ---- input devices: names, bindings, tracking (the controls menus) ----------------------

const char *deviceName(DDWORD type)
{
    return type == DEVICETYPE_KEYBOARD ? "##keyboard" : type == DEVICETYPE_MOUSE ? "##mouse" : nullptr;
}
DRESULT getDeviceName(DDWORD type, char *buf, DDWORD size)
{
    const char *name = deviceName(type);
    if (!name || !buf || !size) return LT_ERROR;  // no joystick yet
    snprintf(buf, size, "%s", name);
    return LT_OK;
}
DRESULT isDeviceEnabled(char *name, DBOOL *enabled)
{
    if (enabled) *enabled = name && (!strcasecmp(name, "##keyboard") || !strcasecmp(name, "##mouse"));
    return LT_OK;
}
DeviceBinding *getDeviceBindings(DDWORD type)
{
    const char *dev = deviceName(type);
    if (!dev) return nullptr;
    DeviceBinding *head = nullptr, **tail = &head;
    for (const Input::Trigger &t : G().input.triggers(type == DEVICETYPE_MOUSE)) {
        auto *b = new DeviceBinding{};
        snprintf(b->strDeviceName, sizeof(b->strDeviceName), "%s", dev);
        snprintf(b->strTriggerName, sizeof(b->strTriggerName), "%s", t.name.c_str());
        b->nScale = t.scale;
        GameAction **atail = &b->pActionHead;
        for (const auto &[id, name] : t.actions) {
            auto *a = new GameAction{};
            a->nActionCode = id;
            snprintf(a->strActionName, sizeof(a->strActionName), "%s", name.c_str());
            *atail = a;
            atail = &a->pNext;
        }
        *tail = b;
        tail = &b->pNext;
    }
    return head;
}
void freeDeviceBindings(DeviceBinding *b)
{
    while (b) {
        for (GameAction *a = b->pActionHead; a;) {
            GameAction *next = a->pNext;
            delete a;
            a = next;
        }
        DeviceBinding *next = b->pNext;
        delete b;
        b = next;
    }
}
DeviceObject *getDeviceObjects(DDWORD types)
{
    DeviceObject *head = nullptr, **tail = &head;
    auto add = [&](DDWORD dev, DDWORD kind, const std::string &name) {
        auto *o = new DeviceObject{};
        o->m_DeviceType = dev;
        snprintf(o->m_DeviceName, sizeof(o->m_DeviceName), "%s", deviceName(dev));
        o->m_ObjectType = kind;
        snprintf(o->m_ObjectName, sizeof(o->m_ObjectName), "%s", name.c_str());
        *tail = o;
        tail = &o->m_pNext;
    };
    if (types & DEVICETYPE_MOUSE) {
        add(DEVICETYPE_MOUSE, CONTROLTYPE_XAXIS, Input::mouseObjectName(true, 0));
        add(DEVICETYPE_MOUSE, CONTROLTYPE_YAXIS, Input::mouseObjectName(true, 1));
        add(DEVICETYPE_MOUSE, CONTROLTYPE_ZAXIS, Input::mouseObjectName(true, 2));
        for (int i = 0; i < 3; ++i) add(DEVICETYPE_MOUSE, CONTROLTYPE_BUTTON, Input::mouseObjectName(false, i));
    }
    if (types & DEVICETYPE_KEYBOARD)
        for (int dik = 1; dik < 256; ++dik)
            if (!Input::keyName(dik).empty()) add(DEVICETYPE_KEYBOARD, CONTROLTYPE_KEY, Input::keyName(dik));
    return head;
}
void freeDeviceObjects(DeviceObject *o)
{
    while (o) {
        DeviceObject *next = o->m_pNext;
        delete o;
        o = next;
    }
}
DRESULT startDeviceTrack(DDWORD types, DDWORD)
{
    G().input.startTrack((types & DEVICETYPE_KEYBOARD) != 0, (types & DEVICETYPE_MOUSE) != 0);
    return LT_OK;
}
DRESULT trackDevice(DeviceInput *out, DDWORD *inOut)
{
    if (!inOut) return LT_ERROR;
    DDWORD n = 0;
    for (const Input::Tracked &t : G().input.takeTracked()) {
        std::string name = t.mouse ? Input::mouseObjectName(false, t.code) : Input::keyName(t.code);
        if (name.empty() || !out || n >= *inOut) continue;  // keys without a name can't be bound by name
        DeviceInput &d = out[n++];
        memset(&d, 0, sizeof(d));
        d.m_DeviceType = t.mouse ? DEVICETYPE_MOUSE : DEVICETYPE_KEYBOARD;
        snprintf(d.m_DeviceName, sizeof(d.m_DeviceName), "%s", deviceName(d.m_DeviceType));
        d.m_ControlType = t.mouse ? CONTROLTYPE_BUTTON : CONTROLTYPE_KEY;
        snprintf(d.m_ControlName, sizeof(d.m_ControlName), "%s", name.c_str());
        d.m_InputValue = 1;
    }
    *inOut = n;
    return LT_OK;
}
DRESULT endDeviceTrack()
{
    G().input.endTrack();
    return LT_OK;
}

DRESULT clearInput()
{
    G().input.clear();
    return LT_OK;
}

// ---- messages to the server ----------------------------------------------------------------

HMESSAGEWRITE vStartMessage(void *, DBYTE id)
{
    auto *m = new Message;
    m->kind = kToServer;
    m->id = id;
    return toWriteHandle(m);
}
DRESULT vEndMessage2(void *, HMESSAGEWRITE h, DDWORD)
{
    Message *m = toMessage(h);
    if (!m) return LT_ERROR;
    m->pos = 0;
    G().server.fromClient.push_back({(uint8_t)m->id, std::unique_ptr<Message>(m)});
    return LT_OK;
}
DRESULT vEndMessage(void *self, HMESSAGEWRITE h) { return vEndMessage2(self, h, 0); }
DRESULT vSetupEuler(void *, DRotation *r, float p, float y, float rl)
{
    if (r) *r = quatEuler(p, y, rl);
    return LT_OK;
}
DRESULT vGetRotationVectors(void *, DRotation *r, DVector *u, DVector *ri, DVector *f)
{
    quatVectors(r ? *r : quatIdentity(), u, ri, f);
    return LT_OK;
}

// ---- objects -------------------------------------------------------------------------------

HLOCALOBJ getClientObject()
{
    EngineObject *o = G().server.client.object;
    return o ? toHLocal(o) : nullptr;
}
HLOCALOBJ createObject(ObjectCreateStruct *ocs)
{
    if (!ocs) return nullptr;
    EngineObject *o = objects().create(true);
    o->type = ocs->m_ObjectType;
    o->flags = ocs->m_Flags;
    o->pos = ocs->m_Pos;
    o->scale = ocs->m_Scale;
    o->rot = ocs->m_Rotation;
    if (o->rot.m_Vec.x == 0 && o->rot.m_Vec.y == 0 && o->rot.m_Vec.z == 0 && o->rot.m_Spin == 0) o->rot = quatIdentity();
    o->filename = ocs->m_Filename;
    o->skin = ocs->m_SkinName;
    o->name = ocs->m_Name;
    objectModel(o);  // a model starts on animation 0, with its dims
    return toHLocal(o);
}
DRESULT deleteObject(HLOCALOBJ h)
{
    EngineObject *o = O(h);
    if (!o) return LT_ERROR;
    if (o->clientOnly) objects().remove(o);
    return LT_OK;
}
void getObjectPos(HLOCALOBJ h, DVector *p)
{
    EngineObject *o = O(h);
    if (p) *p = o ? o->pos : DVector(0, 0, 0);
}
void setObjectPos(HLOCALOBJ h, DVector *p)
{
    if (EngineObject *o = O(h); o && p) o->pos = *p;
}
void getObjectRotation(HLOCALOBJ h, DRotation *r)
{
    EngineObject *o = O(h);
    if (r) *r = o ? o->rot : quatIdentity();
}
void setObjectRotation(HLOCALOBJ h, DRotation *r)
{
    if (EngineObject *o = O(h); o && r) o->rot = *r;
}
void setObjectPosAndRotation(HLOCALOBJ h, DVector *p, DRotation *r)
{
    setObjectPos(h, p);
    setObjectRotation(h, r);
}
DDWORD getObjectType(HLOCALOBJ h)
{
    EngineObject *o = O(h);
    return o ? (DDWORD)o->type : 0;
}
DRESULT getObjectScale(HLOCALOBJ h, DVector *s)
{
    EngineObject *o = O(h);
    if (!o || !s) return LT_ERROR;
    *s = o->scale;
    return LT_OK;
}
DRESULT setObjectScale(HLOCALOBJ h, DVector *s)
{
    EngineObject *o = O(h);
    if (!o || !s) return LT_ERROR;
    o->scale = *s;
    return LT_OK;
}
void getObjectColor(HLOCALOBJ h, float *r, float *g, float *b, float *a)
{
    EngineObject *o = O(h);
    if (r) *r = o ? o->color[0] : 0;
    if (g) *g = o ? o->color[1] : 0;
    if (b) *b = o ? o->color[2] : 0;
    if (a) *a = o ? o->color[3] : 0;
}
void setObjectColor(HLOCALOBJ h, float r, float g, float b, float a)
{
    if (EngineObject *o = O(h)) {
        o->color[0] = r;
        o->color[1] = g;
        o->color[2] = b;
        o->color[3] = a;
    }
}
DDWORD getObjectFlags(HLOCALOBJ h)
{
    EngineObject *o = O(h);
    return o ? o->flags : 0;
}
void setObjectFlags(HLOCALOBJ h, DDWORD f)
{
    if (EngineObject *o = O(h)) o->flags = f;
}
DRESULT getObjectUserFlags(HLOCALOBJ h, DDWORD *f)
{
    EngineObject *o = O(h);
    if (!o || !f) return LT_ERROR;
    *f = o->userFlags;
    return LT_OK;
}
DRESULT setObjectUserFlags(HLOCALOBJ h, DDWORD f)
{
    EngineObject *o = O(h);
    if (!o) return LT_ERROR;
    o->userFlags = f;
    return LT_OK;
}
DDWORD getObjectClientFlags(HLOCALOBJ h)
{
    EngineObject *o = O(h);
    return o ? o->clientFlags : 0;
}
void setObjectClientFlags(HLOCALOBJ h, DDWORD f)
{
    if (EngineObject *o = O(h)) o->clientFlags = f;
}
void *getObjectUserData(HLOCALOBJ h)
{
    EngineObject *o = O(h);
    return o ? o->clientUserData : nullptr;
}
void setObjectUserData(HLOCALOBJ h, void *d)
{
    if (EngineObject *o = O(h)) o->clientUserData = d;
}

// ---- cameras -------------------------------------------------------------------------------

void getCameraFOV(HLOCALOBJ h, float *x, float *y)
{
    EngineObject *o = O(h);
    if (x) *x = o ? o->fovX : 0;
    if (y) *y = o ? o->fovY : 0;
}
void setCameraFOV(HLOCALOBJ h, float x, float y)
{
    if (EngineObject *o = O(h)) {
        o->fovX = x;
        o->fovY = y;
    }
}
void getCameraRect(HLOCALOBJ h, DBOOL *full, int *l, int *t, int *r, int *b)
{
    EngineObject *o = O(h);
    if (!o) return;
    if (full) *full = o->camFullscreen ? DTRUE : DFALSE;
    if (l) *l = o->camRect[0];
    if (t) *t = o->camRect[1];
    if (r) *r = o->camRect[2];
    if (b) *b = o->camRect[3];
}
void setCameraRect(HLOCALOBJ h, DBOOL full, int l, int t, int r, int b)
{
    if (EngineObject *o = O(h)) {
        o->camFullscreen = full != DFALSE;
        o->camRect[0] = l;
        o->camRect[1] = t;
        o->camRect[2] = r;
        o->camRect[3] = b;
    }
}
DBOOL getCameraLightAdd(HLOCALOBJ h, DVector *v)
{
    EngineObject *o = O(h);
    if (!o || !v) return DFALSE;
    *v = o->lightAdd;
    return DTRUE;
}
DBOOL setCameraLightAdd(HLOCALOBJ h, DVector *v)
{
    EngineObject *o = O(h);
    if (!o || !v) return DFALSE;
    o->lightAdd = *v;
    return DTRUE;
}

// ---- lights ------------------------------------------------------------------------------------

// SetGlobalLightScale: multiplies the world's (and models') base light.
DVector g_lightScale(1, 1, 1);

void getLightColorC(HLOCALOBJ h, float *r, float *g, float *b)
{
    EngineObject *o = O(h);
    if (r) *r = o ? o->lightColor[0] : 0;
    if (g) *g = o ? o->lightColor[1] : 0;
    if (b) *b = o ? o->lightColor[2] : 0;
}
void setLightColorC(HLOCALOBJ h, float r, float g, float b)
{
    if (EngineObject *o = O(h)) {
        o->lightColor[0] = r;
        o->lightColor[1] = g;
        o->lightColor[2] = b;
    }
}
float getLightRadiusC(HLOCALOBJ h) { EngineObject *o = O(h); return o ? o->lightRadius : 0; }
void setLightRadiusC(HLOCALOBJ h, float r) { if (EngineObject *o = O(h)) o->lightRadius = r; }
void getGlobalLightScale(DVector *v) { if (v) *v = g_lightScale; }
void setGlobalLightScale(DVector *v) { if (v) g_lightScale = *v; }
void offsetGlobalLightScale(DVector *v)
{
    if (!v) return;
    g_lightScale.x = std::clamp(g_lightScale.x + v->x, 0.0f, 1.0f);
    g_lightScale.y = std::clamp(g_lightScale.y + v->y, 0.0f, 1.0f);
    g_lightScale.z = std::clamp(g_lightScale.z + v->z, 0.0f, 1.0f);
}

// Where each object-backed world model (doors, lifts, glass) is this frame.
std::vector<WorldRenderer::ModelPlacement> modelPlacements()
{
    std::vector<WorldRenderer::ModelPlacement> out;
    static const bool off = getenv("SHOGO_NOPLACEMENTS") != nullptr;
    if (off) return out;
    for (EngineObject *o : objects().all()) {
        if (o->removed || o->bsp <= 0) continue;
        WorldRenderer::ModelPlacement p;
        p.model = o->bsp;
        p.visible = (o->flags & FLAG_VISIBLE) != 0;
        p.basePos = {o->basePos.x, o->basePos.y, o->basePos.z};
        p.pos = {o->pos.x, o->pos.y, o->pos.z};
        const DRotation &b = o->baseRot;
        DRotation r = quatNormalize(quatMul(o->rot, DRotation(-b.m_Vec.x, -b.m_Vec.y, -b.m_Vec.z, b.m_Spin)));
        if (std::fabs(r.m_Spin) < 0.999999f) {
            const DVector axes[3] = {quatRotate(r, DVector(1, 0, 0)), quatRotate(r, DVector(0, 1, 0)),
                                     quatRotate(r, DVector(0, 0, 1))};
            for (int c = 0; c < 3; ++c) {
                p.rot[0 * 3 + c] = axes[c].x;
                p.rot[1 * 3 + c] = axes[c].y;
                p.rot[2 * 3 + c] = axes[c].z;
            }
        }
        static const bool trace = getenv("SHOGO_TRACE_WORLDMODELS") != nullptr;
        if (trace) {
            float moved = std::fabs(p.pos.x - p.basePos.x) + std::fabs(p.pos.y - p.basePos.y) +
                          std::fabs(p.pos.z - p.basePos.z) + (1 - std::fabs(r.m_Spin)) * 1000;
            int state = !p.visible ? 2 : moved > 0.5f ? 1 : 0;
            if (state != o->tracedState) {
                o->tracedState = state;
                fprintf(stderr, "[worldmodel] %s model %d %s pos (%.0f %.0f %.0f) base (%.0f %.0f %.0f)\n",
                        o->name.c_str(), o->bsp, state == 2 ? "hidden" : state == 1 ? "moved" : "home", p.pos.x,
                        p.pos.y, p.pos.z, p.basePos.x, p.basePos.y, p.basePos.z);
            }
        }
        out.push_back(p);
    }
    return out;
}

// Visible light objects (client and server side) with a radius.
std::vector<WorldRenderer::Light> gatherLights(std::vector<bool> *onlyWorld = nullptr)
{
    std::vector<WorldRenderer::Light> out;
    static const bool off = getenv("SHOGO_NOLIGHTS") != nullptr;
    if (off) return out;
    // The game's "dynamic lighting" option (Display menu) sets DynamicLight.
    if (ConsoleVars::Var *v = GameSession::instance ? G().vars.find("DynamicLight") : nullptr; v && !v->value.empty() && v->number == 0) return out;
    for (EngineObject *o : objects().all()) {
        if (o->removed || o->type != OT_LIGHT || !(o->flags & FLAG_VISIBLE) || o->lightRadius <= 0) continue;
        WorldRenderer::Light L;
        L.pos = {o->pos.x, o->pos.y, o->pos.z};
        L.radius = o->lightRadius;
        L.r = o->lightColor[0];
        L.g = o->lightColor[1];
        L.b = o->lightColor[2];
        L.skipBackfacing = (o->flags & FLAG_DONTLIGHTBACKFACING) != 0;
        out.push_back(L);
        if (onlyWorld) onlyWorld->push_back((o->flags & FLAG_ONLYLIGHTWORLD) != 0);
    }
    return out;
}

// Poses every visible model object and hands its triangles to the renderer.
// The level's ambient light ("AmbientLight r g b" in the world info), 0..1.
Vec3 worldAmbient(const WorldData &w)
{
    size_t at = w.info.find("AmbientLight");
    float r = 0, g = 0, b = 0;
    if (at != std::string::npos && sscanf(w.info.c_str() + at, "AmbientLight %f %f %f", &r, &g, &b) == 3)
        return {r / 255.0f, g / 255.0f, b / 255.0f};
    return {0.5f, 0.5f, 0.5f};
}

// The baked light on the floor under p: a ray straight down to the first
// visible world polygon (world models where they are now), read from its
// lightmap or vertex light. Falls back to the level's ambient light.
Vec3 worldLightBelow(GameSession &g, const Vec3 &p)
{
    const WorldData *w = g.server.world.get();
    if (!w) return {1, 1, 1};
    DVector from(p.x, p.y + 1, p.z);
    for (int tries = 0; tries < 4; ++tries) {
        IntersectQuery q;
        q.m_From = from;
        q.m_To = DVector(p.x, p.y - 4000, p.z);
        q.m_Flags = IGNORE_NONSOLID | INTERSECT_HPOLY;
        IntersectInfo info;
        memset(&info, 0, sizeof(info));
        if (!intersectSegment(g.server, &q, &info, true) || info.m_hPoly == INVALID_HPOLY) break;
        size_t model = info.m_hPoly >> 24, poly = info.m_hPoly & 0xFFFFFF;
        if (model >= w->models.size() || poly >= w->models[model].polys.size()) break;
        const WorldBsp &bsp = w->models[model];
        DDWORD sflags = bsp.surfaces[bsp.polys[poly].surface].flags;
        if (sflags & (kSurfInvisible | kSurfSky)) {
            // Hull brushes and sky portals carry no light; look past them.
            from = DVector(info.m_Point.x, info.m_Point.y - 1, info.m_Point.z);
            continue;
        }
        Vec3 hit{info.m_Point.x, info.m_Point.y, info.m_Point.z};
        if (model > 0)
            if (EngineObject *o = toObject(info.m_hObject); o && o->bsp == int(model)) {
                // Back to where the polygon was built: base + R^-1 (hit - pos).
                const DRotation &b = o->baseRot;
                DRotation r = quatMul(o->rot, DRotation(-b.m_Vec.x, -b.m_Vec.y, -b.m_Vec.z, b.m_Spin));
                DRotation inv(-r.m_Vec.x, -r.m_Vec.y, -r.m_Vec.z, r.m_Spin);
                DVector d = quatRotate(quatNormalize(inv), DVector(hit.x - o->pos.x, hit.y - o->pos.y, hit.z - o->pos.z));
                hit = {o->basePos.x + d.x, o->basePos.y + d.y, o->basePos.z + d.z};
            }
        return bsp.lightAt(poly, hit);
    }
    return worldAmbient(*w);
}

// Models take the world's light where they stand (worldLightBelow, smoothed
// over a few frames), shaded by a fixed direction so they keep their shape,
// plus dynamic lights; tinted by the object's color.
void submitModels(GameSession &g)
{
    static const bool noModels = getenv("SHOGO_NOMODELS") != nullptr;
    if (noModels) return;
    updateAttachments();
    std::vector<Vec3> pos, nrm;
    const Vec3 light = normalized(Vec3{0.3f, 1.0f, -0.4f});
    static const bool fixedLight = getenv("SHOGO_NOMODELWORLDLIGHT") != nullptr;
    std::vector<bool> onlyWorld;
    const std::vector<WorldRenderer::Light> lights = gatherLights(&onlyWorld);
    std::vector<const WorldRenderer::Light *> near;
    for (EngineObject *o : objects().all()) {
        if (o->removed || o->type != OT_MODEL || !(o->flags & FLAG_VISIBLE)) continue;
        const ModelInfo *m = objectModel(o);
        if (!m || m->tris.empty()) continue;
        poseModel(*m, o->anim, o->animTime, nullptr, &pos, &nrm);
        int tw = 1, th = 1;
        int tex = g.renderer.texture(o->skin, &tw, &th);
        static const bool traceModels = getenv("SHOGO_TRACE_MODELS") != nullptr;
        if (traceModels && !o->traced) {
            o->traced = true;
            fprintf(stderr, "[model] %s skin %s (tex %d) color %.2f %.2f %.2f %.2f flags %x\n", o->filename.c_str(),
                    o->skin.c_str(), tex, o->color[0], o->color[1], o->color[2], o->color[3], o->flags);
        }
        DVector sc = o->scale;
        if (sc.x == 0 && sc.y == 0 && sc.z == 0) sc = DVector(1, 1, 1);
        const bool bright = (o->flags & FLAG_NOLIGHT) != 0;
        // Props pass their "TintColor" (default black) as the object color, so
        // black means untinted; anything else multiplies.
        float tint[3] = {o->color[0], o->color[1], o->color[2]};
        if (tint[0] == 0 && tint[1] == 0 && tint[2] == 0) tint[0] = tint[1] = tint[2] = 1;
        const bool close = (o->flags & FLAG_REALLYCLOSE) != 0;
        for (size_t i = 0; i < pos.size(); ++i) {
            DVector p = quatRotate(o->rot, DVector(pos[i].x * sc.x, pos[i].y * sc.y, pos[i].z * sc.z));
            pos[i] = {o->pos.x + p.x, o->pos.y + p.y, o->pos.z + p.z};
            DVector n = quatRotate(o->rot, DVector(nrm[i].x, nrm[i].y, nrm[i].z));
            nrm[i] = {n.x, n.y, n.z};
        }
        // Dynamic lights that reach this model.
        near.clear();
        if (!bright) {
            float reach = std::max({o->dims.x, o->dims.y, o->dims.z, 1.0f}) * 1.5f;
            for (size_t li = 0; li < lights.size(); ++li) {
                if (onlyWorld[li]) continue;
                Vec3 d = lights[li].pos - Vec3{o->pos.x, o->pos.y, o->pos.z};
                float rr = lights[li].radius + reach;
                if (dot(d, d) < rr * rr) near.push_back(&lights[li]);
            }
        }
        Vec3 base{1, 1, 1};  // world light where the model stands
        if (!bright && !fixedLight) {
            Vec3 target = worldLightBelow(g, {o->pos.x, o->pos.y, o->pos.z});
            // The floor's color is a strong cue on a floor and too strong on a model:
            // keep its brightness, half its tint.
            float grey = (target.x + target.y + target.z) / 3.0f;
            target = {grey + (target.x - grey) * 0.5f, grey + (target.y - grey) * 0.5f, grey + (target.z - grey) * 0.5f};
            float *wl = o->worldLight;
            if (wl[0] < 0) {
                wl[0] = target.x;
                wl[1] = target.y;
                wl[2] = target.z;
            } else {
                const float k = 0.25f;  // per frame: crossing a light pool fades over a few frames
                wl[0] += (target.x - wl[0]) * k;
                wl[1] += (target.y - wl[1]) * k;
                wl[2] += (target.z - wl[2]) * k;
            }
            base = {wl[0], wl[1], wl[2]};
            static const bool traceLight = getenv("SHOGO_TRACE_MODELLIGHT") != nullptr;
            if (traceLight && (o->flags & FLAG_REALLYCLOSE))
                fprintf(stderr, "[modellight] %s at (%.0f %.0f %.0f): %.2f %.2f %.2f\n", o->filename.c_str(), o->pos.x,
                        o->pos.y, o->pos.z, target.x, target.y, target.z);
        }
        const Vec3 scale3{g_lightScale.x * base.x, g_lightScale.y * base.y, g_lightScale.z * base.z};
        const bool anyHidden = std::find(o->hiddenNodes.begin(), o->hiddenNodes.end(), true) != o->hiddenNodes.end();
        for (const ModelTri &t : m->tris) {
            if (anyHidden) {
                bool hide = false;
                for (uint16_t vi : t.v) {
                    size_t node = (size_t)m->verts[vi].node;
                    if (node < o->hiddenNodes.size() && o->hiddenNodes[node]) hide = true;
                }
                if (hide) continue;
            }
            WorldRenderer::DrawPoly dp;
            dp.tex = tex;
            if (o->color[3] < 0.99f) {
                dp.blend = WorldRenderer::Alpha;
                dp.alpha = o->color[3];
            }
            for (int k = 0; k < 3; ++k) {
                WorldRenderer::DrawVert v;
                v.p = pos[t.v[k]];
                v.u = t.uv[k * 2] * (float)tw;
                v.v = t.uv[k * 2 + 1] * (float)th;
                // Directional shading: the lit side a little over the world light, the dark side under.
                float shade = 0.45f + 0.55f * std::max(0.0f, dot(nrm[t.v[k]], light));
                float l = bright ? 1.0f : fixedLight ? shade : shade / 0.85f;
                float lr = bright ? l : l * scale3.x, lg = bright ? l : l * scale3.y, lb = bright ? l : l * scale3.z;
                for (const WorldRenderer::Light *L : near) {
                    Vec3 d = L->pos - v.p;
                    float dist = length(d);
                    if (dist >= L->radius) continue;
                    // Distance falloff, with some wrap so the dark side isn't black.
                    float nl = dist > 0.001f ? dot(nrm[t.v[k]], d * (1.0f / dist)) : 1.0f;
                    float f = (1.0f - dist / L->radius) * std::clamp(0.35f + 0.65f * nl, 0.0f, 1.0f);
                    lr += L->r * f;
                    lg += L->g * f;
                    lb += L->b * f;
                }
                v.r = lr * tint[0];
                v.g = lg * tint[1];
                v.b = lb * tint[2];
                dp.verts.push_back(v);
            }
            g.renderer.addDynamic(std::move(dp), close);
        }
    }
}

// A poly grid as triangles: a height field in object space (x and z from -1
// to 1, height = value / 127 in y), scaled, rotated and moved by the object.
// Each vertex takes its color from the grid's table (value + 128) and the
// object's color. The texture (a sprite) is laid out in world units: one
// texel per xScale / yScale units along the grid, starting at its corner,
// offset by the pan. That's what the levels' data implies: the same flag
// sprite on a grid half the size uses half the scale.
void submitPolyGrid(GameSession &g, const EngineObject *o)
{
    if (o->grid.empty() || o->gridW < 2 || o->gridH < 2 || o->gridColors.size() < 256) return;
    int tw = 1, th = 1;
    int tex = o->gridTexture.empty() ? -1 : g.renderer.texture(o->gridTexture, &tw, &th);
    DVector u, r, f;
    quatVectors(o->rot, &u, &r, &f);
    const DVector sc = o->scale;
    const float xs = o->gridScale[0] != 0 ? o->gridScale[0] : 1, ys = o->gridScale[1] != 0 ? o->gridScale[1] : 1;
    const uint32_t W = o->gridW, H = o->gridH;
    std::vector<WorldRenderer::DrawVert> verts(size_t(W) * H);
    for (uint32_t j = 0; j < H; ++j)
        for (uint32_t i = 0; i < W; ++i) {
            int value = (signed char)o->grid[size_t(j) * W + i];
            float lx = -1 + 2.0f * float(i) / float(W - 1), lz = -1 + 2.0f * float(j) / float(H - 1);
            float ly = float(value) / 127.0f;
            DVector p = o->pos + r * (lx * sc.x) + u * (ly * sc.y) + f * (lz * sc.z);
            const PGColor &c = o->gridColors[size_t(uint8_t(value + 128))];
            WorldRenderer::DrawVert &v = verts[size_t(j) * W + i];
            v.p = {p.x, p.y, p.z};
            v.u = (lx + 1) * sc.x / xs + o->gridPan[0];
            v.v = (lz + 1) * sc.z / ys + o->gridPan[1];
            v.r = c.x / 255.0f * o->color[0];
            v.g = c.y / 255.0f * o->color[1];
            v.b = c.z / 255.0f * o->color[2];
        }
    const float alpha = o->color[3];
    for (uint32_t j = 0; j + 1 < H; ++j)
        for (uint32_t i = 0; i + 1 < W; ++i) {
            const size_t a = size_t(j) * W + i, b = a + 1, c = a + W, d = c + 1;
            const size_t tris[2][3] = {{a, b, d}, {a, d, c}};
            for (const auto &t : tris) {
                WorldRenderer::DrawPoly dp;
                dp.tex = tex;
                dp.alpha = alpha;
                dp.blend = alpha < 0.999f ? WorldRenderer::Alpha : WorldRenderer::Opaque;
                for (size_t k : t) dp.verts.push_back(verts[k]);
                g.renderer.addDynamic(std::move(dp), false);
            }
        }
}

void submitPolyGrids(GameSession &g)
{
    for (EngineObject *o : objects().all())
        if (!o->removed && o->type == OT_POLYGRID && (o->flags & FLAG_VISIBLE)) submitPolyGrid(g, o);
}

Camera cameraFor(const EngineObject *cam)
{
    Camera c;
    c.pos = {cam->pos.x, cam->pos.y, cam->pos.z};
    c.fovX = cam->fovX;
    if (!cam->camFullscreen) c.rect = DRect{cam->camRect[0], cam->camRect[1], cam->camRect[2], cam->camRect[3]};
    DVector u, r, f;
    quatVectors(cam->rot, &u, &r, &f);
    c.hasBasis = true;
    c.right = {r.x, r.y, r.z};
    c.up = {u.x, u.y, u.z};
    c.forward = {f.x, f.y, f.z};
    return c;
}

DRESULT renderCamera(HLOCALOBJ h)
{
    GameSession &g = G();
    EngineObject *cam = O(h);
    if (!cam || !g.ctx->screen) return LT_ERROR;
    if (!g.server.world) return LT_OK;
    if (g.renderedWorld != g.server.world.get()) {
        g.renderer.load(*g.server.world, *g.ctx->resources);
        g.renderedWorld = g.server.world.get();
    }
    Camera c = cameraFor(cam);
    submitModels(g);
    submitPolyGrids(g);
    submitEffects(g.renderer, *g.ctx->resources, c.pos, c.right, c.up);
    g.renderer.setLights(gatherLights());
    g.renderer.setModelPlacements(modelPlacements());
    soundCameraMoved(c.pos, c.right);
    g.renderer.setLightScale(g_lightScale.x, g_lightScale.y, g_lightScale.z);
    g.renderer.setScreenAdd(cam->lightAdd.x, cam->lightAdd.y, cam->lightAdd.z);
    g.renderer.render(*g.ctx->screen, c);
    static const bool traceCamera = getenv("SHOGO_TRACE_CAMERA") != nullptr;
    static int cameraFrames = 0;
    if (traceCamera && cameraFrames++ % 30 == 0)
        fprintf(stderr, "[camera] pos (%.0f %.0f %.0f) light scale %.2f %.2f %.2f, %zu polygons drawn\n", c.pos.x, c.pos.y,
                c.pos.z, g_lightScale.x, g_lightScale.y, g_lightScale.z, g.renderer.stats().drawnPolys);
    static const bool traceLights = getenv("SHOGO_TRACE_LIGHTS") != nullptr;
    if (traceLights) {
        for (EngineObject *o : objects().all())
            if (!o->removed && o->type == OT_LIGHT && !o->traced) {
                o->traced = true;
                fprintf(stderr, "[light] id=%u pos=(%.0f,%.0f,%.0f) radius %.0f color %.2f %.2f %.2f flags %x\n", o->id,
                        o->pos.x, o->pos.y, o->pos.z, o->lightRadius, o->lightColor[0], o->lightColor[1],
                        o->lightColor[2], o->flags);
            }
        static int frame = 0;
        if (++frame % 10 == 0)
            fprintf(stderr, "[light] frame %d: %zu lit polys, scale %.2f %.2f %.2f, screen add %.2f %.2f %.2f\n", frame,
                    g.renderer.stats().litPolys, g_lightScale.x, g_lightScale.y, g_lightScale.z, cam->lightAdd.x,
                    cam->lightAdd.y, cam->lightAdd.z);
    }
    return LT_OK;
}
// ---- collision queries -----------------------------------------------------------------------

DBOOL clientIntersectSegment(ClientIntersectQuery *q, ClientIntersectInfo *info)
{
    return intersectSegment(G().server, q, info, true) ? DTRUE : DFALSE;
}
DBOOL clientCastRay(ClientIntersectQuery *q, ClientIntersectInfo *info)
{
    if (!q) return DFALSE;
    IntersectQuery ray = *q;
    DVector d = q->m_Direction;
    float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (l <= 0) return DFALSE;
    float far = 100000.0f / l;
    ray.m_To = DVector(q->m_From.x + d.x * far, q->m_From.y + d.y * far, q->m_From.z + d.z * far);
    return intersectSegment(G().server, &ray, info, true) ? DTRUE : DFALSE;
}
// GetPointStatus is a virtual method on ClientDE.
DRESULT vClientPointStatus(void *, DVector *p) { return pointStatus(p); }

DBOOL clientContainerCode(HLOCALOBJ h, D_WORD *code)
{
    EngineObject *o = O(h);
    if (!o || o->type != OT_CONTAINER) return DFALSE;
    if (code) *code = o->containerCode;
    return DTRUE;
}

// Objects whose centre is within radius (client and server objects alike).
DRESULT clientFindObjectsInSphere(DVector *center, float radius, HLOCALOBJ *list, DDWORD max, DDWORD *nOut,
                                  DDWORD *nFound)
{
    DDWORD found = 0, out = 0;
    if (center) {
        const float r2 = radius * radius;
        for (EngineObject *o : objects().all()) {
            if (o->removed) continue;
            float dx = o->pos.x - center->x, dy = o->pos.y - center->y, dz = o->pos.z - center->z;
            if (dx * dx + dy * dy + dz * dz > r2) continue;
            if (list && out < max) list[out++] = toHLocal(o);
            ++found;
        }
    }
    if (nOut) *nOut = out;
    if (nFound) *nFound = found;
    return LT_OK;
}

DDWORD clientPointContainers(DVector *p, HLOCALOBJ *list, DDWORD max)
{
    if (!p || !G().server.world) return 0;
    std::vector<EngineObject *> found(max ? max : 1);
    int n = pointContainers(G().server, *p, found.data(), int(max));
    for (int i = 0; i < n; ++i)
        if (list) list[i] = toHLocal(found[size_t(i)]);
    return (DDWORD)n;
}

// ---- poly grids ---------------------------------------------------------------------------

DBOOL setupPolyGrid(HLOCALOBJ h, DDWORD w, DDWORD hgt, DBOOL)
{
    EngineObject *o = O(h);
    if (!o || !w || !hgt) return DFALSE;
    o->gridW = w;
    o->gridH = hgt;
    o->grid.assign(size_t(w) * hgt, 0);
    o->gridColors.assign(256, PGColor{});
    return DTRUE;
}
DRESULT getPolyGridInfo(HLOCALOBJ h, char **bytes, DDWORD *w, DDWORD *hgt, PGColor **colors)
{
    EngineObject *o = O(h);
    if (!o || o->grid.empty()) return LT_ERROR;
    if (bytes) *bytes = o->grid.data();
    if (w) *w = o->gridW;
    if (hgt) *hgt = o->gridH;
    if (colors) *colors = o->gridColors.data();
    return LT_OK;
}
DRESULT setPolyGridTexture(HLOCALOBJ h, char *file)
{
    EngineObject *o = O(h);
    if (!o) return LT_ERROR;
    o->gridTexture = file ? file : "";
    return LT_OK;
}
DRESULT setPolyGridEnvMap(HLOCALOBJ h, char *) { return O(h) ? LT_OK : LT_ERROR; }
DRESULT getPolyGridTextureInfo(HLOCALOBJ h, float *px, float *py, float *sx, float *sy)
{
    EngineObject *o = O(h);
    if (!o) return LT_ERROR;
    if (px) *px = o->gridPan[0];
    if (py) *py = o->gridPan[1];
    if (sx) *sx = o->gridScale[0];
    if (sy) *sy = o->gridScale[1];
    return LT_OK;
}
DRESULT setPolyGridTextureInfo(HLOCALOBJ h, float px, float py, float sx, float sy)
{
    EngineObject *o = O(h);
    if (!o) return LT_ERROR;
    o->gridPan[0] = px;
    o->gridPan[1] = py;
    o->gridScale[0] = sx;
    o->gridScale[1] = sy;
    return LT_OK;
}
DRESULT fitPolyGrid(HLOCALOBJ h, DVector *mn, DVector *mx, DVector *pos, DVector *scale)
{
    // The grid spans -1..1 in X and Z in object space (height in Y).
    EngineObject *o = O(h);
    if (!o || !mn || !mx) return LT_ERROR;
    if (pos) *pos = DVector((mn->x + mx->x) / 2, (mn->y + mx->y) / 2, (mn->z + mx->z) / 2);
    if (scale) *scale = DVector((mx->x - mn->x) / 2, (mx->y - mn->y) / 2, (mx->z - mn->z) / 2);
    return LT_OK;
}

// Draws just these objects (poly grids: the menu background) with the camera,
// over what the screen already shows.
DRESULT renderObjects(HLOCALOBJ hCam, HLOCALOBJ *list, int n)
{
    GameSession &g = G();
    EngineObject *cam = O(hCam);
    if (!cam || !g.ctx->screen || !list) return LT_ERROR;
    if (!g.renderedWorld) g.renderer.setResources(*g.ctx->resources);
    for (int i = 0; i < n; ++i) {
        EngineObject *o = O(list[i]);
        if (o && !o->removed && o->type == OT_POLYGRID && (o->flags & FLAG_VISIBLE)) {
            submitPolyGrid(g, o);
            o->flags |= FLAG_WASDRAWN;
        }
    }
    g.renderer.renderDynamic(*g.ctx->screen, cameraFor(cam));
    return LT_OK;
}

#define HOOK(name, fn) shogo_set_virtual_hook("ClientDE::" name, (void *)&fn)
}  // namespace

void GameSession::installClient(ClientDE *c, ClientContext *context)
{
    instance = this;
    de = c;
    ctx = context;
    server.vars = &vars;
    server.removedCallback = [this](EngineObject *o) {
        if (shell && shell->OnObjectRemove) shell->OnObjectRemove(shell, toHLocal(o));
    };
    server.onClientEnteredWorld = [this] { enteredWorld = true; };
    server.onClientLeavingWorld = [this] {
        enteredWorld = false;
        if (shell && shell->OnExitWorld) shell->OnExitWorld(shell);
    };
    server.commandOn = [this](int c) { return input.isCommandOn(c); };
    vars.commands = [this](const std::vector<std::string> &a) { return input.command(a); };

    c->ReadConfigFile = apiReadConfigFile;
    c->WriteConfigFile = apiWriteConfigFile;
    c->IsCommandOn = isCommandOn;
    c->GetAxisOffsets = getAxisOffsets;
    c->SetInputState = setInputState;
    c->ClearInput = clearInput;
    c->GetDeviceName = getDeviceName;
    c->IsDeviceEnabled = isDeviceEnabled;
    c->GetDeviceBindings = getDeviceBindings;
    c->FreeDeviceBindings = freeDeviceBindings;
    c->GetDeviceObjects = getDeviceObjects;
    c->FreeDeviceObjects = freeDeviceObjects;
    c->StartDeviceTrack = startDeviceTrack;
    c->TrackDevice = trackDevice;
    c->EndDeviceTrack = endDeviceTrack;
    c->StartGame = startGame;
    c->GetGameMode = getGameMode;
    c->GetLocalClientID = getLocalClientID;
    c->IsConnected = isConnected;
    c->Disconnect = disconnect;
    c->GetWorldInfoString = getWorldInfoString;
    c->GetConsoleVar = getConsoleVar;
    c->GetVarValueFloat = getVarValueFloat;
    c->GetVarValueString = getVarValueString;
    c->RunConsoleString = runConsoleString;
    c->GetClientObject = getClientObject;
    c->CreateObject = createObject;
    c->DeleteObject = deleteObject;
    c->GetObjectPos = getObjectPos;
    c->SetObjectPos = setObjectPos;
    c->GetObjectRotation = getObjectRotation;
    c->SetObjectRotation = setObjectRotation;
    c->SetObjectPosAndRotation = setObjectPosAndRotation;
    c->GetObjectType = getObjectType;
    c->GetObjectScale = getObjectScale;
    c->SetObjectScale = setObjectScale;
    c->GetObjectColor = getObjectColor;
    c->SetObjectColor = setObjectColor;
    c->GetObjectFlags = getObjectFlags;
    c->SetObjectFlags = setObjectFlags;
    c->GetObjectUserFlags = getObjectUserFlags;
    c->SetObjectUserFlags = setObjectUserFlags;
    c->GetObjectClientFlags = getObjectClientFlags;
    c->SetObjectClientFlags = setObjectClientFlags;
    c->GetObjectUserData = getObjectUserData;
    c->SetObjectUserData = setObjectUserData;
    c->GetCameraFOV = getCameraFOV;
    c->SetCameraFOV = setCameraFOV;
    c->GetCameraRect = getCameraRect;
    c->SetCameraRect = setCameraRect;
    c->GetLightColor = getLightColorC;
    c->SetLightColor = setLightColorC;
    c->GetLightRadius = getLightRadiusC;
    c->SetLightRadius = setLightRadiusC;
    c->GetGlobalLightScale = getGlobalLightScale;
    c->SetGlobalLightScale = setGlobalLightScale;
    c->OffsetGlobalLightScale = offsetGlobalLightScale;
    c->GetCameraLightAdd = getCameraLightAdd;
    c->SetCameraLightAdd = setCameraLightAdd;
    c->RenderCamera = renderCamera;
    c->RenderObjects = renderObjects;
    c->Parse = parseCommand;
    c->IntersectSegment = clientIntersectSegment;
    c->CastRay = clientCastRay;
    c->GetPointContainers = clientPointContainers;
    c->GetContainerCode = clientContainerCode;
    c->FindObjectsInSphere = clientFindObjectsInSphere;
    shogo_set_virtual_hook("ClientDE::GetPointStatus", (void *)&vClientPointStatus);
    c->SetupPolyGrid = setupPolyGrid;
    c->GetPolyGridInfo = getPolyGridInfo;
    c->SetPolyGridTexture = setPolyGridTexture;
    c->SetPolyGridEnvMap = setPolyGridEnvMap;
    c->GetPolyGridTextureInfo = getPolyGridTextureInfo;
    c->SetPolyGridTextureInfo = setPolyGridTextureInfo;
    c->FitPolyGrid = fitPolyGrid;
    c->RotateAroundAxis = rotateAroundAxis;
    c->EulerRotateX = eulerRotateX;
    c->EulerRotateY = eulerRotateY;
    c->EulerRotateZ = eulerRotateZ;
    c->AlignRotation = alignRotation;
    c->InterpolateRotation = interpolateRotation;

    HOOK("StartMessage", vStartMessage);
    HOOK("EndMessage", vEndMessage);
    HOOK("EndMessage2", vEndMessage2);
    HOOK("SetupEuler", vSetupEuler);
    HOOK("GetRotationVectors", vGetRotationVectors);
    HOOK("GetSConValueFloat", vGetSConValueFloat);
    HOOK("GetSConValueString", vGetSConValueString);
    HOOK("GetServerConVarValueFloat", vGetServerConVarValueFloat);
    HOOK("GetServerConVarValueString", vGetServerConVarValueString);
}

bool GameSession::readConfigFile(const std::string &name)
{
    FILE *f = nullptr;
    for (const std::string &path : {name, "__support/app/" + name}) {
        // Case-insensitive match within the directory.
        std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
        std::string file = path.substr(dir.size());
        if (DIR *d = opendir(dir.empty() ? "." : dir.c_str())) {
            while (dirent *e = readdir(d))
                if (strcasecmp(e->d_name, file.c_str()) == 0) {
                    f = fopen((dir + e->d_name).c_str(), "rb");
                    break;
                }
            closedir(d);
        }
        if (f) break;
    }
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) vars.run(line);
    fclose(f);
    return true;
}

void GameSession::runCommandLine(const std::vector<std::string> &commands)
{
    for (const std::string &c : commands) {
        std::string name = c.substr(0, c.find_first_of(" \t"));
        if (!name.empty() && name[0] == '+') name.erase(0, 1);
        for (char &ch : name) ch = (char)tolower((unsigned char)ch);
        if (!commandLineVars.count(name)) {
            ConsoleVars::Var *v = vars.find(name);
            commandLineVars[name] = v ? std::optional<std::string>(v->value) : std::nullopt;
        }
        vars.run(c);
    }
}

bool GameSession::writeConfigFile(const std::string &name)
{
    // Write over the file the game folder has, whatever its case.
    std::string path = name;
    if (DIR *d = opendir(".")) {
        while (dirent *e = readdir(d))
            if (strcasecmp(e->d_name, name.c_str()) == 0) path = e->d_name;
        closedir(d);
    }
    auto quoted = [](const std::string &s) { return "\"" + s + "\""; };
    std::string text;
    for (const auto &[key, v] : vars.all()) {
        std::string value = v->value;
        auto cl = commandLineVars.find(key);
        if (cl != commandLineVars.end()) {
            if (!cl->second) continue;
            value = *cl->second;
        } else if (!v->args.empty()) {  // "ModelAdd 0 0 0"
            text += v->name;
            for (const std::string &a : v->args) text += " " + a;
            text += "\n";
            continue;
        }
        text += quoted(v->name) + " " + quoted(value) + "\n";
    }
    text += input.configLines();

    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

void GameSession::update(double dt)
{
    if (!pendingWorld.empty()) {
        std::string name = std::move(pendingWorld);
        pendingWorld.clear();
        server.loadWorld(name.c_str(), LOADWORLD_LOADWORLDOBJECTS | LOADWORLD_RUNWORLD);
    }
    server.update(dt);

    if (enteredWorld) {
        enteredWorld = false;
        if (shell && shell->PreLoadWorld) {
            // As the game names worlds: "Worlds\02_Quarters" (it keeps what follows the last '\').
            std::string gameName = server.worldName;
            for (char &c : gameName)
                if (c == '/') c = '\\';
            if (gameName.size() > 4 && strcasecmp(gameName.c_str() + gameName.size() - 4, ".dat") == 0)
                gameName.resize(gameName.size() - 4);
            shell->PreLoadWorld(shell, gameName.data());
        }
        if (shell && shell->OnEnterWorld) shell->OnEnterWorld(shell);
    }
    if (!shell) return;

    // Command edges, for the shells' OnCommandOn / OnCommandOff.
    std::vector<int> now = input.activeCommands();
    for (int c : now)
        if (std::find(commandsOn.begin(), commandsOn.end(), c) == commandsOn.end()) {
            if (shell->OnCommandOn) shell->OnCommandOn(shell, c);
            if (server.worldRunning && server.shell && server.shell->OnCommandOn)
                server.shell->OnCommandOn(server.shell, reinterpret_cast<HCLIENT>(&server.client), c);
        }
    for (int c : commandsOn)
        if (std::find(now.begin(), now.end(), c) == now.end()) {
            if (shell->OnCommandOff) shell->OnCommandOff(shell, c);
            if (server.worldRunning && server.shell && server.shell->OnCommandOff)
                server.shell->OnCommandOff(server.shell, reinterpret_cast<HCLIENT>(&server.client), c);
        }
    commandsOn = std::move(now);

    // Server -> client messages, in order.
    while (!server.toClient.empty()) {
        PendingMessage pm = std::move(server.toClient.front());
        server.toClient.pop_front();
        static bool traceMsgs = getenv("SHOGO_TRACE_MESSAGES") != nullptr;
        if (traceMsgs) {
            printf("[message] server -> client %d (%zu bytes)", pm.id, pm.msg->data.size());
            for (size_t i = 0; i < pm.msg->data.size() && i < 200; ++i) printf(" %02x", pm.msg->data[i]);
            printf("\n");
        }
        if (shell->OnMessage) shell->OnMessage(shell, pm.id, toReadHandle(pm.msg.get()));
        pm.msg->magic = 0;
    }
    // Objects with new special-effect messages.
    for (EngineObject *o : std::vector<EngineObject *>(objects().all())) {
        if (o->clientOnly || o->removed || o->sfxSent || o->sfx.empty()) continue;
        o->sfxSent = true;
        o->clientKnown = true;
        Message m;
        m.data = o->sfx;
        if (shell->SpecialEffectNotify) shell->SpecialEffectNotify(shell, toHLocal(o), toReadHandle(&m));
        m.magic = 0;
    }
    while (!server.instantEffects.empty()) {
        std::unique_ptr<Message> m = std::move(server.instantEffects.front());
        server.instantEffects.pop_front();
        static const bool traceFx = getenv("SHOGO_TRACE_FX") != nullptr;
        if (traceFx) fprintf(stderr, "[fx] instant effect, %zu bytes, first byte %d\n", m->data.size(), m->data.empty() ? -1 : m->data[0]);
        if (shell->SpecialEffectNotify) shell->SpecialEffectNotify(shell, nullptr, toReadHandle(m.get()));
        m->magic = 0;
    }

    updateEffects((float)dt);

    // Client-side model animations (the player's movement object, weapon models).
    for (EngineObject *o : std::vector<EngineObject *>(objects().all())) {
        if (!o->clientOnly || o->removed || o->type != OT_MODEL || o->magic != EngineObject::kMagic) continue;
        animateObject(o, (float)dt, [this](EngineObject *obj, ArgList *args) {
            if (shell->OnModelKey) shell->OnModelKey(shell, toHLocal(obj), args);
        });
    }
}

}  // namespace shogo
