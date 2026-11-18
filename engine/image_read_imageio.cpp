#include "engine/image_read.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>

namespace shogo {

bool decodeImageRGBA(const std::vector<uint8_t> &data, int &width, int &height, std::vector<uint8_t> &rgba)
{
    CFDataRef cf = CFDataCreate(nullptr, data.data(), CFIndex(data.size()));
    CGImageSourceRef src = cf ? CGImageSourceCreateWithData(cf, nullptr) : nullptr;
    if (cf) CFRelease(cf);
    if (!src) return false;
    CGImageRef best = nullptr;
    for (size_t i = 0, n = CGImageSourceGetCount(src); i < n; ++i) {
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, i, nullptr);
        if (!img) continue;
        if (!best || CGImageGetWidth(img) > CGImageGetWidth(best)) {
            if (best) CGImageRelease(best);
            best = img;
        } else {
            CGImageRelease(img);
        }
    }
    CFRelease(src);
    if (!best) return false;
    width = int(CGImageGetWidth(best));
    height = int(CGImageGetHeight(best));
    rgba.assign(size_t(width) * size_t(height) * 4, 0);
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(), size_t(width), size_t(height), 8, size_t(width) * 4, rgb,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(rgb);
    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), best);
        CGContextRelease(ctx);
    }
    CGImageRelease(best);
    if (!ctx) return false;
    for (size_t i = 0; i < rgba.size(); i += 4)  // back to straight alpha
        if (uint8_t a = rgba[i + 3]; a && a < 255)
            for (int c = 0; c < 3; ++c) rgba[i + c] = uint8_t(std::min(255, rgba[i + c] * 255 / a));
    return true;
}

}  // namespace shogo
