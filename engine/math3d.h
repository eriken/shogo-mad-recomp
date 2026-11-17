// Rotation math behind the API's DRotation (a quaternion: m_Vec = x,y,z, m_Spin = w).
//
// Conventions match the world renderer: left-handed, +Y up, +Z forward; Euler
// angles are (pitch about X, yaw about Y, roll about Z) composed as
// R = Ry(yaw) * Rx(pitch) * Rz(roll); positive pitch looks down.
#ifndef SHOGO_ENGINE_MATH3D_H
#define SHOGO_ENGINE_MATH3D_H

#include "engine/api.h"

namespace shogo {

DRotation quatIdentity();
DRotation quatMul(const DRotation &a, const DRotation &b);
DRotation quatAxisAngle(const DVector &axis, float angle);
DRotation quatEuler(float pitch, float yaw, float roll);
DRotation quatNormalize(const DRotation &q);
DRotation quatSlerp(const DRotation &a, const DRotation &b, float t);
DVector quatRotate(const DRotation &q, const DVector &v);
void quatVectors(const DRotation &q, DVector *up, DVector *right, DVector *forward);
// Rotation whose forward is `forward` and whose up is as close to `up` as possible.
DRotation quatLookAt(const DVector &forward, const DVector &up);
void quatToMatrix(const DRotation &q, DMatrix *m);
DRotation quatFromMatrix(const DMatrix &m);

inline DVector vec(float x, float y, float z) { DVector v; v.x = x; v.y = y; v.z = z; return v; }

}  // namespace shogo

#endif
