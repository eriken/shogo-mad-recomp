// Saving surfaces to image files (screenshots, debugging).
#ifndef SHOGO_ENGINE_IMAGE_WRITE_H
#define SHOGO_ENGINE_IMAGE_WRITE_H

#include "engine/surfaces.h"

#include <string>

namespace shogo {
bool writePNG(const Surface &s, const std::string &path);
}

#endif
