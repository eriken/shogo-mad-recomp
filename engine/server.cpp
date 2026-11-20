#include "engine/server.h"

#include "generic_msg_de.h"

#include "engine/api_common.h"
#include "engine/math3d.h"
#include "engine/model.h"
#include "engine/sound.h"
#include "engine/movement.h"
#include "engine/savegame.h"
#include "engine/stub_support.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <random>

namespace shogo {

Server *Server::instance = nullptr;

namespace {
Server &S() { return *Server::instance; }
EngineObject *O(HOBJECT h) { return toObject(h); }

std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string normalizeWorldName(const char *name)
{
    std::string s = name ? name : "";
    for (char &c : s)
        if (c == '\\') c = '/';
    if (s.size() < 4 || strcasecmp(s.c_str() + s.size() - 4, ".dat") != 0) s += ".dat";
    return s;
}

std::mt19937 &rng()
{
    static std::mt19937 r(1234);
    return r;
}

// ---- properties (valid during MID_PRECREATE) --------------------------------------------

const WorldProp *findProp(const char *name)
{
    if (!name) return nullptr;
    if (S().currentProps)
        if (const WorldProp *p = S().currentProps->find(name)) return p;
    for (const WorldProp &p : S().stringProps)
        if (strcasecmp(p.name.c_str(), name) == 0) return &p;
    return nullptr;
}

bool parseVec(const std::string &s, DVector *v)
{
    float x, y, z;
    if (sscanf(s.c_str(), "%f %f %f", &x, &y, &z) != 3) return false;
    *v = DVector(x, y, z);
    return true;
}

void fillGeneric(const WorldProp &p, GenericProp *g)
{
    memset(g, 0, sizeof(*g));
    g->m_Rotation = quatIdentity();
    char buf[64];
    switch (p.type) {
    case WorldProp::String: {
        snprintf(g->m_String, sizeof(g->m_String), "%s", p.string.c_str());
        parseVec(p.string, &g->m_Vec);
        g->m_Color = g->m_Vec;
        if (strcasecmp(p.string.c_str(), "true") == 0) g->m_Long = 1;
        else if (strcasecmp(p.string.c_str(), "false") == 0) g->m_Long = 0;
        else g->m_Long = atol(p.string.c_str());
        g->m_Float = (float)atof(p.string.c_str());
        if (strcasecmp(p.string.c_str(), "true") == 0) g->m_Float = 1;
        g->m_Bool = g->m_Long != 0 || g->m_Float != 0;
        break;
    }
    case WorldProp::Vector:
    case WorldProp::Color:
        g->m_Vec = DVector(p.vec[0], p.vec[1], p.vec[2]);
        g->m_Color = g->m_Vec;
        snprintf(g->m_String, sizeof(g->m_String), "%g %g %g", p.vec[0], p.vec[1], p.vec[2]);
        break;
    case WorldProp::Real:
    case WorldProp::LongInt:
        g->m_Float = p.vec[0];
        g->m_Long = (long)p.vec[0];
        g->m_Bool = p.vec[0] != 0;
        snprintf(buf, sizeof(buf), "%g", p.vec[0]);
        snprintf(g->m_String, sizeof(g->m_String), "%s", buf);
        break;
    case WorldProp::Flags:
    case WorldProp::Bool:
        g->m_Long = (long)p.bits;
        g->m_Float = (float)p.bits;
        g->m_Bool = p.bits != 0;
        snprintf(g->m_String, sizeof(g->m_String), "%u", p.bits);
        break;
    case WorldProp::Rotation:
        g->m_Rotation = quatEuler(p.vec[0], p.vec[1], p.vec[2]);
        g->m_Vec = DVector(p.vec[0], p.vec[1], p.vec[2]);
        break;
    }
}

DRESULT getPropGeneric(char *name, GenericProp *g)
{
    const WorldProp *p = findProp(name);
    if (!p || !g) return LT_ERROR;
    fillGeneric(*p, g);
    return LT_OK;
}

DRESULT getPropString(char *name, char *ret, int maxLen)
{
    GenericProp g;
    if (getPropGeneric(name, &g) != LT_OK || !ret || maxLen <= 0) return LT_ERROR;
    snprintf(ret, (size_t)maxLen, "%s", g.m_String);
    return LT_OK;
}
DRESULT getPropVector(char *name, DVector *ret)
{
    GenericProp g;
    if (getPropGeneric(name, &g) != LT_OK || !ret) return LT_ERROR;
    *ret = g.m_Vec;
    return LT_OK;
}
DRESULT getPropColor(char *name, DVector *ret)
{
    GenericProp g;
    if (getPropGeneric(name, &g) != LT_OK || !ret) return LT_ERROR;
    *ret = g.m_Color;
    return LT_OK;
}
DRESULT getPropReal(char *name, float *ret)
{
    GenericProp g;
    if (getPropGeneric(name, &g) != LT_OK || !ret) return LT_ERROR;
    *ret = g.m_Float;
    return LT_OK;
}
DRESULT getPropFlags(char *name, DDWORD *ret)
{
    GenericProp g;
    if (getPropGeneric(name, &g) != LT_OK || !ret) return LT_ERROR;
    *ret = (DDWORD)g.m_Long;
    return LT_OK;
}
DRESULT getPropBool(char *name, DBOOL *ret)
{
    GenericProp g;
    if (getPropGeneric(name, &g) != LT_OK || !ret) return LT_ERROR;
    *ret = g.m_Bool;
    return LT_OK;
}
DRESULT getPropLongInt(char *name, long *ret)
{
    GenericProp g;
    if (getPropGeneric(name, &g) != LT_OK || !ret) return LT_ERROR;
    *ret = g.m_Long;
    return LT_OK;
}
DRESULT getPropRotation(char *name, DRotation *ret)
{
    GenericProp g;
    if (getPropGeneric(name, &g) != LT_OK || !ret) return LT_ERROR;
    *ret = g.m_Rotation;
    return LT_OK;
}
DRESULT getPropRotationEuler(char *name, DVector *ret)
{
    const WorldProp *p = findProp(name);
    if (!p || !ret || p->type != WorldProp::Rotation) return LT_ERROR;
    *ret = DVector(p->vec[0], p->vec[1], p->vec[2]);
    return LT_OK;
}
DRESULT doesPropExist(char *name, int *type)
{
    const WorldProp *p = findProp(name);
    if (!p) return LT_ERROR;
    if (type) *type = p->type;
    return LT_OK;
}

// "Key value;Key value" as used by spawn strings.
std::vector<WorldProp> parsePropString(const char *s)
{
    std::vector<WorldProp> out;
    if (!s) return out;
    std::string all(s);
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find(';', start);
        if (end == std::string::npos) end = all.size();
        std::string item = all.substr(start, end - start);
        size_t a = item.find_first_not_of(" \t");
        if (a != std::string::npos) {
            item = item.substr(a);
            size_t sp = item.find_first_of(" \t");
            WorldProp p;
            p.type = WorldProp::String;
            p.name = item.substr(0, sp);
            if (sp != std::string::npos) {
                std::string v = item.substr(item.find_first_not_of(" \t", sp));
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
                p.string = v;
            }
            out.push_back(p);
        }
        start = end + 1;
    }
    return out;
}

// ---- classes -------------------------------------------------------------------------------

HCLASS getClass(char *name) { return reinterpret_cast<HCLASS>(S().findClass(name)); }
HCLASS getObjectClass(HOBJECT h)
{
    EngineObject *o = O(h);
    return o ? reinterpret_cast<HCLASS>(o->cls) : nullptr;
}
DBOOL isKindOf(HCLASS hClass, HCLASS hTest)
{
    auto *c = reinterpret_cast<ClassDef *>(hClass), *t = reinterpret_cast<ClassDef *>(hTest);
    if (!t) return DFALSE;
    for (; c; c = c->m_ParentClass)
        if (c == t) return DTRUE;
    return DFALSE;
}

LPBASECLASS apiCreateObject(HCLASS hClass, ObjectCreateStruct *ocs)
{
    EngineObject *o = S().createObject(reinterpret_cast<ClassDef *>(hClass), ocs, 0.0f, nullptr);
    return o ? o->game : nullptr;
}
LPBASECLASS createObjectProps(HCLASS hClass, ObjectCreateStruct *ocs, char *props)
{
    auto saved = std::move(S().stringProps);
    S().stringProps = parsePropString(props);
    EngineObject *o = S().createObject(reinterpret_cast<ClassDef *>(hClass), ocs, 2.0f, nullptr);
    S().stringProps = std::move(saved);
    return o ? o->game : nullptr;
}
void apiRemoveObject(HOBJECT h)
{
    if (EngineObject *o = O(h)) S().removeObject(o);
}
HOBJECT objectToHandle(LPBASECLASS p) { return p ? p->m_hObject : nullptr; }
LPBASECLASS handleToObject(HOBJECT h)
{
    EngineObject *o = O(h);
    return o ? o->game : nullptr;
}

// ---- time, random, console -------------------------------------------------------------------

DFLOAT getTime() { return (DFLOAT)S().time; }
DFLOAT getFrameTime() { return (DFLOAT)S().frameTime; }
DFLOAT randomRange(DFLOAT a, DFLOAT b)
{
    if (b <= a) return a;
    return std::uniform_real_distribution<float>(a, b)(rng());
}
int intRandom(int a, int b)
{
    if (b <= a) return a;
    return std::uniform_int_distribution<int>(a, b)(rng());
}
DFLOAT randomScale(DFLOAT s) { return randomRange(0, s); }

void printTagged(const char *tag, const char *fmt, va_list ap)
{
    printf("[%s] ", tag);
    vprintf(fmt, ap);
    printf("\n");
}
void bprint(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printTagged("server", fmt, ap);
    va_end(ap);
}
void debugOut(char *fmt, ...)
{
    if (!getenv("SHOGO_DEBUGOUT")) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void runGameConString(char *s) { if (s) S().vars->run(s); }
void setGameConVar(char *name, char *val)
{
    if (!name) return;
    S().vars->set(name, val ? val : "");
    S().gameVarNames.insert(name);
}
HCONVAR getGameConVar(char *name) { return reinterpret_cast<HCONVAR>(name ? S().vars->find(name) : nullptr); }
float getVarValueFloat(HCONVAR h) { auto *v = reinterpret_cast<ConsoleVars::Var *>(h); return v ? v->number : 0; }
char *getVarValueString(HCONVAR h)
{
    auto *v = reinterpret_cast<ConsoleVars::Var *>(h);
    return v ? v->value.data() : nullptr;
}

DRESULT getGameInfo(void **data, DDWORD *len)
{
    if (data) *data = S().gameInfo.empty() ? nullptr : S().gameInfo.data();
    if (len) *len = (DDWORD)S().gameInfo.size();
    return LT_OK;
}

// ---- object lists --------------------------------------------------------------------------

ObjectList *createObjectList()
{
    auto *l = new ObjectList;
    l->m_pFirstLink = nullptr;
    l->m_nInList = 0;
    return l;
}
ObjectLink *addObjectToList(ObjectList *l, HOBJECT h)
{
    if (!l) return nullptr;
    auto *link = new ObjectLink;
    link->m_hObject = h;
    link->m_pNext = nullptr;
    ObjectLink **tail = &l->m_pFirstLink;
    while (*tail) tail = &(*tail)->m_pNext;
    *tail = link;
    ++l->m_nInList;
    return link;
}
void removeObjectFromList(ObjectList *l, HOBJECT h)
{
    if (!l) return;
    for (ObjectLink **p = &l->m_pFirstLink; *p; p = &(*p)->m_pNext)
        if ((*p)->m_hObject == h) {
            ObjectLink *dead = *p;
            *p = dead->m_pNext;
            delete dead;
            --l->m_nInList;
            return;
        }
}
void relinquishList(ObjectList *l)
{
    if (!l) return;
    for (ObjectLink *p = l->m_pFirstLink; p;) {
        ObjectLink *n = p->m_pNext;
        delete p;
        p = n;
    }
    delete l;
}
ObjectList *findNamedObjects(char *name)
{
    ObjectList *l = createObjectList();
    if (!name || !name[0]) return l;
    for (EngineObject *o : objects().all())
        if (!o->clientOnly && !o->removed && strcasecmp(o->name.c_str(), name) == 0) addObjectToList(l, toHObject(o));
    return l;
}
ObjectList *findObjectsTouchingSphere(DVector *pos, float radius)
{
    ObjectList *l = createObjectList();
    if (!pos) return l;
    for (EngineObject *o : objects().all()) {
        if (o->clientOnly || o->removed) continue;
        float dx = o->pos.x - pos->x, dy = o->pos.y - pos->y, dz = o->pos.z - pos->z;
        float r = radius + std::max({o->dims.x, o->dims.y, o->dims.z});
        if (dx * dx + dy * dy + dz * dz <= r * r) addObjectToList(l, toHObject(o));
    }
    return l;
}
HOBJECT nextObject(HOBJECT h, bool active)
{
    const auto &all = objects().all();
    size_t i = 0;
    if (EngineObject *cur = O(h)) {
        auto it = std::find(all.begin(), all.end(), cur);
        i = it == all.end() ? all.size() : size_t(it - all.begin()) + 1;
    }
    for (; i < all.size(); ++i) {
        EngineObject *o = all[i];
        if (!o->clientOnly && !o->removed && o->game && o->active == active) return toHObject(o);
    }
    return nullptr;
}
HOBJECT getNextObject(HOBJECT h) { return nextObject(h, true); }
HOBJECT getNextInactiveObject(HOBJECT h) { return nextObject(h, false); }

// ---- object state --------------------------------------------------------------------------

char *getObjectName(HOBJECT h)
{
    EngineObject *o = O(h);
    return o ? o->name.data() : nullptr;
}
void setNextUpdate(HOBJECT h, DFLOAT t)
{
    if (EngineObject *o = O(h)) {
        o->nextUpdate = t > 0 ? S().time + t : 0;
        if (t > 0) o->active = true;
    }
}
void setDeactivationTime(HOBJECT h, DFLOAT t) { if (EngineObject *o = O(h)) o->deactivationTime = t; }
void pingObjects(HOBJECT) {}
void getObjectPos(HOBJECT h, DVector *p) { EngineObject *o = O(h); if (p) *p = o ? o->pos : DVector(0, 0, 0); }
void setObjectPos(HOBJECT h, DVector *p) { if (EngineObject *o = O(h); o && p) o->pos = *p; }
DRESULT teleportObject(HOBJECT h, DVector *p)
{
    EngineObject *o = O(h);
    if (!o || !p) return LT_ERROR;
    moveObject(S(), o, *p, true);
    return LT_OK;
}
void scaleObject(HOBJECT h, DVector *s) { if (EngineObject *o = O(h); o && s) o->scale = *s; }
DRESULT getObjectScale(HOBJECT h, DVector *s)
{
    EngineObject *o = O(h);
    if (!o || !s) return LT_ERROR;
    *s = o->scale;
    return LT_OK;
}
DRESULT getObjectRotation(HOBJECT h, DRotation *r)
{
    EngineObject *o = O(h);
    if (!r) return LT_ERROR;
    *r = o ? o->rot : quatIdentity();
    return o ? LT_OK : LT_ERROR;
}
DRESULT setObjectRotation(HOBJECT h, DRotation *r)
{
    EngineObject *o = O(h);
    if (!o || !r) return LT_ERROR;
    o->rot = *r;
    return LT_OK;
}
DDWORD getObjectFlags(HOBJECT h) { EngineObject *o = O(h); return o ? o->flags : 0; }
void setObjectFlags(HOBJECT h, DDWORD f) { if (EngineObject *o = O(h)) o->flags = f; }
DDWORD getObjectUserFlags(HOBJECT h) { EngineObject *o = O(h); return o ? o->userFlags : 0; }
DRESULT setObjectUserFlags(HOBJECT h, DDWORD f)
{
    EngineObject *o = O(h);
    if (!o) return LT_ERROR;
    o->userFlags = f;
    return LT_OK;
}
DBOOL getObjectColor(HOBJECT h, float *r, float *g, float *b, float *a)
{
    EngineObject *o = O(h);
    if (!o) return DFALSE;
    if (r) *r = o->color[0];
    if (g) *g = o->color[1];
    if (b) *b = o->color[2];
    if (a) *a = o->color[3];
    return DTRUE;
}
DBOOL setObjectColor(HOBJECT h, float r, float g, float b, float a)
{
    EngineObject *o = O(h);
    if (!o) return DFALSE;
    o->color[0] = r;
    o->color[1] = g;
    o->color[2] = b;
    o->color[3] = a;
    return DTRUE;
}
void setObjectState(HOBJECT h, int s)
{
    if (EngineObject *o = O(h)) {
        o->state = s;
        o->active = s == OBJSTATE_ACTIVE;
    }
}
int getObjectState(HOBJECT h) { EngineObject *o = O(h); return o ? o->state : 0; }
void getLightColor(HOBJECT h, float *r, float *g, float *b)
{
    EngineObject *o = O(h);
    if (r) *r = o ? o->lightColor[0] : 0;
    if (g) *g = o ? o->lightColor[1] : 0;
    if (b) *b = o ? o->lightColor[2] : 0;
}
void setLightColor(HOBJECT h, float r, float g, float b)
{
    if (EngineObject *o = O(h)) {
        o->lightColor[0] = r;
        o->lightColor[1] = g;
        o->lightColor[2] = b;
    }
}
float getLightRadius(HOBJECT h) { EngineObject *o = O(h); return o ? o->lightRadius : 0; }
void setLightRadius(HOBJECT h, float r) { if (EngineObject *o = O(h)) o->lightRadius = r; }
void setBlockingPriority(HOBJECT h, DBYTE p) { if (EngineObject *o = O(h)) o->blockingPriority = p; }
DBYTE getBlockingPriority(HOBJECT h) { EngineObject *o = O(h); return o ? o->blockingPriority : 0; }
DBOOL getContainerCode(HOBJECT h, D_WORD *code)
{
    EngineObject *o = O(h);
    if (!o || o->type != OT_CONTAINER) return DFALSE;
    if (code) *code = o->containerCode;
    return DTRUE;
}
DRESULT setObjectFilenames(HOBJECT h, char *file, char *skin)
{
    EngineObject *o = O(h);
    if (!o) return LT_ERROR;
    if (file) o->filename = file;
    if (skin) o->skin = skin;
    return LT_OK;
}
DBOOL getModelFilenames(HOBJECT h, char *file, int fileLen, char *skin, int skinLen)
{
    EngineObject *o = O(h);
    if (!o) return DFALSE;
    if (file && fileLen > 0) snprintf(file, (size_t)fileLen, "%s", o->filename.c_str());
    if (skin && skinLen > 0) snprintf(skin, (size_t)skinLen, "%s", o->skin.c_str());
    return DTRUE;
}

DRESULT createInterObjectLink(HOBJECT owner, HOBJECT linked)
{
    EngineObject *a = O(owner), *b = O(linked);
    if (!a || !b) return LT_ERROR;
    if (std::find(a->links.begin(), a->links.end(), b) == a->links.end()) a->links.push_back(b);
    return LT_OK;
}
void breakInterObjectLink(HOBJECT owner, HOBJECT linked)
{
    EngineObject *a = O(owner), *b = O(linked);
    if (a) a->links.erase(std::remove(a->links.begin(), a->links.end(), b), a->links.end());
}

// ---- virtual methods (first parameter is the ServerDE) -------------------------------------

HOBJECT vGetWorldObject(void *) { return S().worldObject ? toHObject(S().worldObject) : nullptr; }
DRESULT vGetWorldBox(void *, DVector &mn, DVector &mx)
{
    if (!S().world) return LT_ERROR;
    const WorldBsp &m = S().world->models[0];
    mn = DVector(m.boxMin.x, m.boxMin.y, m.boxMin.z);
    mx = DVector(m.boxMax.x, m.boxMax.y, m.boxMax.z);
    return LT_OK;
}
DRESULT vGetGlobalForce(void *, DVector *v) { if (v) *v = S().globalForce; return LT_OK; }
DRESULT vSetGlobalForce(void *, DVector *v) { if (v) S().globalForce = *v; return LT_OK; }
float vGetObjectMass(void *, HOBJECT h) { EngineObject *o = O(h); return o ? o->mass : 0; }
void vSetObjectMass(void *, HOBJECT h, float m) { if (EngineObject *o = O(h)) o->mass = m; }
float vGetForceIgnoreLimit(void *, HOBJECT h, float &l) { EngineObject *o = O(h); l = o ? o->forceIgnoreLimit : 0; return l; }
void vSetForceIgnoreLimit(void *, HOBJECT h, float l) { if (EngineObject *o = O(h)) o->forceIgnoreLimit = l; }
DRESULT vSetFriction(void *, HOBJECT h, float f) { if (EngineObject *o = O(h)) o->friction = f; return LT_OK; }
DRESULT vMoveObject(void *, HOBJECT h, DVector *p)
{
    EngineObject *o = O(h);
    if (!o || !p) return LT_ERROR;
    moveObject(S(), o, *p, false);
    return LT_OK;
}
DRESULT vGetPolyTextureFlags(void *, HPOLY poly, DDWORD *flags)
{
    if (flags) *flags = Server::instance ? polyTextureFlags(*Server::instance, poly) : 0;
    return LT_OK;
}
DRESULT vGetStandingOn(void *, HOBJECT h, CollisionInfo *info)
{
    return standingOn(S(), O(h), info) ? LT_OK : LT_ERROR;
}
DBOOL apiIntersectSegment(IntersectQuery *q, IntersectInfo *info)
{
    return intersectSegment(S(), q, info, false) ? DTRUE : DFALSE;
}
DBOOL apiCastRay(IntersectQuery *q, IntersectInfo *info)
{
    if (!q) return DFALSE;
    IntersectQuery ray = *q;
    DVector d = q->m_Direction;
    float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (l <= 0) return DFALSE;
    float far = 100000.0f / l;
    ray.m_To = DVector(q->m_From.x + d.x * far, q->m_From.y + d.y * far, q->m_From.z + d.z * far);
    return intersectSegment(S(), &ray, info, false) ? DTRUE : DFALSE;
}
DDWORD getPointContainers(DVector *p, HOBJECT *list, DDWORD max)
{
    if (!p || !S().world) return 0;
    std::vector<EngineObject *> found(max ? max : 1);
    int n = pointContainers(S(), *p, found.data(), int(max));
    for (int i = 0; i < n; ++i)
        if (list) list[i] = toHObject(found[size_t(i)]);
    return (DDWORD)n;
}
DDWORD getObjectContainers(HOBJECT h, HOBJECT *list, DDWORD *flags, DDWORD max)
{
    EngineObject *o = O(h);
    if (!o) return 0;
    DDWORD n = getPointContainers(&o->pos, list, max);
    if (flags)
        for (DDWORD i = 0; i < n; ++i) flags[i] = 0;
    return n;
}
DRESULT vGetNetFlags(void *, HOBJECT, DDWORD &f) { f = 0; return LT_OK; }
DRESULT vSetNetFlags(void *, HOBJECT, DDWORD) { return LT_OK; }
void vGetObjectDims(void *, HOBJECT h, DVector *d) { EngineObject *o = O(h); if (d) *d = o ? o->dims : DVector(0, 0, 0); }
DRESULT vSetObjectDims(void *, HOBJECT h, DVector *d)
{
    EngineObject *o = O(h);
    if (!o || !d) return LT_ERROR;
    o->dims = *d;
    return LT_OK;
}
DRESULT vGetVelocity(void *, HOBJECT h, DVector *v) { EngineObject *o = O(h); if (!o || !v) return LT_ERROR; *v = o->velocity; return LT_OK; }
DRESULT vSetVelocity(void *, HOBJECT h, DVector *v) { EngineObject *o = O(h); if (!o || !v) return LT_ERROR; o->velocity = *v; return LT_OK; }
DRESULT vGetAccel(void *, HOBJECT h, DVector *v) { EngineObject *o = O(h); if (!o || !v) return LT_ERROR; *v = o->accel; return LT_OK; }
DRESULT vSetAccel(void *, HOBJECT h, DVector *v) { EngineObject *o = O(h); if (!o || !v) return LT_ERROR; o->accel = *v; return LT_OK; }
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
DRESULT vGetClientPing(void *, HCLIENT, float &ping) { ping = 0; return LT_OK; }

// Messages from the server.
enum MsgKind { kToClient = 1, kSpecialEffect, kInstantEffect, kToObject, kToServerShell };

HMESSAGEWRITE vStartMessage(void *, HCLIENT, DBYTE id)
{
    auto *m = new Message;
    m->kind = kToClient;
    m->id = id;
    return toWriteHandle(m);
}
HMESSAGEWRITE vStartSpecialEffectMessage(void *, LPBASECLASS obj)
{
    auto *m = new Message;
    m->kind = kSpecialEffect;
    m->target = obj ? toObject(obj->m_hObject) : nullptr;
    return toWriteHandle(m);
}
HMESSAGEWRITE vStartInstantSpecialEffectMessage(void *, DVector *)
{
    auto *m = new Message;
    m->kind = kInstantEffect;
    return toWriteHandle(m);
}
HMESSAGEWRITE vStartMessageToObject(void *, LPBASECLASS sender, HOBJECT to, DDWORD id)
{
    if (!O(to)) return nullptr;
    auto *m = new Message;
    m->kind = kToObject;
    m->id = id;
    m->target = O(to);
    m->strings.push_back(std::to_string(sender ? objectId(toObject(sender->m_hObject)) : 0));
    return toWriteHandle(m);
}
DRESULT vStartMessageToServer(void *, LPBASECLASS sender, DDWORD id, HMESSAGEWRITE *out)
{
    auto *m = new Message;
    m->kind = kToServerShell;
    m->id = id;
    m->target = sender;
    if (out) *out = toWriteHandle(m);
    return LT_OK;
}
DRESULT vEndMessage2(void *, HMESSAGEWRITE h, DDWORD)
{
    Message *m = toMessage(h);
    if (!m) return LT_ERROR;
    std::unique_ptr<Message> owned(m);
    owned->pos = 0;
    switch (m->kind) {
    case kToClient: S().toClient.push_back({(uint8_t)m->id, std::move(owned)}); break;
    case kSpecialEffect:
        if (auto *o = static_cast<EngineObject *>(m->target); o && o->magic == EngineObject::kMagic) {
            o->sfx = m->data;
            o->sfxSent = false;
        }
        break;
    case kInstantEffect: S().instantEffects.push_back(std::move(owned)); break;
    case kToObject: {
        auto *to = static_cast<EngineObject *>(m->target);
        EngineObject *from = objectFromId((uint32_t)std::stoul(m->strings[0]));
        m->strings.clear();
        if (to && to->magic == EngineObject::kMagic) S().objectMessage(to, from, m->id, m);
        break;
    }
    case kToServerShell:
        if (S().shell && S().shell->OnObjectMessage)
            S().shell->OnObjectMessage(S().shell, static_cast<LPBASECLASS>(m->target), m->id, toReadHandle(m));
        break;
    }
    if (owned && (m->kind == kToObject || m->kind == kToServerShell || m->kind == kSpecialEffect)) m->magic = 0;
    return LT_OK;
}
DRESULT vEndMessage(void *self, HMESSAGEWRITE h) { return vEndMessage2(self, h, 0); }

// ---- clients ---------------------------------------------------------------------------------

ServerClient *C(HCLIENT h)
{
    auto *c = reinterpret_cast<ServerClient *>(h);
    return (c && c->magic == ServerClient::kMagic) ? c : nullptr;
}
HCLIENT getNextClient(HCLIENT prev) { return prev ? nullptr : reinterpret_cast<HCLIENT>(&S().client); }
DDWORD getClientID(HCLIENT h) { return C(h) ? 1 : 0; }
DBOOL getClientName(HCLIENT h, char *name, int maxLen)
{
    ServerClient *c = C(h);
    if (!c || !name || maxLen <= 0) return DFALSE;
    snprintf(name, (size_t)maxLen, "%s", c->name.c_str());
    return DTRUE;
}
void setClientInfoFlags(HCLIENT h, DDWORD f) { if (ServerClient *c = C(h)) c->infoFlags = f; }
DDWORD getClientInfoFlags(HCLIENT h) { ServerClient *c = C(h); return c ? c->infoFlags | CIF_LOCAL : 0; }
void setClientUserData(HCLIENT h, void *d) { if (ServerClient *c = C(h)) c->userData = d; }
void *getClientUserData(HCLIENT h) { ServerClient *c = C(h); return c ? c->userData : nullptr; }
DRESULT setClientViewPos(HCLIENT, DVector *) { return LT_OK; }
DBOOL isCommandOn(HCLIENT, int c) { return (S().commandOn && S().commandOn(c)) ? DTRUE : DFALSE; }

// ---- sky -----------------------------------------------------------------------------------

DRESULT getSkyDef(SkyDef *d)
{
    if (!d) return LT_ERROR;
    *d = S().sky;
    return LT_OK;
}
DRESULT setSkyDef(SkyDef *d)
{
    if (!d) return LT_ERROR;
    S().sky = *d;
    S().haveSky = true;
    return LT_OK;
}
DRESULT addObjectToSky(HOBJECT h, DDWORD index)
{
    EngineObject *o = O(h);
    if (!o) return LT_ERROR;
    S().skyObjects.push_back({o, index});
    return LT_OK;
}
DRESULT removeObjectFromSky(HOBJECT h)
{
    auto &v = S().skyObjects;
    v.erase(std::remove_if(v.begin(), v.end(), [&](auto &p) { return p.first == O(h); }), v.end());
    return LT_OK;
}

// ---- world -----------------------------------------------------------------------------------

DRESULT apiLoadWorld(char *name, DDWORD flags) { return S().loadWorld(name, flags); }
DRESULT apiRunWorld() { return S().runWorld(); }

DBOOL upperStrcmp(char *a, char *b) { return (a && b && strcasecmp(a, b) == 0) ? DTRUE : DFALSE; }

DDWORD g_serverFlags = 0;
DDWORD getServerFlags() { return g_serverFlags; }
DDWORD setServerFlags(DDWORD f) { g_serverFlags = f; return f; }
DRESULT cacheFile(DDWORD, char *) { return LT_OK; }
DRESULT okNoArgs() { return LT_OK; }

#define HOOK(name, fn) shogo_set_virtual_hook("ServerDE::" name, (void *)&fn)
}  // namespace

// ---- Server ----------------------------------------------------------------------------------

void Server::install(ServerDE *d, ClassDef **defs, int n)
{
    instance = this;
    de = d;
    classes.assign(defs, defs + n);

    d->GetGameInfo = getGameInfo;
    d->GetClass = getClass;
    d->GetObjectClass = getObjectClass;
    d->IsKindOf = isKindOf;
    d->CreateObject = apiCreateObject;
    d->CreateObjectProps = createObjectProps;
    d->RemoveObject = apiRemoveObject;
    d->GetTime = getTime;
    d->GetFrameTime = getFrameTime;
    d->GetServerFlags = getServerFlags;
    d->SetServerFlags = setServerFlags;
    d->CacheFile = cacheFile;
    d->Random = randomRange;
    d->IntRandom = intRandom;
    d->RandomScale = randomScale;
    d->BPrint = bprint;
    d->CPrint = bprint;
    d->DebugOut = debugOut;
    d->GetSkyDef = getSkyDef;
    d->SetSkyDef = setSkyDef;
    d->AddObjectToSky = addObjectToSky;
    d->RemoveObjectFromSky = removeObjectFromSky;
    d->FindNamedObjects = findNamedObjects;
    d->FindObjectsTouchingSphere = findObjectsTouchingSphere;
    d->RelinquishList = relinquishList;
    d->CreateObjectList = createObjectList;
    d->AddObjectToList = addObjectToList;
    d->RemoveObjectFromList = removeObjectFromList;
    d->GetNextClient = getNextClient;
    d->GetClientID = getClientID;
    d->GetClientName = getClientName;
    d->SetClientInfoFlags = setClientInfoFlags;
    d->GetClientInfoFlags = getClientInfoFlags;
    d->SetClientUserData = setClientUserData;
    d->GetClientUserData = getClientUserData;
    d->SetClientViewPos = setClientViewPos;
    d->RunGameConString = runGameConString;
    d->SetGameConVar = setGameConVar;
    d->GetGameConVar = getGameConVar;
    d->GetVarValueFloat = getVarValueFloat;
    d->GetVarValueString = getVarValueString;
    d->GetPropString = getPropString;
    d->GetPropVector = getPropVector;
    d->GetPropColor = getPropColor;
    d->GetPropReal = getPropReal;
    d->GetPropFlags = getPropFlags;
    d->GetPropBool = getPropBool;
    d->GetPropLongInt = getPropLongInt;
    d->GetPropRotation = getPropRotation;
    d->GetPropRotationEuler = getPropRotationEuler;
    d->GetPropGeneric = getPropGeneric;
    d->DoesPropExist = doesPropExist;
    d->ObjectToHandle = objectToHandle;
    d->HandleToObject = handleToObject;
    d->IsCommandOn = isCommandOn;
    d->GetRotationVectorsFromMatrix = getRotationVectorsFromMatrix;
    d->UpperStrcmp = upperStrcmp;
    d->Parse = parseCommand;
    d->RotateAroundAxis = rotateAroundAxis;
    d->EulerRotateX = eulerRotateX;
    d->EulerRotateY = eulerRotateY;
    d->EulerRotateZ = eulerRotateZ;
    d->AlignRotation = alignRotation;
    d->InterpolateRotation = interpolateRotation;
    d->SetupTransformationMatrix = setupTransformationMatrix;
    d->SetupTranslationMatrix = setupTranslationMatrix;
    d->SetupRotationMatrix = setupRotationMatrix;
    d->SetupTranslationFromMatrix = setupTranslationFromMatrix;
    d->SetupRotationFromMatrix = setupRotationFromMatrix;
    d->CreateInterObjectLink = createInterObjectLink;
    d->BreakInterObjectLink = breakInterObjectLink;
    d->GetObjectColor = getObjectColor;
    d->SetObjectColor = setObjectColor;
    d->GetObjectUserFlags = getObjectUserFlags;
    d->SetObjectUserFlags = setObjectUserFlags;
    d->GetNextObject = getNextObject;
    d->GetNextInactiveObject = getNextInactiveObject;
    d->GetObjectName = getObjectName;
    d->SetNextUpdate = setNextUpdate;
    d->SetDeactivationTime = setDeactivationTime;
    d->PingObjects = pingObjects;
    d->GetObjectPos = getObjectPos;
    d->SetObjectPos = setObjectPos;
    d->ScaleObject = scaleObject;
    d->GetObjectScale = getObjectScale;
    d->TeleportObject = teleportObject;
    d->IntersectSegment = apiIntersectSegment;
    d->CastRay = apiCastRay;
    d->GetPointContainers = getPointContainers;
    d->GetLastCollision = [](CollisionInfo *info) -> DRESULT {
        if (!info || !Server::instance) return LT_ERROR;
        *info = Server::instance->lastCollision;
        return LT_OK;
    };
    d->GetObjectContainers = getObjectContainers;
    d->GetObjectRotation = getObjectRotation;
    d->SetObjectRotation = setObjectRotation;
    d->RotateObject = setObjectRotation;
    d->GetObjectFlags = getObjectFlags;
    d->SetObjectFlags = setObjectFlags;
    d->SetObjectState = setObjectState;
    d->GetObjectState = getObjectState;
    d->SetBlockingPriority = setBlockingPriority;
    d->GetBlockingPriority = getBlockingPriority;
    d->GetLightColor = getLightColor;
    d->SetLightColor = setLightColor;
    d->GetLightRadius = getLightRadius;
    d->SetLightRadius = setLightRadius;
    d->GetModelFilenames = getModelFilenames;
    d->SetModelFilenames = setObjectFilenames;
    d->SetObjectFilenames = setObjectFilenames;
    d->GetContainerCode = getContainerCode;
    d->LoadWorld = apiLoadWorld;
    installSaveAPI(d);
    d->RunWorld = apiRunWorld;

    HOOK("GetWorldObject", vGetWorldObject);
    HOOK("GetWorldBox", vGetWorldBox);
    HOOK("GetGlobalForce", vGetGlobalForce);
    HOOK("SetGlobalForce", vSetGlobalForce);
    HOOK("GetObjectMass", vGetObjectMass);
    HOOK("SetObjectMass", vSetObjectMass);
    HOOK("GetForceIgnoreLimit", vGetForceIgnoreLimit);
    HOOK("SetForceIgnoreLimit", vSetForceIgnoreLimit);
    HOOK("SetFrictionCoefficient", vSetFriction);
    HOOK("MoveObject", vMoveObject);
    HOOK("GetStandingOn", vGetStandingOn);
    HOOK("GetPolyTextureFlags", vGetPolyTextureFlags);
    HOOK("GetNetFlags", vGetNetFlags);
    HOOK("SetNetFlags", vSetNetFlags);
    HOOK("GetObjectDims", vGetObjectDims);
    HOOK("SetObjectDims", vSetObjectDims);
    HOOK("SetObjectDims2", vSetObjectDims);
    HOOK("GetVelocity", vGetVelocity);
    HOOK("SetVelocity", vSetVelocity);
    HOOK("GetAcceleration", vGetAccel);
    HOOK("SetAcceleration", vSetAccel);
    HOOK("SetupEuler", vSetupEuler);
    HOOK("GetRotationVectors", vGetRotationVectors);
    HOOK("GetClientPing", vGetClientPing);
    HOOK("StartMessage", vStartMessage);
    HOOK("StartSpecialEffectMessage", vStartSpecialEffectMessage);
    HOOK("StartInstantSpecialEffectMessage", vStartInstantSpecialEffectMessage);
    HOOK("StartMessageToObject", vStartMessageToObject);
    HOOK("StartMessageToServer", vStartMessageToServer);
    HOOK("EndMessage", vEndMessage);
    HOOK("EndMessage2", vEndMessage2);
}

ClassDef *Server::findClass(const char *name) const
{
    if (!name) return nullptr;
    // By lower-case name (the game looks classes up every frame).
    if (m_classesIndexed != classes.size()) {
        m_classesIndexed = classes.size();
        m_classByName.clear();
        for (ClassDef *c : classes) m_classByName.emplace(lower(c->m_ClassName), c);
    }
    auto it = m_classByName.find(lower(name));
    return it == m_classByName.end() ? nullptr : it->second;
}

DDWORD Server::engineMessage(EngineObject *o, DDWORD msg, void *data, float fdata)
{
    if (!o || !o->game) return 0;
    for (ClassDef *c = o->cls; c; c = c->m_ParentClass)
        if (c->m_EngineMessageFn) return c->m_EngineMessageFn(o->game, msg, data, fdata);
    return 0;
}

DDWORD Server::objectMessage(EngineObject *to, EngineObject *from, DDWORD msg, Message *m)
{
    if (!to || !to->game || to->removed) return 0;
    if (m && msg == MID_TRIGGER) m->rereadable = true;
    for (ClassDef *c = to->cls; c; c = c->m_ParentClass)
        if (c->m_ObjectMessageFn)
            return c->m_ObjectMessageFn(to->game, from ? toHObject(from) : nullptr, msg, toReadHandle(m));
    return 0;
}

EngineObject *Server::createObject(ClassDef *cls, ObjectCreateStruct *ocs, float kind, const WorldObject *props,
                                   bool deferInitialUpdate)
{
    if (!cls || !ocs) return nullptr;
    ConstructObjectFn construct = nullptr;
    for (ClassDef *c = cls; c && !construct; c = c->m_ParentClass) construct = c->m_ConstructFn;
    if (!construct) return nullptr;

    EngineObject *o = objects().create(false);
    o->cls = cls;
    size_t size = std::max<size_t>((size_t)cls->m_ClassObjectSize, sizeof(void *) * 4);
    void *mem = ::operator new(size, std::align_val_t(16));
    memset(mem, 0, size);
    construct(mem);
    o->game = static_cast<LPBASECLASS>(mem);
    o->game->m_hObject = toHObject(o);

    const WorldObject *savedProps = currentProps;
    currentProps = props;
    DDWORD ok = engineMessage(o, MID_PRECREATE, ocs, kind);
    currentProps = savedProps;
    if (!ok) {
        // The object declined to be created.
        for (ClassDef *c = cls; c; c = c->m_ParentClass)
            if (c->m_DestructFn) {
                c->m_DestructFn(mem);
                break;
            }
        ::operator delete(mem, std::align_val_t(16));
        objects().remove(o);
        return nullptr;
    }

    o->type = ocs->m_ObjectType;
    o->flags = ocs->m_Flags;
    o->pos = ocs->m_Pos;
    o->scale = ocs->m_Scale;
    o->rot = ocs->m_Rotation;
    if (o->rot.m_Vec.x == 0 && o->rot.m_Vec.y == 0 && o->rot.m_Vec.z == 0 && o->rot.m_Spin == 0) o->rot = quatIdentity();
    o->containerCode = ocs->m_ContainerCode;
    o->userData = ocs->m_UserData;
    o->filename = ocs->m_Filename;
    o->skin = ocs->m_SkinName;
    o->name = ocs->m_Name;
    o->deactivationTime = ocs->m_fDeactivationTime;
    o->nextUpdate = ocs->m_NextUpdate > 0 ? time + ocs->m_NextUpdate : 0;
    if (o->type == OT_WORLDMODEL || o->type == OT_CONTAINER) {
        if (world)
            for (size_t i = 0; i < world->models.size(); ++i)
                if (!o->filename.empty() && strcasecmp(world->models[i].name.c_str(), o->filename.c_str()) == 0) {
                    o->bsp = int(i);
                    const WorldBsp &b = world->models[i];
                    o->dims = DVector((b.boxMax.x - b.boxMin.x) / 2, (b.boxMax.y - b.boxMin.y) / 2,
                                      (b.boxMax.z - b.boxMin.z) / 2);
                    // A world model from the world file sits at its geometry's centre (the
                    // file's translation), not at the editor handle its Pos property names.
                    // The game relies on it: a self-triggering door spawns its trigger box
                    // at the door's position.
                    if (props) o->pos = DVector(b.translation.x, b.translation.y, b.translation.z);
                    break;
                }
        o->basePos = o->pos;
        o->baseRot = o->rot;
    }
    objectModel(o);  // a model starts on animation 0, with its dims

    if (!deferInitialUpdate) engineMessage(o, MID_INITIALUPDATE, nullptr, kind);
    return o;
}

size_t Server::triggerObjects(const std::string &name, const std::string &message)
{
    std::vector<EngineObject *> targets;
    for (EngineObject *o : objects().all())
        if (!o->clientOnly && !o->removed && o->game && strcasecmp(o->name.c_str(), name.c_str()) == 0)
            targets.push_back(o);
    for (EngineObject *o : targets) {
        Message m;
        m.put<uint32_t>((uint32_t)message.size());
        m.put(message.data(), message.size());
        objectMessage(o, client.object, MID_TRIGGER, &m);
        m.magic = 0;
    }
    return targets.size();
}

void Server::removeObject(EngineObject *o)
{
    if (o && !o->clientOnly) o->removed = true;
}

void Server::flushRemovals()
{
    std::vector<EngineObject *> dead;
    for (EngineObject *o : objects().all())
        if (o->removed && !o->clientOnly) dead.push_back(o);
    for (EngineObject *o : dead) {
        // Tell owners of links to this object.
        for (EngineObject *other : objects().all()) {
            auto it = std::find(other->links.begin(), other->links.end(), o);
            if (it != other->links.end()) {
                other->links.erase(it);
                engineMessage(other, MID_LINKBROKEN, toHObject(o), 0);
            }
        }
        saveObjectRemoved(o);
        if (o->clientKnown && removedCallback) removedCallback(o);
        skyObjects.erase(std::remove_if(skyObjects.begin(), skyObjects.end(), [&](auto &p) { return p.first == o; }),
                         skyObjects.end());
        if (client.object == o) client.object = nullptr;
        if (o->game) {
            for (ClassDef *c = o->cls; c; c = c->m_ParentClass)
                if (c->m_DestructFn) {
                    c->m_DestructFn(o->game);
                    break;
                }
            ::operator delete(o->game, std::align_val_t(16));
            o->game = nullptr;
        }
        objects().remove(o);
    }
}

void Server::addClient()
{
    if (shell && shell->OnAddClient) shell->OnAddClient(shell, reinterpret_cast<HCLIENT>(&client));
}

DRESULT Server::loadWorld(const char *name, DDWORD flags)
{
    std::string path = normalizeWorldName(name);
    auto data = fs->read(path);
    if (!data) {
        fprintf(stderr, "[server] world not found: %s\n", path.c_str());
        return LT_ERROR;
    }
    std::string err;
    auto w = WorldData::parse(*data, &err);
    if (!w) {
        fprintf(stderr, "[server] %s: %s\n", path.c_str(), err.c_str());
        return LT_ERROR;
    }
    printf("[server] loading %s\n", path.c_str());

    bool switching = world != nullptr;
    if (client.inWorld && onClientLeavingWorld) onClientLeavingWorld();
    if (shell && shell->PreStartWorld) shell->PreStartWorld(shell, switching);

    // Drop the old world's objects.
    for (EngineObject *o : objects().all())
        if (!o->clientOnly) o->removed = true;
    flushRemovals();
    skyObjects.clear();
    haveSky = false;
    client.object = nullptr;
    client.inWorld = false;
    clearClientRefs();

    world = std::move(w);
    mixer().clear();  // sounds of the old world
    collisionModels.assign(world->models.size(), CollisionModel());
    for (size_t i = 0; i < world->models.size(); ++i) collisionModels[i].build(world->models[i], i);
    worldName = path;
    worldRunning = false;

    // The world itself is a world model object (GetWorldObject).
    if (ClassDef *wm = findClass("WorldModel")) {
        ObjectCreateStruct ocs;
        memset(&ocs, 0, sizeof(ocs));
        ocs.m_Scale = DVector(1, 1, 1);
        ocs.m_Rotation = quatIdentity();
        ocs.m_ObjectType = OT_WORLDMODEL;
        if (EngineObject *o = createObject(wm, &ocs, 0.0f, nullptr)) {
            o->type = OT_WORLDMODEL;
            o->bsp = 0;
            o->flags |= FLAG_VISIBLE | FLAG_SOLID;
            worldObject = o;
        }
    }

    if (flags & LOADWORLD_LOADWORLDOBJECTS) {
        std::vector<std::pair<EngineObject *, float>> created;
        size_t unknown = 0;
        for (const WorldObject &wo : world->objects) {
            ClassDef *cls = findClass(wo.className.c_str());
            if (!cls) {
                ++unknown;
                continue;
            }
            if (cls->m_ClassFlags & CF_NORUNTIME) continue;
            ObjectCreateStruct ocs;
            memset(&ocs, 0, sizeof(ocs));
            ocs.m_Scale = DVector(1, 1, 1);
            ocs.m_Rotation = quatIdentity();
            if (EngineObject *o = createObject(cls, &ocs, PRECREATE_WORLDFILE, &wo, true))
                created.push_back({o, INITIALUPDATE_WORLDFILE});
        }
        // Everything exists before anything gets its initial update.
        for (auto &[o, kind] : created)
            if (o->magic == EngineObject::kMagic && !o->removed) engineMessage(o, MID_INITIALUPDATE, nullptr, kind);
        printf("[server] created %zu objects from %zu in the world (%zu of unknown classes)\n", created.size(),
               world->objects.size(), unknown);
    }
    if (flags & LOADWORLD_RUNWORLD) return runWorld();
    return LT_OK;
}

DRESULT Server::runWorld()
{
    if (!world) return LT_ERROR;
    worldRunning = true;
    toClientBeforeRun = toClient.size();
    if (shell && shell->PostStartWorld) shell->PostStartWorld(shell);
    enterClient();
    return LT_OK;
}

void Server::enterClient()
{
    if (!shell || !shell->OnClientEnterWorld) return;
    LPBASECLASS player = shell->OnClientEnterWorld(shell, reinterpret_cast<HCLIENT>(&client), nullptr, 0);
    client.object = player ? toObject(player->m_hObject) : nullptr;
    client.inWorld = true;
    printf("[server] client entered the world; player object %s\n",
           client.object ? client.object->cls->m_ClassName : "(none)");
    if (onClientEnteredWorld) onClientEnteredWorld();
}

void Server::update(double dt)
{
    frameTime = dt;
    time += dt;

    while (!fromClient.empty()) {
        PendingMessage pm = std::move(fromClient.front());
        fromClient.pop_front();
        if (shell && shell->OnMessage)
            shell->OnMessage(shell, reinterpret_cast<HCLIENT>(&client), pm.id, toReadHandle(pm.msg.get()));
        pm.msg->magic = 0;
    }
    if (!worldRunning) return;

    if (shell && shell->Update) shell->Update(shell, (DFLOAT)dt);

    // Physics: velocity, acceleration and gravity, moved with collision.
    // The player's object is moved by the client (MID_PLAYER_UPDATE).
    std::vector<EngineObject *> movers;
    for (EngineObject *o : objects().all()) {
        if (o->clientOnly || o->removed || o == client.object || o->type == OT_WORLDMODEL || o->type == OT_CONTAINER)
            continue;
        bool moving = o->velocity.x != 0 || o->velocity.y != 0 || o->velocity.z != 0 || o->accel.x != 0 ||
                      o->accel.y != 0 || o->accel.z != 0 || (o->flags & FLAG_GRAVITY);
        if (moving) movers.push_back(o);
    }
    for (EngineObject *o : movers) {
        if (o->magic != EngineObject::kMagic || o->removed) continue;
        // Friction on the ground (SetFrictionCoefficient): the coefficient times
        // gravity, as for a block on a floor. Our reading; the engine's own model
        // isn't known. Without it a dropped weapon keeps the sideways
        // acceleration it was spawned with and slides across the level for ever.
        float groundFriction = 0.0f;
        if ((o->flags & FLAG_GRAVITY) && o->friction > 0.0f &&
            (o->velocity.x != 0 || o->velocity.z != 0 || o->accel.x != 0 || o->accel.z != 0) &&
            standingOn(*this, o, nullptr))
            groundFriction = o->friction * std::fabs(globalForce.y);
        DVector off = integrateMotion(*this, o, (float)dt, groundFriction);
        if (off.x == 0 && off.y == 0 && off.z == 0) continue;
        moveObject(*this, o, DVector(o->pos.x + off.x, o->pos.y + off.y, o->pos.z + off.z), false);
    }

    // Objects whose update time has come. Iterate over a snapshot: updates create objects.
    std::vector<EngineObject *> due;
    for (EngineObject *o : objects().all())
        if (!o->clientOnly && !o->removed && o->nextUpdate > 0 && time >= o->nextUpdate) due.push_back(o);
    for (EngineObject *o : due) {
        if (o->magic != EngineObject::kMagic || o->removed) continue;
        o->nextUpdate = 0;
        engineMessage(o, MID_UPDATE, nullptr, 0);
    }

    // Model animations; string keys go to objects that asked for them.
    std::vector<EngineObject *> models;
    for (EngineObject *o : objects().all())
        if (!o->clientOnly && !o->removed && o->type == OT_MODEL) models.push_back(o);
    for (EngineObject *o : models) {
        if (o->magic != EngineObject::kMagic || o->removed) continue;
        // Server objects get keys only with FLAG_MODELKEYS; the client shell gets
        // them for every client object (the first-person weapon fires on its "fire" keys).
        animateObject(o, (float)dt, [this](EngineObject *obj, ArgList *args) {
            if (obj->flags & FLAG_MODELKEYS) engineMessage(obj, MID_MODELSTRINGKEY, args, 0);
        });
    }
    flushRemovals();
    updateAttachments();
}

// ---- console variables -------------------------------------------------------------------

ConsoleVars::Var *ConsoleVars::find(const std::string &name)
{
    auto it = m_vars.find(lower(name));
    return it == m_vars.end() ? nullptr : it->second.get();
}

ConsoleVars::Var *ConsoleVars::set(const std::string &name, const std::string &value)
{
    auto &v = m_vars[lower(name)];
    if (!v) {
        v = std::make_unique<Var>();
        v->name = name;
    }
    v->value = value;
    v->number = (float)atof(value.c_str());
    v->args.clear();
    return v.get();
}

void ConsoleVars::run(const std::string &command)
{
    std::vector<char> text(command.begin(), command.end());
    text.push_back(0);
    std::vector<char> buf(text.size() * 2 + 16);
    char *argv[64];
    char *pos = text.data();
    for (;;) {
        int n = 0;
        char *next = nullptr;
        int more = parseCommand(pos, &next, buf.data(), argv, &n);
        if (n > 0) {
            std::vector<std::string> args(argv, argv + n);
            if (args[0].size() > 1 && args[0][0] == '+') args[0] = args[0].substr(1);
            if (!commands || !commands(args)) {
                if (args.size() >= 2) {
                    Var *v = set(args[0], args[1]);
                    if (args.size() > 2) v->args.assign(args.begin() + 1, args.end());
                }
            }
        }
        if (!more || !next) break;
        pos = next;
    }
}

}  // namespace shogo
