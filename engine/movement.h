// Object movement against the world and other objects (MoveObject), gravity,
// what an object stands on, touch notifications, and ray queries. Shared by
// the server and the client's physics interface.
#ifndef SHOGO_ENGINE_MOVEMENT_H
#define SHOGO_ENGINE_MOVEMENT_H

#include "engine/api.h"
#include "engine/collision.h"
#include "engine/objects.h"

namespace shogo {

class Server;

// Moves `o` toward `target`, colliding and sliding. Clips the object's velocity
// against what it hit and sends touch notifications (server objects only).
void moveObject(Server &server, EngineObject *o, const DVector &target, bool teleport);

// Integrates velocity, acceleration and gravity over dt; returns the offset to apply.
// groundFriction > 0 (units/s^2): the object rests on the ground, and its sideways
// speed (after this frame's acceleration) drops by that much per second, to zero.
DVector integrateMotion(Server &server, EngineObject *o, float dt, float groundFriction = 0.0f);

// What the object rests on (a short sweep downwards).
bool standingOn(Server &server, EngineObject *o, CollisionInfo *info);

// Sets an object's dims the way the engine does when they grow taller: from
// where it is, lifted out of the floor it would then be in (by no more than it
// grows), and only where there's room above for that. Fails, leaving the dims
// as they were, where there isn't.
bool growDims(Server &server, EngineObject *o, const DVector &newDims);

// IntersectSegment / CastRay. clientSide: the query comes from the client.
bool intersectSegment(Server &server, IntersectQuery *q, IntersectInfo *info, bool clientSide);

// Containers (water, ladders...) that contain the point, or that a box of
// half extents `ext` around it overlaps (GetObjectContainers).
int pointContainers(Server &server, const DVector &p, EngineObject **out, int max, const DVector &ext = DVector(0, 0, 0));

// GetPolyTextureFlags: the surface type stored in the polygon's texture (DTX
// header, the u32 after the flags: 20 metal, 40 glass, 110 sky...). 0 if unknown.
DDWORD polyTextureFlags(Server &server, HPOLY poly);

}  // namespace shogo

#endif
