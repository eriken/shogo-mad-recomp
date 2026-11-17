// System fonts for the ClientDE font API (HDEFONT), rendered with Core Text.
//
// The game asks for Windows GDI fonts by name and pixel size. Text is rasterized
// without anti-aliasing, as 1998 GDI did, so every text pixel is exactly the
// foreground color and the game's color-key transparency works around it.
#ifndef SHOGO_ENGINE_FONTS_H
#define SHOGO_ENGINE_FONTS_H

#include "engine/surfaces.h"

#include <memory>
#include <string>

namespace shogo {

class Font {
public:
    static constexpr uint32_t kMagic = 0x464E5431;  // "FNT1"
    uint32_t magic = kMagic;

    // width/height follow GDI CreateFont: height = character cell height in pixels,
    // width = average character width (0 = natural proportions).
    static std::unique_ptr<Font> create(const std::string &name, int width, int height, bool italic, bool underline,
                                        bool bold);
    ~Font();

    // Size of the text (multiple lines allowed, separated by '\n').
    void measure(const char *text, int *w, int *h) const;
    // Draws text with its top-left at (x, y), clipped to clip (null = whole surface).
    void draw(Surface &dst, const char *text, int x, int y, uint32_t color, const DRect *clip) const;

private:
    Font() = default;
    struct Impl;
    Impl *m = nullptr;
};

inline Font *toFont(HDEFONT h)
{
    auto *f = reinterpret_cast<Font *>(h);
    return (f && f->magic == Font::kMagic) ? f : nullptr;
}

}  // namespace shogo

#endif
