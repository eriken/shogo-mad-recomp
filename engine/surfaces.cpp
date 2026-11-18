#include "engine/surfaces.h"

#include <algorithm>
#include <cmath>

namespace shogo {

uint64_t nextSurfaceId()
{
    static uint64_t next = 0;
    return ++next;
}

// ---------------------------------------------------------------------------
// PCX

std::optional<Surface> decodePCX(const std::vector<uint8_t> &d)
{
    if (d.size() < 128 || d[0] != 0x0A || d[2] != 1) return std::nullopt;  // ZSoft, RLE
    auto u16 = [&](size_t o) { return int(d[o] | d[o + 1] << 8); };
    int bpp = d[3], planes = d[65], bytesPerLine = u16(66);
    int w = u16(8) - u16(4) + 1, h = u16(10) - u16(6) + 1;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || bpp != 8 || (planes != 1 && planes != 3)) return std::nullopt;

    // Decode the RLE scanlines: each line is planes * bytesPerLine bytes.
    size_t lineBytes = size_t(planes) * size_t(bytesPerLine);
    std::vector<uint8_t> raw(lineBytes * size_t(h));
    size_t in = 128, out = 0;
    while (out < raw.size() && in < d.size()) {
        uint8_t b = d[in++];
        int count = 1;
        if ((b & 0xC0) == 0xC0) {
            count = b & 0x3F;
            if (in >= d.size()) break;
            b = d[in++];
        }
        while (count-- > 0 && out < raw.size()) raw[out++] = b;
    }

    Surface s(w, h);
    if (planes == 1) {
        // 256-color palette in the last 769 bytes, introduced by 0x0C.
        if (d.size() < 769 || d[d.size() - 769] != 0x0C) return std::nullopt;
        const uint8_t *pal = &d[d.size() - 768];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t *c = pal + 3 * raw[size_t(y) * lineBytes + size_t(x)];
                s.row(y)[x] = uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
            }
    } else {
        for (int y = 0; y < h; ++y) {
            const uint8_t *line = &raw[size_t(y) * lineBytes];
            for (int x = 0; x < w; ++x)
                s.row(y)[x] = uint32_t(line[x]) << 16 | uint32_t(line[bytesPerLine + x]) << 8 |
                              line[2 * bytesPerLine + x];
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Blits

namespace {
DRect whole(const Surface &s) { return DRect{0, 0, s.width, s.height}; }

DRect clampRect(DRect r, const Surface &s)
{
    r.left = std::max(r.left, 0);
    r.top = std::max(r.top, 0);
    r.right = std::min(r.right, s.width);
    r.bottom = std::min(r.bottom, s.height);
    return r;
}

inline void put(uint32_t &dst, uint32_t src, const BlitParams &p)
{
    switch (p.mode) {
    case BlitMode::Copy: dst = src; break;
    case BlitMode::Transparent:
        if (!sameColor565(src, p.key)) dst = src;
        break;
    case BlitMode::SolidColor:
        if (!sameColor565(src, p.key)) dst = p.fill;
        break;
    }
}
}  // namespace

void blit(Surface &dst, const Surface &src, const DRect *srcRect, int dx, int dy, const BlitParams &p)
{
    DRect r = clampRect(srcRect ? *srcRect : whole(src), src);
    // Shift the destination for any part of the source rect clamped away on the top/left.
    if (srcRect) {
        dx += r.left - srcRect->left;
        dy += r.top - srcRect->top;
    }
    int w = r.right - r.left, h = r.bottom - r.top;
    dst.touch();
    // Clip against the destination.
    if (dx < 0) { r.left -= dx; w += dx; dx = 0; }
    if (dy < 0) { r.top -= dy; h += dy; dy = 0; }
    w = std::min(w, dst.width - dx);
    h = std::min(h, dst.height - dy);
    for (int y = 0; y < h; ++y) {
        const uint32_t *s = src.row(r.top + y) + r.left;
        uint32_t *t = dst.row(dy + y) + dx;
        if (p.mode == BlitMode::Copy) {
            std::copy(s, s + w, t);
        } else {
            for (int x = 0; x < w; ++x) put(t[x], s[x], p);
        }
    }
}

void scaleBlit(Surface &dst, const Surface &src, const DRect *dstRect, const DRect *srcRect, const BlitParams &p)
{
    DRect sr = srcRect ? *srcRect : whole(src);
    DRect dr = dstRect ? *dstRect : whole(dst);
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top, dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    DRect clip = clampRect(dr, dst);
    dst.touch();
    for (int y = clip.top; y < clip.bottom; ++y) {
        int syy = sr.top + int((int64_t(y - dr.top) * sh) / dh);
        if (syy < 0 || syy >= src.height) continue;
        const uint32_t *s = src.row(syy);
        uint32_t *t = dst.row(y);
        for (int x = clip.left; x < clip.right; ++x) {
            int sxx = sr.left + int((int64_t(x - dr.left) * sw) / dw);
            if (sxx < 0 || sxx >= src.width) continue;
            put(t[x], s[sxx], p);
        }
    }
}

namespace {
// Fills the convex polygon (dest space) and, for each covered pixel centre, maps
// back to the source with the inverse of the affine map destToSrc.
template <class Map> void rasterConvex(Surface &dst, const DFloatPt *poly, int n, Map srcAt)
{
    float minY = poly[0].y, maxY = poly[0].y;
    for (int i = 1; i < n; ++i) {
        minY = std::min(minY, poly[i].y);
        maxY = std::max(maxY, poly[i].y);
    }
    int y0 = std::max(0, (int)std::ceil(minY - 0.5f)), y1 = std::min(dst.height - 1, (int)std::floor(maxY - 0.5f));
    for (int y = y0; y <= y1; ++y) {
        float cy = y + 0.5f, xl = 1e30f, xr = -1e30f;
        for (int i = 0; i < n; ++i) {
            DFloatPt a = poly[i], b = poly[(i + 1) % n];
            if ((cy < a.y) == (cy < b.y)) continue;  // edge doesn't cross this scanline
            float x = a.x + (cy - a.y) * (b.x - a.x) / (b.y - a.y);
            xl = std::min(xl, x);
            xr = std::max(xr, x);
        }
        if (xl > xr) continue;
        int x0 = std::max(0, (int)std::ceil(xl - 0.5f)), x1 = std::min(dst.width - 1, (int)std::floor(xr - 0.5f));
        for (int x = x0; x <= x1; ++x) srcAt(x, y, x + 0.5f, cy);
    }
}

struct Affine {
    // src = (a*x + b*y + c, d*x + e*y + f)
    float a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
};
}  // namespace

void warpBlit(Surface &dst, const Surface &src, const DWarpPt *pts, int n, const BlitParams &p)
{
    if (n < 3 || n > 10) return;
    // Solve the affine dest->source map from the first three points.
    float x0 = pts[0].dest_x, y0 = pts[0].dest_y, x1 = pts[1].dest_x, y1 = pts[1].dest_y, x2 = pts[2].dest_x,
          y2 = pts[2].dest_y;
    float det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(det) < 1e-6f) return;
    auto solve = [&](float s0, float s1, float s2, float &ka, float &kb, float &kc) {
        ka = ((s1 - s0) * (y2 - y0) - (s2 - s0) * (y1 - y0)) / det;
        kb = ((s2 - s0) * (x1 - x0) - (s1 - s0) * (x2 - x0)) / det;
        kc = s0 - ka * x0 - kb * y0;
    };
    Affine m;
    solve(pts[0].source_x, pts[1].source_x, pts[2].source_x, m.a, m.b, m.c);
    solve(pts[0].source_y, pts[1].source_y, pts[2].source_y, m.d, m.e, m.f);
    DFloatPt poly[10];
    for (int i = 0; i < n; ++i) poly[i] = DFloatPt{pts[i].dest_x, pts[i].dest_y};
    dst.touch();
    rasterConvex(dst, poly, n, [&](int x, int y, float fx, float fy) {
        int sx = std::clamp((int)(m.a * fx + m.b * fy + m.c), 0, src.width - 1);
        int sy = std::clamp((int)(m.d * fx + m.e * fy + m.f), 0, src.height - 1);
        put(dst.row(y)[x], src.row(sy)[sx], p);
    });
}

void transformBlit(Surface &dst, const Surface &src, const DFloatPt *origin, int dx, int dy, float angle, float sx,
                   float sy, const BlitParams &p)
{
    if (sx == 0 || sy == 0 || src.width == 0 || src.height == 0) return;
    // The origin is the pivot in destination coordinates; the source centre maps onto it,
    // and that pivot is placed at (dx, dy). With no origin, the centre is the pivot.
    float px = origin ? origin->x : (float)dx, py = origin ? origin->y : (float)dy;
    float cs = std::cos(angle), sn = std::sin(angle);
    float hw = src.width * 0.5f, hh = src.height * 0.5f;
    DFloatPt poly[4];
    const float cx[4] = {-hw, hw, hw, -hw}, cyy[4] = {-hh, -hh, hh, hh};
    for (int i = 0; i < 4; ++i) {
        float x = cx[i] * sx, y = cyy[i] * sy;
        poly[i] = DFloatPt{px + x * cs - y * sn, py + x * sn + y * cs};
    }
    dst.touch();
    rasterConvex(dst, poly, 4, [&](int x, int y, float fx, float fy) {
        float rx = fx - px, ry = fy - py;
        float ux = (rx * cs + ry * sn) / sx + hw, uy = (-rx * sn + ry * cs) / sy + hh;
        int ix = std::clamp((int)ux, 0, src.width - 1), iy = std::clamp((int)uy, 0, src.height - 1);
        put(dst.row(y)[x], src.row(iy)[ix], p);
    });
}

void maskedBlit(Surface &dst, const Surface &src, const Surface &mask, const DRect *srcRect, int dx, int dy,
                uint32_t key)
{
    if (mask.width == 0 || mask.height == 0) return;
    DRect r = clampRect(srcRect ? *srcRect : whole(src), src);
    dst.touch();
    for (int y = r.top; y < r.bottom; ++y) {
        int ty = dy + (y - r.top);
        if (ty < 0 || ty >= dst.height) continue;
        for (int x = r.left; x < r.right; ++x) {
            int tx = dx + (x - r.left);
            if (tx < 0 || tx >= dst.width) continue;
            if (sameColor565(src.row(y)[x], key)) continue;
            dst.row(ty)[tx] = mask.row((y - r.top) % mask.height)[(x - r.left) % mask.width];
        }
    }
}

void fillRect(Surface &dst, const DRect *rect, uint32_t color)
{
    DRect r = clampRect(rect ? *rect : whole(dst), dst);
    dst.touch();
    for (int y = r.top; y < r.bottom; ++y) std::fill(dst.row(y) + r.left, dst.row(y) + r.right, color);
}

DRect borderSize(const Surface &s, uint32_t color)
{
    auto rowIs = [&](int y) {
        for (int x = 0; x < s.width; ++x)
            if (!sameColor565(s.row(y)[x], color)) return false;
        return true;
    };
    auto colIs = [&](int x) {
        for (int y = 0; y < s.height; ++y)
            if (!sameColor565(s.row(y)[x], color)) return false;
        return true;
    };
    DRect b{0, 0, 0, 0};
    while (b.top < s.height && rowIs(b.top)) ++b.top;
    if (b.top == s.height) return DRect{0, 0, 0, 0};  // all border: report nothing
    while (rowIs(s.height - 1 - b.bottom)) ++b.bottom;
    while (colIs(b.left)) ++b.left;
    while (colIs(s.width - 1 - b.right)) ++b.right;
    return b;
}

}  // namespace shogo
