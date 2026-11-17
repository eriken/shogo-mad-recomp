#include "engine/dtx.h"

#include <cstring>

namespace shogo {

namespace {
uint32_t rd32(const std::vector<uint8_t> &d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }
uint16_t rd16(const std::vector<uint8_t> &d, size_t o) { uint16_t v; memcpy(&v, &d[o], 2); return v; }
}  // namespace

std::optional<Texture> decodeDTX(const std::vector<uint8_t> &d, std::string *err)
{
    auto fail = [&](const char *why) -> std::optional<Texture> {
        if (err) *err = why;
        return std::nullopt;
    };
    constexpr size_t kHeader = 44, kPalette = 1024;
    if (d.size() < kHeader + kPalette) return fail("too small");
    if (rd32(d, 0) != 0 || (int32_t)rd32(d, 4) != -2) return fail("not a version -2 DTX");
    Texture t;
    t.width = rd16(d, 8);
    t.height = rd16(d, 10);
    int mips = rd16(d, 12);
    t.flags = rd32(d, 16);
    if (t.width <= 0 || t.height <= 0 || t.width > 4096 || t.height > 4096 || mips < 1 || mips > 12)
        return fail("bad dimensions");
    // Palette entries are 4 bytes: a flag byte, then R, G, B.
    for (int i = 0; i < 256; ++i) {
        const uint8_t *e = &d[kHeader + 4 * size_t(i)];
        t.palette[i] = uint32_t(e[1]) << 16 | uint32_t(e[2]) << 8 | e[3];
    }
    size_t p = kHeader + kPalette;
    int w = t.width, h = t.height;
    for (int m = 0; m < mips && w > 0 && h > 0; ++m) {
        size_t n = size_t(w) * size_t(h);
        if (p + n > d.size()) {
            if (m == 0) return fail("truncated");
            break;
        }
        t.indices.emplace_back(d.begin() + p, d.begin() + p + n);
        std::vector<uint32_t> px(n);
        for (size_t i = 0; i < n; ++i) px[i] = t.palette[d[p + i]];
        t.mips.push_back(std::move(px));
        p += n;
        w /= 2;
        h /= 2;
    }
    // Sprite and effect textures carry a 4-bit alpha mask after the mips.
    size_t need = 0;
    for (const auto &m : t.indices) need += (m.size() + 1) / 2;
    if (t.indices.size() == size_t(mips) && d.size() - p == need) {
        for (const auto &m : t.indices) {
            std::vector<uint8_t> a(m.size());
            for (size_t i = 0; i < a.size(); ++i) {
                uint8_t b = d[p + i / 2];
                a[i] = uint8_t(((i & 1) ? b >> 4 : b & 15) * 17);
            }
            p += (m.size() + 1) / 2;
            t.alpha.push_back(std::move(a));
        }
    }
    return t;
}

}  // namespace shogo
