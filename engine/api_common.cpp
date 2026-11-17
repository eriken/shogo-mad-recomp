#include "engine/api_common.h"

#include "engine/math3d.h"

namespace shogo {

namespace {
void eulerRotateAxis(DRotation *r, const DVector &local, float amount)
{
    if (!r) return;
    // Rotate about the rotation's own axis.
    DVector axis = quatRotate(*r, local);
    *r = quatNormalize(quatMul(quatAxisAngle(axis, amount), *r));
}
}  // namespace

void rotateAroundAxis(DRotation *r, DVector *axis, float amount)
{
    if (r && axis) *r = quatNormalize(quatMul(quatAxisAngle(*axis, amount), *r));
}
void eulerRotateX(DRotation *r, float a) { eulerRotateAxis(r, DVector(1, 0, 0), a); }
void eulerRotateY(DRotation *r, float a) { eulerRotateAxis(r, DVector(0, 1, 0), a); }
void eulerRotateZ(DRotation *r, float a) { eulerRotateAxis(r, DVector(0, 0, 1), a); }
void alignRotation(DRotation *r, DVector *fwd, DVector *up)
{
    if (r && fwd) *r = quatLookAt(*fwd, up ? *up : DVector(0, 1, 0));
}
DRESULT interpolateRotation(DRotation *d, DRotation *a, DRotation *b, float t)
{
    if (!d || !a || !b) return LT_ERROR;
    *d = quatSlerp(*a, *b, t);
    return LT_OK;
}
void setupRotationMatrix(DMatrix *m, DRotation *r) { if (m && r) quatToMatrix(*r, m); }
void setupTransformationMatrix(DMatrix *m, DVector *t, DRotation *r)
{
    if (!m) return;
    quatToMatrix(r ? *r : quatIdentity(), m);
    if (t) {
        m->m[0][3] = t->x;
        m->m[1][3] = t->y;
        m->m[2][3] = t->z;
    }
}
void setupTranslationMatrix(DMatrix *m, DVector *t)
{
    DRotation id = quatIdentity();
    setupTransformationMatrix(m, t, &id);
}
void setupTranslationFromMatrix(DVector *t, DMatrix *m) { if (t && m) *t = DVector(m->m[0][3], m->m[1][3], m->m[2][3]); }
void setupRotationFromMatrix(DRotation *r, DMatrix *m) { if (r && m) *r = quatFromMatrix(*m); }
void getRotationVectorsFromMatrix(DMatrix *m, DVector *u, DVector *r, DVector *f)
{
    if (!m) return;
    if (r) *r = DVector(m->m[0][0], m->m[1][0], m->m[2][0]);
    if (u) *u = DVector(m->m[0][1], m->m[1][1], m->m[2][1]);
    if (f) *f = DVector(m->m[0][2], m->m[1][2], m->m[2][2]);
}
int parseCommand(char *cmd, char **newPos, char *buf, char **args, int *nArgs)
{
    constexpr int kMaxArgs = 64;
    int n = 0;
    char *p = cmd ? cmd : const_cast<char *>("");
    char *out = buf;
    bool more = false;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        if (*p == ';') {
            ++p;
            more = true;
            break;
        }
        char *start = out;
        if (*p == '"') {
            ++p;
            while (*p && *p != '"') *out++ = *p++;
            if (*p == '"') ++p;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != ';') *out++ = *p++;
        }
        *out++ = 0;
        if (n < kMaxArgs && args) args[n++] = start;
    }
    if (newPos) *newPos = p;
    if (nArgs) *nArgs = n;
    return more ? 1 : 0;
}

}  // namespace shogo
