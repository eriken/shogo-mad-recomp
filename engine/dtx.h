// LithTech 1.0 textures (.DTX, version -2): 8-bit palettized with mipmaps.
#ifndef SHOGO_ENGINE_DTX_H
#define SHOGO_ENGINE_DTX_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shogo {

struct Texture {
    int width = 0, height = 0;
    uint32_t flags = 0;
    // Mip levels, each 0x00RRGGBB per texel; [0] is full size.
    std::vector<std::vector<uint32_t>> mips;
    uint32_t palette[256] = {};
    std::vector<std::vector<uint8_t>> indices;  // palette indices per mip
    // Per-texel alpha (0..255) per mip, when the file has an alpha mask: 4 bits
    // per texel for every mip level, low nibble first, after the color data.
    std::vector<std::vector<uint8_t>> alpha;
};

std::optional<Texture> decodeDTX(const std::vector<uint8_t> &data, std::string *err = nullptr);

}  // namespace shogo

#endif
