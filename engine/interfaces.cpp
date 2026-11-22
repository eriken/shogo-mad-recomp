// The small C++ interfaces the API objects hand out: Physics() and Common().
// One instance of each serves both the client and the server, since they share
// the same objects.
#include "engine/interfaces.h"

#include "common_de.h"
#include "physics_lt.h"

#include "engine/math3d.h"
#include "engine/model.h"
#include "engine/movement.h"
#include "engine/objects.h"
#include "engine/server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The SDK declares these non-pure base methods but leaves their bodies to the engine.
DRESULT PhysicsLT::GetFrictionCoefficient(HOBJECT h, float &c)
{
    auto *o = shogo::toObject(h);
    c = o ? o->friction : 0;
    return o ? LT_OK : LT_ERROR;
}
DRESULT PhysicsLT::SetFrictionCoefficient(HOBJECT h, float c)
{
    auto *o = shogo::toObject(h);
    if (!o) return LT_ERROR;
    o->friction = c;
    return LT_OK;
}
DRESULT PhysicsLT::GetForceIgnoreLimit(HOBJECT h, float &l)
{
    auto *o = shogo::toObject(h);
    l = o ? o->forceIgnoreLimit : 0;
    return o ? LT_OK : LT_ERROR;
}
DRESULT PhysicsLT::SetForceIgnoreLimit(HOBJECT h, float l)
{
    auto *o = shogo::toObject(h);
    if (!o) return LT_ERROR;
    o->forceIgnoreLimit = l;
    return LT_OK;
}
DRESULT PhysicsLT::GetVelocity(HOBJECT h, DVector *v)
{
    auto *o = shogo::toObject(h);
    if (!o || !v) return LT_ERROR;
    *v = o->velocity;
    return LT_OK;
}
DRESULT PhysicsLT::GetAcceleration(HOBJECT h, DVector *v)
{
    auto *o = shogo::toObject(h);
    if (!o || !v) return LT_ERROR;
    *v = o->accel;
    return LT_OK;
}
DRESULT PhysicsLT::GetObjectMass(HOBJECT h, float &m)
{
    auto *o = shogo::toObject(h);
    m = o ? o->mass : 0;
    return o ? LT_OK : LT_ERROR;
}
DRESULT PhysicsLT::SetObjectMass(HOBJECT h, float m)
{
    auto *o = shogo::toObject(h);
    if (!o) return LT_ERROR;
    o->mass = m;
    return LT_OK;
}
DRESULT PhysicsLT::GetObjectDims(HOBJECT h, DVector *d)
{
    auto *o = shogo::toObject(h);
    if (!o || !d) return LT_ERROR;
    *d = o->dims;
    return LT_OK;
}
DRESULT PhysicsLT::GetStandingOn(HOBJECT h, CollisionInfo *info)
{
    if (info) memset(info, 0, sizeof(*info));
    if (!shogo::Server::instance) return LT_ERROR;
    return shogo::standingOn(*shogo::Server::instance, shogo::toObject(h), info) ? LT_OK : LT_ERROR;
}

namespace shogo {

DRESULT pointStatus(const DVector *p)
{
    if (!Server::instance || !Server::instance->world || Server::instance->world->models.empty()) return DE_NOTINWORLD;
    if (!p) return DE_OUTSIDE;
    return Server::instance->world->models[0].pointInside({p->x, p->y, p->z}) ? DE_INSIDE : DE_OUTSIDE;
}

namespace {

class EnginePhysics : public CPhysicsLT {
public:
    DRESULT SetVelocity(HOBJECT h, DVector *v) override
    {
        EngineObject *o = toObject(h);
        if (!o || !v) return LT_ERROR;
        o->velocity = *v;
        return LT_OK;
    }
    DRESULT SetAcceleration(HOBJECT h, DVector *v) override
    {
        EngineObject *o = toObject(h);
        if (!o || !v) return LT_ERROR;
        if (getenv("SHOGO_TRACE_PHYSICS")) printf("[physics] SetAcceleration %.0f %.0f %.0f\n", v->x, v->y, v->z);
        o->accel = *v;
        return LT_OK;
    }
    DRESULT SetObjectDims(HOBJECT h, DVector *d, DDWORD) override
    {
        EngineObject *o = toObject(h);
        if (!o || !d) return LT_ERROR;
        // The player's mover goes to a tiny cube and back up to its size on
        // every animation change, so as not to "clip thru stuff": out of the
        // floor, where it fits (it tries again each frame where it doesn't).
        if (!Server::instance) {
            o->dims = *d;
            return LT_OK;
        }
        return growDims(*Server::instance, o, *d) ? LT_OK : LT_ERROR;
    }
    DRESULT MoveObject(HOBJECT h, DVector *p, DDWORD flags) override
    {
        EngineObject *o = toObject(h);
        if (!o || !p || !Server::instance) return LT_ERROR;
        moveObject(*Server::instance, o, *p, (flags & MOVEOBJECT_TELEPORT) != 0);
        return LT_OK;
    }
    DRESULT GetWorldObject(HOBJECT *out) override
    {
        if (!out) return LT_ERROR;
        *out = (Server::instance && Server::instance->worldObject) ? toHObject(Server::instance->worldObject) : nullptr;
        return *out ? LT_OK : LT_ERROR;
    }
    DRESULT GetGlobalForce(DVector &v) override
    {
        v = Server::instance ? Server::instance->globalForce : DVector(0, -2000, 0);
        return LT_OK;
    }
    DRESULT SetGlobalForce(DVector &v) override
    {
        if (Server::instance) Server::instance->globalForce = v;
        return LT_OK;
    }
    DRESULT UpdateMovement(MoveInfo *info) override
    {
        if (!info || !Server::instance) return LT_ERROR;
        EngineObject *o = toObject(info->m_hObject);
        if (!o) return LT_ERROR;
        info->m_Offset = integrateMotion(*Server::instance, o, info->m_dt);
        return LT_OK;
    }
    DRESULT MovePushObjects(HOBJECT h, DVector &p, HOBJECT *, DDWORD) override { return MoveObject(h, &p, 0); }
    DRESULT RotatePushObjects(HOBJECT h, DRotation &r, HOBJECT *, DDWORD) override
    {
        EngineObject *o = toObject(h);
        if (!o) return LT_ERROR;
        o->rot = r;
        return LT_OK;
    }
};

class EngineCommon : public CommonLT {
public:
    DRESULT Parse(ConParse *) override { return LT_ERROR; }
    DRESULT GetObjectType(HOBJECT h, DDWORD *type) override
    {
        EngineObject *o = toObject(h);
        if (!o || !type) return LT_ERROR;
        *type = (DDWORD)o->type;
        return LT_OK;
    }
    DRESULT GetModelAnimUserDims(HOBJECT h, DVector *dims, HMODELANIM anim) override
    {
        return modelAnimUserDims(h, dims, anim);
    }
    DRESULT GetRotationVectors(DRotation &r, DVector &u, DVector &ri, DVector &f) override
    {
        quatVectors(r, &u, &ri, &f);
        return LT_OK;
    }
    DRESULT SetupEuler(DRotation &r, float p, float y, float rl) override
    {
        r = quatEuler(p, y, rl);
        return LT_OK;
    }
    DRESULT CreateMessage(LMessage *&m) override
    {
        m = nullptr;
        return LT_ERROR;
    }
    DRESULT GetPointStatus(DVector *p) override { return pointStatus(p); }
    DRESULT GetPointShade(DVector *, DVector *c) override
    {
        if (c) *c = DVector(255, 255, 255);
        return LT_OK;
    }
    DRESULT GetPolyTextureFlags(HPOLY poly, DDWORD *f) override
    {
        if (f) *f = Server::instance ? polyTextureFlags(*Server::instance, poly) : 0;
        return LT_OK;
    }
};

EnginePhysics g_physics;
EngineCommon g_common;

}  // namespace
}  // namespace shogo

// The API headers make these classes friends of ClientDE / ServerDE so they
// can fill in the protected interface pointers.
class CClientMgr {
public:
    static void attach(ClientDE *c)
    {
        c->m_pPhysicsLT = &shogo::g_physics;
        c->m_pCommonLT = &shogo::g_common;
    }
};
class CServerMgr {
public:
    static void attach(ServerDE *s)
    {
        s->m_pPhysicsLT = &shogo::g_physics;
        s->m_pCommonLT = &shogo::g_common;
    }
};

namespace shogo {
void installInterfaces(ClientDE *c) { CClientMgr::attach(c); }
void installInterfaces(ServerDE *s) { CServerMgr::attach(s); }
}  // namespace shogo
