#include "engine/movie.h"

#include "engine/sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

namespace shogo {

std::string resolveGamePath(const char *gamePath)
{
    if (!gamePath) return {};
    std::string p = gamePath;
    if (p.size() > 1 && p[1] == ':') p.erase(0, 2);  // drop a drive letter
    for (char &c : p)
        if (c == '\\') c = '/';
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return p;

    // Walk the parts, matching each in its directory without regard to case.
    std::string out = p[0] == '/' ? "/" : "";
    size_t i = p[0] == '/' ? 1 : 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        std::string dir = out.empty() ? "." : out;
        std::string found = part;
        if (DIR *d = opendir(dir.c_str())) {
            while (dirent *e = readdir(d))
                if (strcasecmp(e->d_name, part.c_str()) == 0) {
                    found = e->d_name;
                    break;
                }
            closedir(d);
        }
        if (!out.empty() && out.back() != '/') out += '/';
        out += found;
    }
    return out;
}

bool MoviePlayer::start(const char *gamePath, std::string *err)
{
    stop();
    auto v = std::make_unique<SmackerVideo>();
    if (!v->open(resolveGamePath(gamePath), err)) return false;
    m_video = std::move(v);
    m_start = -1;
    return true;
}

void MoviePlayer::stop()
{
    if (m_video) mixer().streamStop();
    m_video.reset();
    m_start = -1;
}

void MoviePlayer::update(double now, Surface &screen)
{
    if (!m_video) return;
    if (m_start < 0) m_start = now;
    const double t = now - m_start;
    const int due = int(std::floor(t / m_video->frameDuration()));
    if (due >= m_video->frameCount()) {
        // Over: the last of the soundtrack is left to play out.
        m_video.reset();
        return;
    }
    while (m_video->frameIndex() < due) {
        if (!m_video->nextFrame()) {
            m_video.reset();
            return;
        }
        const std::vector<int16_t> &a = m_video->audio();
        if (a.empty()) continue;
        if (m_video->audioChannels() == 2) {
            mixer().streamPush(a.data(), a.size() / 2, m_video->audioRate());
        } else {
            std::vector<int16_t> st(a.size() * 2);
            for (size_t i = 0; i < a.size(); ++i) st[i * 2] = st[i * 2 + 1] = a[i];
            mixer().streamPush(st.data(), a.size(), m_video->audioRate());
        }
    }
    draw(screen);
}

// Fit to the screen keeping the shape, centered on black. Interlaced movies
// are stored at half height and shown with a blank line after each line, as
// Smacker played them; doubled ones repeat each line.
void MoviePlayer::draw(Surface &screen) const
{
    screen.touch();
    const SmackerVideo &v = *m_video;
    const int lineScale = (v.interlaced() || v.doubled()) ? 2 : 1;
    const int srcW = v.width(), srcH = v.height() * lineScale;
    fillRect(screen, nullptr, 0);
    if (srcW <= 0 || srcH <= 0) return;
    double scale = std::min(double(screen.width) / srcW, double(screen.height) / srcH);
    if (scale >= 1) scale = std::floor(scale);  // whole pixels when it grows
    const int dw = int(srcW * scale), dh = int(srcH * scale);
    const int x0 = (screen.width - dw) / 2, y0 = (screen.height - dh) / 2;
    const uint32_t *pal = v.palette();
    const uint8_t *img = v.image().data();
    for (int y = 0; y < dh; ++y) {
        const int sy = int(int64_t(y) * srcH / dh);
        if (v.interlaced() && (sy & 1)) continue;
        const uint8_t *src = img + size_t(sy / lineScale) * size_t(v.width());
        uint32_t *dst = screen.row(y0 + y) + x0;
        for (int x = 0; x < dw; ++x) dst[x] = pal[src[int64_t(x) * srcW / dw]];
    }
}

}  // namespace shogo
