// Engine functions that ClientDE and ServerDE both offer with the same
// signature: rotation helpers and the console-style argument parser.
#ifndef SHOGO_ENGINE_API_COMMON_H
#define SHOGO_ENGINE_API_COMMON_H

#include "engine/api.h"

namespace shogo {

void rotateAroundAxis(DRotation *r, DVector *axis, float amount);
void eulerRotateX(DRotation *r, float a);
void eulerRotateY(DRotation *r, float a);
void eulerRotateZ(DRotation *r, float a);
void alignRotation(DRotation *r, DVector *fwd, DVector *up);
DRESULT interpolateRotation(DRotation *d, DRotation *a, DRotation *b, float t);
void setupRotationMatrix(DMatrix *m, DRotation *r);
void setupTransformationMatrix(DMatrix *m, DVector *t, DRotation *r);
void setupTranslationMatrix(DMatrix *m, DVector *t);
void setupTranslationFromMatrix(DVector *t, DMatrix *m);
void setupRotationFromMatrix(DRotation *r, DMatrix *m);
void getRotationVectorsFromMatrix(DMatrix *m, DVector *u, DVector *r, DVector *f);

// Splits one command (up to ';' or the end) into arguments; text in quotes is
// one argument. Returns nonzero when a ';' was found and more commands follow.
int parseCommand(char *command, char **newPos, char *argBuffer, char **argPointers, int *nArgs);

}  // namespace shogo

#endif
