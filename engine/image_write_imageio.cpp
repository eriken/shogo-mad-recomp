#include "engine/image_write.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

namespace shogo {

bool writePNG(const Surface &s, const std::string &path)
{
    if (s.width <= 0 || s.height <= 0) return false;
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    // 0x00RRGGBB in a little-endian uint32 = B,G,R,X bytes.
    CGContextRef ctx = CGBitmapContextCreate((void *)s.pixels.data(), (size_t)s.width, (size_t)s.height, 8,
                                             (size_t)s.width * 4, rgb,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(rgb);
    if (!ctx) return false;
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), (CFIndex)path.size(),
                                                           false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dest) {
        CGImageDestinationAddImage(dest, img, nullptr);
        ok = CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CFRelease(url);
    CGImageRelease(img);
    return ok;
}

}  // namespace shogo
