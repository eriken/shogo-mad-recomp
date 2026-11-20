#include "engine/client_api.h"

#include "engine/movie.h"
#include "engine/stub_support.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace shogo {

namespace {
ClientContext *g = nullptr;

uint32_t color(HDECOLOR c) { return rgbOf(c); }  // null (0) is black, per the API

const Font *fontOr(HDEFONT h)
{
    if (const Font *f = toFont(h)) return f;
    if (!g->defaultFont) g->defaultFont = Font::create("Arial", 0, 16, false, false, false);
    return g->defaultFont.get();
}

const char *text(HSTRING s)
{
    const char *t = g->strings ? g->strings->data(s) : nullptr;
    return t ? t : "";
}

std::optional<Surface> loadBitmap(const char *name)
{
    if (!name || !g->resources) return std::nullopt;
    auto data = g->resources->read(name);
    if (!data) {
        fprintf(stderr, "[surfaces] bitmap not found: %s\n", name);
        return std::nullopt;
    }
    auto s = decodePCX(*data);
    if (!s) fprintf(stderr, "[surfaces] not a supported PCX: %s\n", name);
    return s;
}

HSURFACE newSurface(Surface s) { return toHandle(new Surface(std::move(s))); }

// ---- console / lifecycle --------------------------------------------------

void printConsole(char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    printf("[console] ");
    vprintf(msg, ap);
    printf("\n");
    va_end(ap);
}

void shutdownWithMessage(char *msg, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);
    printf("[ShutdownWithMessage] %s\n", buf);
    g->quitRequested = true;
    g->quitMessage = buf;
}

// ---- render modes / frame -------------------------------------------------

void fillMode(RMode &m, int w, int h, const std::string &renderer)
{
    memset(&m, 0, sizeof(m));
    m.m_bHardware = DTRUE;
    strcpy(m.m_RenderDLL, "native");
    strcpy(m.m_InternalName, "native");
    snprintf(m.m_Description, sizeof m.m_Description, "%s", renderer.c_str());
    m.m_Width = (DDWORD)w;
    m.m_Height = (DDWORD)h;
    m.m_BitDepth = 16;
}

// The engine picks the resolution (the host's; the display's shape), so the
// game is offered just that one, and its own choices keep it.
RMode *getRenderModes()
{
    RMode *m = new RMode;
    fillMode(*m, g->screen->width, g->screen->height, g->rendererName);
    m->m_pNext = nullptr;
    return m;
}

void relinquishRenderModes(RMode *m)
{
    while (m) {
        RMode *next = m->m_pNext;
        delete m;
        m = next;
    }
}

DRESULT getRenderMode(RMode *m)
{
    if (!m) return LT_ERROR;
    fillMode(*m, g->screen->width, g->screen->height, g->rendererName);
    return LT_OK;
}

DRESULT setRenderMode(RMode *m) { return m ? LT_OK : LT_ERROR; }

// ---- the screen on the GPU ----------------------------------------------------

// The GPU screen, when s is the screen and it lives there.
ScreenTarget *gpuScreen(const Surface *s)
{
    return g->screenTarget && s && s == g->screen.get() ? g->screenTarget : nullptr;
}

// A surface the CPU is about to read: the screen's CPU copy is brought up to date.
Surface &forRead(Surface &s)
{
    if (ScreenTarget *t = gpuScreen(&s)) t->toCPU(s);
    return s;
}

// A surface the CPU is about to draw on: for the screen, its CPU copy, which
// then counts until the next GPU drawing.
Surface &forCPUWrite(Surface &s)
{
    if (ScreenTarget *t = gpuScreen(&s)) {
        t->toCPU(s);
        t->cpuWrites(s);
    }
    return s;
}

// A 2D operation from src onto dst: on the GPU when dst is the GPU screen
// (and src isn't), otherwise with the CPU.
template <class Gpu, class Cpu> DRESULT draw2D(Surface *dst, Surface *src, Gpu onGpu, Cpu onCpu)
{
    if (!dst || !src) return LT_ERROR;
    forRead(*src);
    ScreenTarget *t = gpuScreen(dst);
    if (t && src != dst) onGpu(*t, *dst, *src);
    else onCpu(forCPUWrite(*dst), *src);
    return LT_OK;
}

DRESULT flipScreen(DDWORD)
{
    ++g->framesPresented;
    if (g->screenTarget) g->screenTarget->present(*g->screen);
    else if (g->present) g->present(*g->screen);
    return LT_OK;
}

DRESULT clearScreen(DRect *rect, DDWORD flags)
{
    if (!(flags & CLEARSCREEN_SCREEN)) return LT_OK;
    if (ScreenTarget *t = gpuScreen(g->screen.get())) t->fill(*g->screen, rect, 0);
    else fillRect(*g->screen, rect, 0);
    return LT_OK;
}

DRESULT ok() { return LT_OK; }

// ---- colors ------------------------------------------------------------------

HDECOLOR makeColor(float r, float gg, float b, DBOOL transparent)
{
    auto c = [](float v) { return (unsigned)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
    HDECOLOR col = SETRGB(c(r), c(gg), c(b));
    return transparent ? (col | COLOR_TRANSPARENCY_MASK) : col;
}

// ---- surfaces ----------------------------------------------------------------

HSURFACE getScreenSurface() { return toHandle(g->screen.get()); }

HSURFACE createSurfaceFromBitmap(char *name)
{
    auto s = loadBitmap(name);
    return s ? newSurface(std::move(*s)) : nullptr;
}

HSURFACE createSurface(DDWORD w, DDWORD h)
{
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return nullptr;
    return newSurface(Surface((int)w, (int)h));
}

DRESULT deleteSurface(HSURFACE h)
{
    Surface *s = toSurface(h);
    if (!s || s == g->screen.get()) return LT_ERROR;
    s->magic = 0;
    delete s;
    return LT_OK;
}

void *getSurfaceUserData(HSURFACE h)
{
    Surface *s = toSurface(h);
    return s ? s->userData : nullptr;
}

void setSurfaceUserData(HSURFACE h, void *data)
{
    if (Surface *s = toSurface(h)) s->userData = data;
}

void getSurfaceDims(HSURFACE h, DDWORD *w, DDWORD *hgt)
{
    Surface *s = toSurface(h);
    if (w) *w = s ? (DDWORD)s->width : 0;
    if (hgt) *hgt = s ? (DDWORD)s->height : 0;
}

DRESULT getPixel(HSURFACE h, DDWORD x, DDWORD y, HDECOLOR *out)
{
    Surface *s = toSurface(h);
    if (!s || !out || x >= (DDWORD)s->width || y >= (DDWORD)s->height) return LT_ERROR;
    *out = forRead(*s).row((int)y)[x];
    return LT_OK;
}

DRESULT setPixel(HSURFACE h, DDWORD x, DDWORD y, HDECOLOR c)
{
    Surface *s = toSurface(h);
    if (!s || x >= (DDWORD)s->width || y >= (DDWORD)s->height) return LT_ERROR;
    forCPUWrite(*s).row((int)y)[x] = color(c);
    s->touch();
    return LT_OK;
}

DRESULT optimizeSurface(HSURFACE h, HDECOLOR key)
{
    Surface *s = toSurface(h);
    if (!s) return LT_ERROR;
    s->optimized = true;
    s->optimizedKey = key;
    return LT_OK;
}

DRESULT unoptimizeSurface(HSURFACE h)
{
    Surface *s = toSurface(h);
    if (!s) return LT_ERROR;
    s->optimized = false;
    return LT_OK;
}

DRESULT getBorderSize(HSURFACE h, HDECOLOR c, DRect *out)
{
    Surface *s = toSurface(h);
    if (!s || !out) return LT_ERROR;
    *out = borderSize(forRead(*s), color(c));
    return LT_OK;
}

BlitParams copyMode() { return BlitParams{}; }
BlitParams keyMode(HDECOLOR key) { return BlitParams{BlitMode::Transparent, color(key), 0}; }
BlitParams solidMode(HDECOLOR key, HDECOLOR fill) { return BlitParams{BlitMode::SolidColor, color(key), color(fill)}; }

DRESULT drawSurfaceToSurface(HSURFACE dst, HSURFACE src, DRect *r, int x, int y)
{
    return draw2D(
        toSurface(dst), toSurface(src), [&](ScreenTarget &t, Surface &d, Surface &s) { t.blit(d, s, r, x, y, copyMode()); },
        [&](Surface &d, Surface &s) { blit(d, s, r, x, y, copyMode()); });
}

DRESULT drawSurfaceToSurfaceTransparent(HSURFACE dst, HSURFACE src, DRect *r, int x, int y, HDECOLOR key)
{
    return draw2D(
        toSurface(dst), toSurface(src), [&](ScreenTarget &t, Surface &d, Surface &s) { t.blit(d, s, r, x, y, keyMode(key)); },
        [&](Surface &d, Surface &s) { blit(d, s, r, x, y, keyMode(key)); });
}

DRESULT drawSurfaceSolidColor(HSURFACE dst, HSURFACE src, DRect *r, int x, int y, HDECOLOR key, HDECOLOR fill)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.blit(d, s, r, x, y, solidMode(key, fill)); },
        [&](Surface &d, Surface &s) { blit(d, s, r, x, y, solidMode(key, fill)); });
}

DRESULT drawSurfaceMasked(HSURFACE dst, HSURFACE src, HSURFACE mask, DRect *r, int x, int y, HDECOLOR key)
{
    Surface *m = toSurface(mask);
    if (!m) return LT_ERROR;
    forRead(*m);
    if (m == toSurface(dst)) {  // the mask is the destination itself: on the CPU
        forCPUWrite(*m);
        return draw2D(
            toSurface(dst), toSurface(src), [](ScreenTarget &, Surface &, Surface &) {},
            [&](Surface &d, Surface &s) { maskedBlit(d, s, *m, r, x, y, color(key)); });
    }
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.maskedBlit(d, s, *m, r, x, y, color(key)); },
        [&](Surface &d, Surface &s) { maskedBlit(d, s, *m, r, x, y, color(key)); });
}

DRESULT scaleSurfaceToSurface(HSURFACE dst, HSURFACE src, DRect *dr, DRect *sr)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.scaleBlit(d, s, dr, sr, copyMode()); },
        [&](Surface &d, Surface &s) { scaleBlit(d, s, dr, sr, copyMode()); });
}

DRESULT scaleSurfaceToSurfaceTransparent(HSURFACE dst, HSURFACE src, DRect *dr, DRect *sr, HDECOLOR key)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.scaleBlit(d, s, dr, sr, keyMode(key)); },
        [&](Surface &d, Surface &s) { scaleBlit(d, s, dr, sr, keyMode(key)); });
}

DRESULT scaleSurfaceToSurfaceSolidColor(HSURFACE dst, HSURFACE src, DRect *dr, DRect *sr, HDECOLOR key, HDECOLOR fill)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.scaleBlit(d, s, dr, sr, solidMode(key, fill)); },
        [&](Surface &d, Surface &s) { scaleBlit(d, s, dr, sr, solidMode(key, fill)); });
}

DRESULT warpSurfaceToSurface(HSURFACE dst, HSURFACE src, DWarpPt *pts, int n)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.warpBlit(d, s, pts, n, copyMode()); },
        [&](Surface &d, Surface &s) { warpBlit(d, s, pts, n, copyMode()); });
}

DRESULT warpSurfaceToSurfaceTransparent(HSURFACE dst, HSURFACE src, DWarpPt *pts, int n, HDECOLOR key)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.warpBlit(d, s, pts, n, keyMode(key)); },
        [&](Surface &d, Surface &s) { warpBlit(d, s, pts, n, keyMode(key)); });
}

DRESULT warpSurfaceToSurfaceSolidColor(HSURFACE dst, HSURFACE src, DWarpPt *pts, int n, HDECOLOR key, HDECOLOR fill)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.warpBlit(d, s, pts, n, solidMode(key, fill)); },
        [&](Surface &d, Surface &s) { warpBlit(d, s, pts, n, solidMode(key, fill)); });
}

DRESULT transformSurfaceToSurface(HSURFACE dst, HSURFACE src, DFloatPt *o, int x, int y, float a, float sx, float sy)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.transformBlit(d, s, o, x, y, a, sx, sy, copyMode()); },
        [&](Surface &d, Surface &s) { transformBlit(d, s, o, x, y, a, sx, sy, copyMode()); });
}

DRESULT transformSurfaceToSurfaceTransparent(HSURFACE dst, HSURFACE src, DFloatPt *o, int x, int y, float a, float sx,
                                             float sy, HDECOLOR key)
{
    return draw2D(
        toSurface(dst), toSurface(src),
        [&](ScreenTarget &t, Surface &d, Surface &s) { t.transformBlit(d, s, o, x, y, a, sx, sy, keyMode(key)); },
        [&](Surface &d, Surface &s) { transformBlit(d, s, o, x, y, a, sx, sy, keyMode(key)); });
}

DRESULT fillRectImpl(HSURFACE dst, DRect *r, HDECOLOR c)
{
    Surface *s = toSurface(dst);
    if (!s) return LT_ERROR;
    if (ScreenTarget *t = gpuScreen(s)) t->fill(*s, r, color(c));
    else fillRect(*s, r, color(c));
    return LT_OK;
}

DBOOL drawBitmapToSurface(HSURFACE dst, char *name, DRect *r, int x, int y)
{
    Surface *d = toSurface(dst);
    auto bmp = loadBitmap(name);
    if (!d || !bmp) return DFALSE;
    if (ScreenTarget *t = gpuScreen(d)) t->blit(*d, *bmp, r, x, y, copyMode());
    else blit(*d, *bmp, r, x, y, copyMode());
    return DTRUE;
}

// ---- fonts and text ------------------------------------------------------------

HDEFONT createFont(char *name, int w, int h, DBOOL italic, DBOOL underline, DBOOL bold)
{
    return reinterpret_cast<HDEFONT>(Font::create(name ? name : "", w, h, italic, underline, bold).release());
}

void deleteFont(HDEFONT h) { delete toFont(h); }

void getStringDimensions(HDEFONT f, HSTRING s, int *w, int *h) { fontOr(f)->measure(text(s), w, h); }

HSURFACE createSurfaceFromString(HDEFONT f, HSTRING s, HDECOLOR fore, HDECOLOR back, int extraX, int extraY)
{
    int w = 0, h = 0;
    const Font *font = fontOr(f);
    font->measure(text(s), &w, &h);
    w += extraX;
    h += extraY;
    if (w <= 0 || h <= 0) return nullptr;
    Surface surf(w, h);
    fillRect(surf, nullptr, color(back));
    font->draw(surf, text(s), 0, 0, color(fore), nullptr);
    return newSurface(std::move(surf));
}

void drawStringToSurface(HSURFACE dst, HDEFONT f, HSTRING s, DRect *r, HDECOLOR fore, HDECOLOR back)
{
    Surface *d = toSurface(dst);
    if (!d) return;
    DRect rect = r ? *r : DRect{0, 0, d->width, d->height};
    forCPUWrite(*d);
    if (!GETT(back)) fillRect(*d, &rect, color(back));
    fontOr(f)->draw(*d, text(s), rect.left, rect.top, color(fore), &rect);
}

// ---- misc ----------------------------------------------------------------------

DRESULT isLobbyLaunched(char *) { return LT_ERROR; }  // never launched from a DirectPlay lobby

DRESULT getEngineHook(char *name, void **data)
{
    static const char kPrefix[] = "shogo_cres_text:";
    *data = nullptr;
    if (name && strncmp(name, kPrefix, sizeof(kPrefix) - 1) == 0 && g->strings) {
        const char *t = g->strings->textResource(name + sizeof(kPrefix) - 1);
        *data = (void *)t;
        return t ? LT_OK : LT_ERROR;
    }
    shogo_stub_called("ClientDE::GetEngineHook");
    return LT_ERROR;
}

float getTime() { return (float)g->time; }
float getFrameTime() { return (float)g->frameTime; }

// Smacker movies (the logo and intro). Failing StartVideo makes the game skip a movie.
MoviePlayer &movie()
{
    static MoviePlayer m;
    return m;
}

DRESULT startVideo(char *name, DDWORD)
{
    std::string err;
    if (!movie().start(name, &err)) {
        printf("[video] can't play %s: %s\n", name ? name : "(null)", err.c_str());
        return VIDEO_ERROR;
    }
    printf("[video] playing %s\n", name);
    return LT_OK;
}

DRESULT stopVideo()
{
    if (!movie().playing()) return LT_ERROR;
    movie().stop();
    return LT_OK;
}

// Draws the frame due now onto the screen; the game's PostUpdate flips it.
DRESULT updateVideo()
{
    if (movie().playing()) forCPUWrite(*g->screen);
    movie().update(g->time, *g->screen);
    return LT_OK;
}

DRESULT isVideoPlaying() { return movie().playing() ? VIDEO_PLAYING : VIDEO_NOTPLAYING; }

void shutdownNow()
{
    printf("[Shutdown] requested by the game\n");
    g->quitRequested = true;
}
}  // namespace

void installClientAPI(ClientDE *c, ClientContext *ctx, int screenWidth, int screenHeight)
{
    g = ctx;
    ctx->screen = std::make_unique<Surface>(screenWidth, screenHeight);
    if (ctx->strings) installClientStrings(c, ctx->strings);

    c->CPrint = printConsole;
    c->ShutdownWithMessage = shutdownWithMessage;
    c->Shutdown = shutdownNow;
    c->IsLobbyLaunched = isLobbyLaunched;
    c->GetEngineHook = getEngineHook;

    c->GetTime = getTime;
    c->GetFrameTime = getFrameTime;
    c->StartVideo = startVideo;
    c->StopVideo = stopVideo;
    c->UpdateVideo = updateVideo;
    c->IsVideoPlaying = isVideoPlaying;

    c->GetRenderModes = getRenderModes;
    c->RelinquishRenderModes = relinquishRenderModes;
    c->GetRenderMode = getRenderMode;
    c->SetRenderMode = setRenderMode;
    c->FlipScreen = flipScreen;
    c->ClearScreen = clearScreen;
    c->Start3D = ok;
    c->End3D = ok;
    c->StartOptimized2D = ok;
    c->EndOptimized2D = ok;

    c->CreateColor = makeColor;
    c->DeleteColor = [](HDECOLOR) {};
    c->SetupColor1 = makeColor;
    c->SetupColor2 = makeColor;

    c->GetScreenSurface = getScreenSurface;
    c->CreateSurfaceFromBitmap = createSurfaceFromBitmap;
    c->CreateSurfaceFromString = createSurfaceFromString;
    c->CreateSurface = createSurface;
    c->DeleteSurface = deleteSurface;
    c->GetSurfaceUserData = getSurfaceUserData;
    c->SetSurfaceUserData = setSurfaceUserData;
    c->GetPixel = getPixel;
    c->SetPixel = setPixel;
    c->GetSurfaceDims = getSurfaceDims;
    c->OptimizeSurface = optimizeSurface;
    c->UnoptimizeSurface = unoptimizeSurface;
    c->GetBorderSize = getBorderSize;
    c->DrawBitmapToSurface = drawBitmapToSurface;
    c->DrawSurfaceMasked = drawSurfaceMasked;
    c->DrawSurfaceSolidColor = drawSurfaceSolidColor;
    c->DrawSurfaceToSurface = drawSurfaceToSurface;
    c->DrawSurfaceToSurfaceTransparent = drawSurfaceToSurfaceTransparent;
    c->ScaleSurfaceToSurface = scaleSurfaceToSurface;
    c->ScaleSurfaceToSurfaceTransparent = scaleSurfaceToSurfaceTransparent;
    c->ScaleSurfaceToSurfaceSolidColor = scaleSurfaceToSurfaceSolidColor;
    c->WarpSurfaceToSurface = warpSurfaceToSurface;
    c->WarpSurfaceToSurfaceTransparent = warpSurfaceToSurfaceTransparent;
    c->WarpSurfaceToSurfaceSolidColor = warpSurfaceToSurfaceSolidColor;
    c->TransformSurfaceToSurface = transformSurfaceToSurface;
    c->TransformSurfaceToSurfaceTransparent = transformSurfaceToSurfaceTransparent;
    c->FillRect = fillRectImpl;

    c->CreateFont = createFont;
    c->DeleteFont = deleteFont;
    c->GetStringDimensions = getStringDimensions;
    c->DrawStringToSurface = drawStringToSurface;
}

}  // namespace shogo
