// Software 2D surfaces behind the ClientDE surface API (HSURFACE).
//
// Pixels are 32-bit 0x00RRGGBB, the same layout as HDECOLOR. Color-key
// transparency compares colors at RGB565 precision, as the original engine did
// in its 16-bit display modes, so keys still match after palette rounding.
// Rectangles exclude their right and bottom edges (API convention).
#ifndef SHOGO_ENGINE_SURFACES_H
#define SHOGO_ENGINE_SURFACES_H

#include "engine/api.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace shogo {

struct Surface {
    static constexpr uint32_t kMagic = 0x53524631;  // "SRF1"
    uint32_t magic = kMagic;
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;  // row-major, width*height
    void *userData = nullptr;
    bool optimized = false;
    HDECOLOR optimizedKey = 0;

    Surface(int w, int h) : width(w), height(h), pixels(size_t(w) * size_t(h), 0) {}
    uint32_t *row(int y) { return &pixels[size_t(y) * size_t(width)]; }
    const uint32_t *row(int y) const { return &pixels[size_t(y) * size_t(width)]; }
};

inline Surface *toSurface(HSURFACE h)
{
    auto *s = reinterpret_cast<Surface *>(h);
    return (s && s->magic == Surface::kMagic) ? s : nullptr;
}
inline HSURFACE toHandle(Surface *s) { return reinterpret_cast<HSURFACE>(s); }

// RGB only (drops the transparency flag).
inline uint32_t rgbOf(HDECOLOR c) { return c & 0x00FFFFFF; }
// True if two colors are equal at RGB565 precision.
inline bool sameColor565(uint32_t a, uint32_t b) { return ((a ^ b) & 0x00F8FCF8) == 0; }

// How source pixels are written.
enum class BlitMode {
    Copy,         // every pixel
    Transparent,  // skip pixels matching the key color
    SolidColor,   // skip key-color pixels, paint the rest with the fill color
};

struct BlitParams {
    BlitMode mode = BlitMode::Copy;
    uint32_t key = 0;
    uint32_t fill = 0;
};

// Decodes an 8-bit (palette) or 24-bit PCX file into a new surface.
std::optional<Surface> decodePCX(const std::vector<uint8_t> &data);

// All operations clip against both surfaces. A null rect means the whole surface.
void blit(Surface &dst, const Surface &src, const DRect *srcRect, int dx, int dy, const BlitParams &p);
void scaleBlit(Surface &dst, const Surface &src, const DRect *dstRect, const DRect *srcRect, const BlitParams &p);
// Affine warp of a convex source polygon onto a destination polygon (3..10 points).
void warpBlit(Surface &dst, const Surface &src, const DWarpPt *pts, int n, const BlitParams &p);
// Rotate (radians) and scale src around origin (dest coords; null = src centre placed at dx,dy).
void transformBlit(Surface &dst, const Surface &src, const DFloatPt *origin, int dx, int dy, float angle, float sx,
                   float sy, const BlitParams &p);
// Non-key pixels of src are replaced by mask pixels tiled from the destination position.
void maskedBlit(Surface &dst, const Surface &src, const Surface &mask, const DRect *srcRect, int dx, int dy,
                uint32_t key);
void fillRect(Surface &dst, const DRect *rect, uint32_t color);
// Border widths (left, top, right, bottom) of solid `color` around the surface's content.
DRect borderSize(const Surface &s, uint32_t color);

}  // namespace shogo

#endif
