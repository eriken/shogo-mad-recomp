#include "engine/gpu_present.h"

#include <algorithm>
#include <cstring>

namespace shogo {

GpuPresenter::~GpuPresenter() { shutdown(); }

bool GpuPresenter::init(SDL_Window *window)
{
    shutdown();
    // No shaders yet; ask for the formats the GPU renderer will bring.
    m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr);
    if (!m_device) return false;
    if (!SDL_ClaimWindowForGPUDevice(m_device, window)) {
        SDL_DestroyGPUDevice(m_device);
        m_device = nullptr;
        return false;
    }
    m_window = window;
    return true;
}

void GpuPresenter::shutdown()
{
    if (!m_device) return;
    SDL_WaitForGPUIdle(m_device);
    if (m_upload) SDL_ReleaseGPUTransferBuffer(m_device, m_upload);
    if (m_frame) SDL_ReleaseGPUTexture(m_device, m_frame);
    SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
    SDL_DestroyGPUDevice(m_device);
    m_upload = nullptr;
    m_frame = nullptr;
    m_device = nullptr;
    m_window = nullptr;
    m_w = m_h = 0;
}

const char *GpuPresenter::driver() const { return m_device ? SDL_GetGPUDeviceDriver(m_device) : "none"; }

void GpuPresenter::setVSync(bool on)
{
    if (!m_device) return;
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!on) {
        if (SDL_WindowSupportsGPUPresentMode(m_device, m_window, SDL_GPU_PRESENTMODE_IMMEDIATE))
            mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        else if (SDL_WindowSupportsGPUPresentMode(m_device, m_window, SDL_GPU_PRESENTMODE_MAILBOX))
            mode = SDL_GPU_PRESENTMODE_MAILBOX;
    }
    SDL_SetGPUSwapchainParameters(m_device, m_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
}

void GpuPresenter::present(const Surface &s)
{
    if (!m_device || s.width <= 0 || s.height <= 0) return;
    const Uint32 bytes = Uint32(s.width) * Uint32(s.height) * 4;
    if (!m_frame || m_w != s.width || m_h != s.height) {
        SDL_WaitForGPUIdle(m_device);
        if (m_frame) SDL_ReleaseGPUTexture(m_device, m_frame);
        if (m_upload) SDL_ReleaseGPUTransferBuffer(m_device, m_upload);
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;  // XRGB8888 in memory
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = Uint32(s.width);
        ti.height = Uint32(s.height);
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        m_frame = SDL_CreateGPUTexture(m_device, &ti);
        SDL_GPUTransferBufferCreateInfo bi{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
        m_upload = SDL_CreateGPUTransferBuffer(m_device, &bi);
        if (!m_frame || !m_upload) {
            SDL_Log("GPU frame texture: %s", SDL_GetError());
            return;
        }
        m_w = s.width;
        m_h = s.height;
    }

    // cycle = true: the GPU may still be reading last frame's buffer.
    if (void *p = SDL_MapGPUTransferBuffer(m_device, m_upload, true)) {
        memcpy(p, s.pixels.data(), bytes);
        SDL_UnmapGPUTransferBuffer(m_device, m_upload);
    }

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
    if (!cmd) return;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{m_upload, 0, Uint32(s.width), Uint32(s.height)};
    SDL_GPUTextureRegion dst{};
    dst.texture = m_frame;
    dst.w = Uint32(s.width);
    dst.h = Uint32(s.height);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, true);
    SDL_EndGPUCopyPass(copy);

    showTexture(cmd, m_frame, s.width, s.height);
    SDL_SubmitGPUCommandBuffer(cmd);
}

void GpuPresenter::showTexture(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, int w, int h)
{
    if (!m_window || w <= 0 || h <= 0) return;
    SDL_GPUTexture *swap = nullptr;
    Uint32 sw = 0, sh = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, m_window, &swap, &sw, &sh) && swap) {  // null: minimised
        // Black bars: clear the whole target, then blit the frame centred at
        // the largest size that keeps its aspect.
        SDL_GPUColorTargetInfo ct{};
        ct.texture = swap;
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        ct.clear_color = {0, 0, 0, 1};
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr));

        const float scale = std::min(float(sw) / float(w), float(sh) / float(h));
        const Uint32 bw = Uint32(float(w) * scale), bh = Uint32(float(h) * scale);
        SDL_GPUBlitInfo blit{};
        blit.source.texture = tex;
        blit.source.w = Uint32(w);
        blit.source.h = Uint32(h);
        blit.destination.texture = swap;
        blit.destination.x = (sw - bw) / 2;
        blit.destination.y = (sh - bh) / 2;
        blit.destination.w = bw;
        blit.destination.h = bh;
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd, &blit);
    }
}

}  // namespace shogo
