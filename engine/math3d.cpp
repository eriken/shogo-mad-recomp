#include "engine/math3d.h"

#include <cmath>

namespace shogo {

namespace {
DRotation q(float x, float y, float z, float w)
{
    DRotation r;
    r.m_Vec.x = x;
    r.m_Vec.y = y;
    r.m_Vec.z = z;
    r.m_Spin = w;
    return r;
}
float len3(const DVector &v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
DVector cross(const DVector &a, const DVector &b)
{
    return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
DVector norm(const DVector &v)
{
    float l = len3(v);
    return l > 1e-9f ? vec(v.x / l, v.y / l, v.z / l) : vec(0, 0, 0);
}
}  // namespace

DRotation quatIdentity() { return q(0, 0, 0, 1); }

DRotation quatMul(const DRotation &a, const DRotation &b)
{
    const DVector &u = a.m_Vec, &v = b.m_Vec;
    float aw = a.m_Spin, bw = b.m_Spin;
    return q(aw * v.x + bw * u.x + u.y * v.z - u.z * v.y, aw * v.y + bw * u.y + u.z * v.x - u.x * v.z,
             aw * v.z + bw * u.z + u.x * v.y - u.y * v.x, aw * bw - u.x * v.x - u.y * v.y - u.z * v.z);
}

DRotation quatAxisAngle(const DVector &axis, float angle)
{
    DVector n = norm(axis);
    float s = std::sin(angle * 0.5f);
    return q(n.x * s, n.y * s, n.z * s, std::cos(angle * 0.5f));
}

DRotation quatEuler(float pitch, float yaw, float roll)
{
    DRotation ry = quatAxisAngle(vec(0, 1, 0), yaw), rx = quatAxisAngle(vec(1, 0, 0), pitch),
              rz = quatAxisAngle(vec(0, 0, 1), roll);
    return quatMul(quatMul(ry, rx), rz);
}

DRotation quatNormalize(const DRotation &r)
{
    float l = std::sqrt(r.m_Vec.x * r.m_Vec.x + r.m_Vec.y * r.m_Vec.y + r.m_Vec.z * r.m_Vec.z + r.m_Spin * r.m_Spin);
    if (l < 1e-9f) return quatIdentity();
    return q(r.m_Vec.x / l, r.m_Vec.y / l, r.m_Vec.z / l, r.m_Spin / l);
}

DRotation quatSlerp(const DRotation &a, const DRotation &b0, float t)
{
    DRotation b = b0;
    float d = a.m_Vec.x * b.m_Vec.x + a.m_Vec.y * b.m_Vec.y + a.m_Vec.z * b.m_Vec.z + a.m_Spin * b.m_Spin;
    if (d < 0) {
        b = q(-b.m_Vec.x, -b.m_Vec.y, -b.m_Vec.z, -b.m_Spin);
        d = -d;
    }
    float ka = 1 - t, kb = t;
    if (d < 0.9995f) {
        float th = std::acos(d), s = std::sin(th);
        ka = std::sin((1 - t) * th) / s;
        kb = std::sin(t * th) / s;
    }
    return quatNormalize(q(a.m_Vec.x * ka + b.m_Vec.x * kb, a.m_Vec.y * ka + b.m_Vec.y * kb,
                           a.m_Vec.z * ka + b.m_Vec.z * kb, a.m_Spin * ka + b.m_Spin * kb));
}

DVector quatRotate(const DRotation &r, const DVector &v)
{
    // v' = v + 2w(u x v) + 2 u x (u x v)
    const DVector &u = r.m_Vec;
    DVector t = cross(u, v);
    t = vec(t.x * 2, t.y * 2, t.z * 2);
    DVector c = cross(u, t);
    return vec(v.x + r.m_Spin * t.x + c.x, v.y + r.m_Spin * t.y + c.y, v.z + r.m_Spin * t.z + c.z);
}

void quatVectors(const DRotation &r, DVector *up, DVector *right, DVector *forward)
{
    if (up) *up = quatRotate(r, vec(0, 1, 0));
    if (right) *right = quatRotate(r, vec(1, 0, 0));
    if (forward) *forward = quatRotate(r, vec(0, 0, 1));
}

void quatToMatrix(const DRotation &r, DMatrix *m)
{
    DVector x = quatRotate(r, vec(1, 0, 0)), y = quatRotate(r, vec(0, 1, 0)), z = quatRotate(r, vec(0, 0, 1));
    // Columns are the rotated basis vectors.
    m->m[0][0] = x.x; m->m[0][1] = y.x; m->m[0][2] = z.x; m->m[0][3] = 0;
    m->m[1][0] = x.y; m->m[1][1] = y.y; m->m[1][2] = z.y; m->m[1][3] = 0;
    m->m[2][0] = x.z; m->m[2][1] = y.z; m->m[2][2] = z.z; m->m[2][3] = 0;
    m->m[3][0] = 0;   m->m[3][1] = 0;   m->m[3][2] = 0;   m->m[3][3] = 1;
}

DRotation quatFromMatrix(const DMatrix &m)
{
    float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
    DRotation r;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2;
        r = q((m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s, 0.25f * s);
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        r = q(0.25f * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[2][1] - m.m[1][2]) / s);
    } else if (m.m[1][1] > m.m[2][2]) {
        float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        r = q((m.m[0][1] + m.m[1][0]) / s, 0.25f * s, (m.m[1][2] + m.m[2][1]) / s, (m.m[0][2] - m.m[2][0]) / s);
    } else {
        float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        r = q((m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25f * s, (m.m[1][0] - m.m[0][1]) / s);
    }
    return quatNormalize(r);
}

DRotation quatLookAt(const DVector &forward, const DVector &upHint)
{
    DVector f = norm(forward);
    if (len3(f) < 0.5f) return quatIdentity();
    DVector u = upHint;
    if (len3(u) < 1e-6f || std::fabs(f.x * u.x + f.y * u.y + f.z * u.z) > 0.999f * len3(u))
        u = std::fabs(f.y) < 0.99f ? vec(0, 1, 0) : vec(0, 0, 1);
    // Left-handed: right = up x forward.
    DVector r = norm(cross(u, f));
    u = cross(f, r);
    DMatrix m;
    m.m[0][0] = r.x; m.m[0][1] = u.x; m.m[0][2] = f.x; m.m[0][3] = 0;
    m.m[1][0] = r.y; m.m[1][1] = u.y; m.m[1][2] = f.y; m.m[1][3] = 0;
    m.m[2][0] = r.z; m.m[2][1] = u.z; m.m[2][2] = f.z; m.m[2][3] = 0;
    m.m[3][0] = 0;   m.m[3][1] = 0;   m.m[3][2] = 0;   m.m[3][3] = 1;
    return quatFromMatrix(m);
}

}  // namespace shogo
