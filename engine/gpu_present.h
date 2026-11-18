// Shows finished frames in the game window through SDL's GPU API (Metal on
// macOS), scaled to fit and letterboxed: either a CPU frame, uploaded each
// time, or a GPU texture (the GPU screen, gpu_screen.h).
#ifndef SHOGO_ENGINE_GPU_PRESENT_H
#define SHOGO_ENGINE_GPU_PRESENT_H

#include "engine/surfaces.h"

#include <SDL3/SDL.h>

namespace shogo {

class GpuPresenter {
public:
    ~GpuPresenter();
    // Claims the window for a new GPU device. False (with SDL_GetError) when
    // there's no usable GPU.
    bool init(SDL_Window *window);
    void setVSync(bool on);
    void present(const Surface &frame);
    // Records showing tex (w x h, letterboxed) into the window on cmd; the
    // caller submits cmd.
    void showTexture(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, int w, int h);
    const char *driver() const;
    SDL_GPUDevice *device() const { return m_device; }
    void shutdown();  // releases the window; safe to call twice

private:
    SDL_Window *m_window = nullptr;
    SDL_GPUDevice *m_device = nullptr;
    SDL_GPUTexture *m_frame = nullptr;
    SDL_GPUTransferBuffer *m_upload = nullptr;
    int m_w = 0, m_h = 0;
};

}  // namespace shogo

#endif
