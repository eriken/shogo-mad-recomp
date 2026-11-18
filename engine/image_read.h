// Reading image files (PNG, ICO...) through ImageIO: the Dock icon from the
// player's game data.
#ifndef SHOGO_ENGINE_IMAGE_READ_H
#define SHOGO_ENGINE_IMAGE_READ_H

#include <cstdint>
#include <vector>

namespace shogo {

// Decodes the largest image in `data` (an .ico holds several sizes) to
// straight-alpha RGBA, row by row.
bool decodeImageRGBA(const std::vector<uint8_t> &data, int &width, int &height, std::vector<uint8_t> &rgba);

}  // namespace shogo

#endif
