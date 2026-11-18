// The GPU backend: draws the world's passes with SDL's GPU API (Metal on
// macOS).
//
// For now it still works on the CPU frame: begin() uploads the target
// surface, draws go to a GPU copy of it, and end() reads the result back, so
// the 2D drawing around it keeps working unchanged. What it draws so far:
// opaque textured polygons with vertex or lightmap light, the global light
// scale, and the sky. Other polygons (translucent, sprites, particles) are
// skipped, and dynamic lights are ignored.
#ifndef SHOGO_ENGINE_RENDER_GPU_H
#define SHOGO_ENGINE_RENDER_GPU_H

#include "engine/render_backend.h"

#include <memory>

struct SDL_GPUDevice;

namespace shogo {

// device: shared with the window's presenter (not owned), or null for a
// device of its own (headless tools). Null when there's no usable GPU.
std::unique_ptr<RenderBackend> makeGpuBackend(SDL_GPUDevice *device = nullptr);

}  // namespace shogo

#endif
