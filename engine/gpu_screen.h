// The screen on the GPU: a texture the 2D drawing (blits, scaling, warps,
// rotations, masks, fills) and the GPU world renderer both draw into, shown
// in the window without passing through the CPU.
//
// The CPU copy (screen.pixels) is read back only when something asks for it:
// reading pixels, CPU-only drawing such as text and movies, screenshots. CPU
// changes are uploaded again before the next GPU drawing. 2D drawing follows
// surfaces.cpp pixel for pixel: same clipping, same nearest source texel,
// same 565 colour key.
#ifndef SHOGO_ENGINE_GPU_SCREEN_H
#define SHOGO_ENGINE_GPU_SCREEN_H

#include "engine/screen_target.h"

#include <memory>
#include <unordered_map>

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUTransferBuffer;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPURenderPass;
struct SDL_GPUSampler;
struct SDL_GPUFence;

namespace shogo {

class GpuPresenter;

class GpuScreen final : public ScreenTarget {
public:
    // presenter: shows frames in its window and lends its device; null for
    // headless use (a device of its own, frames aren't shown). Null when
    // there's no usable GPU.
    static std::unique_ptr<GpuScreen> create(GpuPresenter *presenter);
    ~GpuScreen() override;

    SDL_GPUDevice *device() const { return m_device; }

    void fill(Surface &screen, const DRect *rect, uint32_t color) override;
    void blit(Surface &screen, const Surface &src, const DRect *srcRect, int dx, int dy, const BlitParams &p) override;
    void scaleBlit(Surface &screen, const Surface &src, const DRect *dstRect, const DRect *srcRect,
                   const BlitParams &p) override;
    void warpBlit(Surface &screen, const Surface &src, const DWarpPt *pts, int n, const BlitParams &p) override;
    void transformBlit(Surface &screen, const Surface &src, const DFloatPt *origin, int dx, int dy, float angle,
                       float sx, float sy, const BlitParams &p) override;
    void maskedBlit(Surface &screen, const Surface &src, const Surface &mask, const DRect *srcRect, int dx, int dy,
                    uint32_t key) override;
    void toCPU(Surface &screen) override;
    void cpuWrites(Surface &screen) override;
    void present(Surface &screen) override;

    // For the GPU world renderer, drawing straight into the screen: true when
    // s is the screen this draws to (the last one it saw).
    bool isScreen(const Surface &s) const { return m_screenId != 0 && s.id == m_screenId; }
    // Hands out the frame's command buffer and the screen texture, current
    // and with no 2D pass open; what's drawn into it counts from then on.
    SDL_GPUCommandBuffer *beginExternal(Surface &screen, SDL_GPUTexture **texture);
    // Adds rgb (0..255 each, saturating) inside rect, like the CPU screen tint.
    void add(Surface &screen, const DRect &rect, const uint32_t rgb[3]);

private:
    struct Op;
    struct Cached {
        SDL_GPUTexture *tex = nullptr;
        uint64_t version = 0;
        int w = 0, h = 0;
        uint64_t lastUse = 0;
    };

    bool init(GpuPresenter *presenter);
    SDL_GPUCommandBuffer *cmd();
    void endPass();
    void sync(Surface &screen);  // the screen texture exists and is current
    SDL_GPUTexture *source(const Surface &s);
    void draw(Surface &screen, const DRect &rect, const Op &op, const Surface *src, const Surface *mask,
              bool additive = false);
    void upload(SDL_GPUTexture *tex, const Surface &s);

    enum class State { CPU, GPU, Both };  // which copy of the screen is current

    GpuPresenter *m_presenter = nullptr;
    SDL_GPUDevice *m_device = nullptr;
    bool m_ownsDevice = false;
    SDL_GPUGraphicsPipeline *m_copy = nullptr, *m_add = nullptr;
    SDL_GPUSampler *m_sampler = nullptr;
    SDL_GPUTexture *m_dummy = nullptr;

    SDL_GPUTexture *m_tex = nullptr;
    SDL_GPUTransferBuffer *m_up = nullptr, *m_down = nullptr;
    uint64_t m_screenId = 0;
    int m_w = 0, m_h = 0;
    State m_state = State::CPU;

    SDL_GPUCommandBuffer *m_cmd = nullptr;
    SDL_GPURenderPass *m_pass = nullptr;
    SDL_GPUGraphicsPipeline *m_bound = nullptr;
    SDL_GPUFence *m_lastFrame = nullptr;
    uint64_t m_frame = 0;
    // SHOGO_TRACE_SCREEN: per-frame counts, printed at present.
    bool m_trace = false;
    int m_draws = 0, m_uploads = 0, m_readbacks = 0, m_screenUploads = 0;
    std::unordered_map<uint64_t, Cached> m_cache;  // by surface id
};

}  // namespace shogo

#endif
