// Where the screen surface really lives when it isn't plain CPU memory: the
// GPU screen (gpu_screen.h) implements this. The client API sends drawing
// that targets the screen here, and asks for the CPU copy (screen.pixels)
// only when something reads it or draws on it with the CPU.
//
// Each operation has the same clipping and pixel rules as its CPU version in
// surfaces.h.
#ifndef SHOGO_ENGINE_SCREEN_TARGET_H
#define SHOGO_ENGINE_SCREEN_TARGET_H

#include "engine/surfaces.h"

namespace shogo {

class ScreenTarget {
public:
    virtual ~ScreenTarget() = default;

    // Drawing onto the screen; sources are CPU surfaces (never the screen).
    virtual void fill(Surface &screen, const DRect *rect, uint32_t color) = 0;
    virtual void blit(Surface &screen, const Surface &src, const DRect *srcRect, int dx, int dy,
                      const BlitParams &p) = 0;
    virtual void scaleBlit(Surface &screen, const Surface &src, const DRect *dstRect, const DRect *srcRect,
                           const BlitParams &p) = 0;
    virtual void warpBlit(Surface &screen, const Surface &src, const DWarpPt *pts, int n, const BlitParams &p) = 0;
    virtual void transformBlit(Surface &screen, const Surface &src, const DFloatPt *origin, int dx, int dy, float angle,
                               float sx, float sy, const BlitParams &p) = 0;
    virtual void maskedBlit(Surface &screen, const Surface &src, const Surface &mask, const DRect *srcRect, int dx,
                            int dy, uint32_t key) = 0;

    // Makes screen.pixels current (reading the GPU copy back if needed).
    virtual void toCPU(Surface &screen) = 0;
    // screen.pixels is about to be changed by the CPU; it's the copy that
    // counts until the next GPU drawing, which uploads it first.
    virtual void cpuWrites(Surface &screen) = 0;
    // Shows the frame (FlipScreen).
    virtual void present(Surface &screen) = 0;
};

}  // namespace shogo

#endif
