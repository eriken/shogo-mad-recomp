#include "engine/fonts.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace shogo {

struct Font::Impl {
    CTFontRef font = nullptr;
    float scaleX = 1.0f;  // horizontal stretch to honour a requested average width
    int ascent = 0, descent = 0, lineHeight = 0;
    bool underline = false;
};

namespace {
CFStringRef cfString(const char *s)
{
    return CFStringCreateWithCString(nullptr, s, kCFStringEncodingWindowsLatin1);
}

CTFontRef makeFont(const std::string &name, double size, bool italic, bool bold)
{
    CFStringRef cfName = cfString(name.empty() ? "Arial" : name.c_str());
    CTFontRef base = CTFontCreateWithName(cfName, size, nullptr);
    CFRelease(cfName);
    CTFontSymbolicTraits traits = (italic ? kCTFontItalicTrait : 0) | (bold ? kCTFontBoldTrait : 0);
    if (!traits) return base;
    CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(base, size, nullptr, traits, traits);
    if (!styled) return base;
    CFRelease(base);
    return styled;
}

// Runs fn(line, lineIndex) for each '\n'-separated line.
template <class Fn> void forEachLine(const char *text, Fn fn)
{
    int index = 0;
    const char *start = text;
    for (const char *p = text;; ++p) {
        if (*p == '\n' || *p == '\0') {
            std::string line(start, p);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            fn(line, index++);
            if (*p == '\0') break;
            start = p + 1;
        }
    }
}

CTLineRef makeLine(CTFontRef font, const std::string &text)
{
    CFStringRef str = CFStringCreateWithBytes(nullptr, (const UInt8 *)text.data(), (CFIndex)text.size(),
                                              kCFStringEncodingWindowsLatin1, false);
    if (!str) str = CFSTR("");
    // Glyphs take the context's fill colour (white into the coverage mask);
    // without this Core Text draws them black, which leaves the mask empty.
    const void *keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
    const void *values[] = {font, kCFBooleanTrue};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(str);
    return line;
}
}  // namespace

std::unique_ptr<Font> Font::create(const std::string &name, int width, int height, bool italic, bool underline,
                                   bool bold)
{
    std::unique_ptr<Font> f(new Font);
    f->m = new Impl;
    f->m->underline = underline;
    // GDI: positive height is the cell height (ascent + descent); negative is the em height.
    int cell = std::abs(height ? height : 16);
    CTFontRef probe = makeFont(name, 100.0, italic, bold);
    double cellPer100 = CTFontGetAscent(probe) + CTFontGetDescent(probe);
    double size = height < 0 ? cell : cell * 100.0 / cellPer100;
    CFRelease(probe);
    f->m->font = makeFont(name, size, italic, bold);
    f->m->ascent = (int)std::lround(CTFontGetAscent(f->m->font));
    f->m->descent = (int)std::lround(CTFontGetDescent(f->m->font));
    f->m->lineHeight = std::max(cell, f->m->ascent + f->m->descent);
    if (width > 0) {
        // GDI's average character width is roughly the mean lowercase advance.
        CTLineRef line = makeLine(f->m->font, "abcdefghijklmnopqrstuvwxyz");
        double avg = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) / 26.0;
        CFRelease(line);
        if (avg > 0) f->m->scaleX = (float)(width / avg);
    }
    return f;
}

Font::~Font()
{
    magic = 0;
    if (m && m->font) CFRelease(m->font);
    delete m;
}

void Font::measure(const char *text, int *w, int *h) const
{
    int maxW = 0, lines = 0;
    forEachLine(text ? text : "", [&](const std::string &s, int) {
        CTLineRef line = makeLine(m->font, s);
        double width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) * m->scaleX;
        CFRelease(line);
        maxW = std::max(maxW, (int)std::ceil(width));
        ++lines;
    });
    if (w) *w = maxW;
    if (h) *h = lines * m->lineHeight;
}

void Font::draw(Surface &dst, const char *text, int x, int y, uint32_t color, const DRect *clip) const
{
    dst.touch();
    int tw = 0, th = 0;
    measure(text, &tw, &th);
    if (tw <= 0 || th <= 0) return;

    // Rasterize into an 8-bit coverage mask, no anti-aliasing.
    std::vector<uint8_t> mask(size_t(tw) * size_t(th), 0);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(mask.data(), (size_t)tw, (size_t)th, 8, (size_t)tw, gray, kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    CGContextSetShouldAntialias(ctx, false);
    CGContextSetAllowsAntialiasing(ctx, false);
    CGContextSetShouldSmoothFonts(ctx, false);
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);
    forEachLine(text, [&](const std::string &s, int index) {
        CTLineRef line = makeLine(m->font, s);
        // Core Graphics' origin is bottom-left; row 0 of the buffer is the top.
        float baseline = (float)(th - index * m->lineHeight - m->ascent);
        CGContextSaveGState(ctx);
        CGContextScaleCTM(ctx, m->scaleX, 1.0);
        CGContextSetTextPosition(ctx, 0, baseline);
        CTLineDraw(line, ctx);
        CGContextRestoreGState(ctx);
        if (m->underline) {
            double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) * m->scaleX;
            CGContextFillRect(ctx, CGRectMake(0, baseline - 2, w, 1));
        }
        CFRelease(line);
    });
    CGContextRelease(ctx);

    DRect c = clip ? *clip : DRect{0, 0, dst.width, dst.height};
    c.left = std::max(c.left, 0);
    c.top = std::max(c.top, 0);
    c.right = std::min(c.right, dst.width);
    c.bottom = std::min(c.bottom, dst.height);
    for (int my = 0; my < th; ++my) {
        int ty = y + my;
        if (ty < c.top || ty >= c.bottom) continue;
        const uint8_t *row = &mask[size_t(my) * size_t(tw)];
        for (int mx = 0; mx < tw; ++mx) {
            int tx = x + mx;
            if (row[mx] >= 128 && tx >= c.left && tx < c.right) dst.row(ty)[tx] = color;
        }
    }
}

}  // namespace shogo
