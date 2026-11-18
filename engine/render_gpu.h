// The GPU backend: draws the world's passes with SDL's GPU API (Metal on
// macOS).
//
// When the target surface is the GPU screen (gpu_screen.h) it draws straight
// into the screen texture. Any other surface is uploaded in begin(), drawn on
// as a GPU copy and read back in end(). It follows the software rasterizer's
// rules: vertex or lightmap light, the light scale, dynamic lights, alpha and
// additive blending, masks and see-through black, soft particles, the screen
// tint.
#ifndef SHOGO_ENGINE_RENDER_GPU_H
#define SHOGO_ENGINE_RENDER_GPU_H

#include "engine/render_backend.h"

#include <memory>

struct SDL_GPUDevice;

namespace shogo {

class GpuScreen;

// device: shared with the window's presenter (not owned), or null for a
// device of its own (headless tools). screen: the GPU screen on that device,
// drawn into directly when it's the target. Null when there's no usable GPU.
std::unique_ptr<RenderBackend> makeGpuBackend(SDL_GPUDevice *device = nullptr, GpuScreen *screen = nullptr);

}  // namespace shogo

#endif
