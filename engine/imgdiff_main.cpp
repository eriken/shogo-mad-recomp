// shogo-imgdiff: compares two screenshots of the same size.
//
//   shogo-imgdiff a.png b.png [--diff out.png] [--threshold N] [--max PERCENT]
//                 [--blocks B]
//
// Prints the share of pixels that differ at all, the share that differ by
// more than N (of 255, default 8) in any channel, and the largest difference.
// --diff writes a picture of where: a dimmed grey copy of a, with differing
// pixels in red (brighter for larger differences).
//
// Exits 0 when at most PERCENT (default 0.5) of pixels differ by more than N,
// 1 when more do, 2 when the images can't be read or their sizes differ.
//
// --blocks B judges B x B blocks instead: a block differs when its average
// colour does by more than N. Texture sampling that lands on the neighbouring
// texel (fine patterns, texel edges) averages out, while wrong content (a
// missing or misdrawn surface) doesn't. PERCENT is then of blocks.
#include "engine/image_read.h"
#include "engine/image_write.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace shogo;

namespace {

bool load(const char *path, int &w, int &h, std::vector<uint8_t> &rgba)
{
    std::vector<uint8_t> data;
    if (FILE *f = fopen(path, "rb")) {
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
        fclose(f);
    }
    if (data.empty() || !decodeImageRGBA(data, w, h, rgba)) {
        fprintf(stderr, "can't read %s\n", path);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    const char *paths[2] = {nullptr, nullptr}, *diffPath = nullptr;
    int threshold = 8;
    double maxPercent = 0.5;
    int blockSize = 0;
    int n = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--diff") && i + 1 < argc) diffPath = argv[++i];
        else if (!strcmp(argv[i], "--threshold") && i + 1 < argc) threshold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxPercent = atof(argv[++i]);
        else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) blockSize = std::max(1, atoi(argv[++i]));
        else if (n < 2) paths[n++] = argv[i];
    }
    if (n < 2) {
        fprintf(stderr, "usage: %s a.png b.png [--diff out.png] [--threshold N] [--max PERCENT]\n", argv[0]);
        return 2;
    }
    int w[2], h[2];
    std::vector<uint8_t> px[2];
    for (int k = 0; k < 2; ++k)
        if (!load(paths[k], w[k], h[k], px[k])) return 2;
    if (w[0] != w[1] || h[0] != h[1]) {
        fprintf(stderr, "sizes differ: %dx%d and %dx%d\n", w[0], h[0], w[1], h[1]);
        return 2;
    }

    Surface diff(w[0], h[0]);
    size_t differ = 0, big = 0;
    int worst = 0;
    for (size_t i = 0; i < size_t(w[0]) * size_t(h[0]); ++i) {
        const uint8_t *a = &px[0][i * 4], *b = &px[1][i * 4];
        int m = 0;
        for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a[c]) - int(b[c])));
        differ += m > 0;
        big += m > threshold;
        worst = std::max(worst, m);
        const uint32_t grey = uint32_t(a[0] * 77 + a[1] * 150 + a[2] * 29) >> 10;  // a quarter brightness
        diff.pixels[i] = m == 0 ? grey * 0x010101u : (uint32_t(std::min(255, 96 + m * 4)) << 16) | grey * 0x0101u;
    }
    const double total = double(w[0]) * double(h[0]);
    const double bigPercent = 100 * double(big) / total;
    printf("%.2f%% of pixels differ, %.2f%% by more than %d, worst %d", 100 * double(differ) / total, bigPercent,
           threshold, worst);
    double failPercent = bigPercent;
    if (blockSize > 0) {
        size_t blocks = 0, bigBlocks = 0;
        for (int by = 0; by < h[0]; by += blockSize)
            for (int bx = 0; bx < w[0]; bx += blockSize) {
                long sum[2][3] = {};
                int count = 0;
                for (int y = by; y < std::min(by + blockSize, h[0]); ++y)
                    for (int x = bx; x < std::min(bx + blockSize, w[0]); ++x, ++count)
                        for (int k = 0; k < 2; ++k)
                            for (int c = 0; c < 3; ++c) sum[k][c] += px[k][(size_t(y) * size_t(w[0]) + size_t(x)) * 4 + size_t(c)];
                int m = 0;
                for (int c = 0; c < 3; ++c) m = std::max(m, int(std::labs(sum[0][c] - sum[1][c]) / count));
                ++blocks;
                bigBlocks += m > threshold;
            }
        failPercent = 100 * double(bigBlocks) / double(blocks);
        printf("; %.2f%% of %dx%d blocks by more than %d", failPercent, blockSize, blockSize, threshold);
    }
    printf("\n");
    if (diffPath && !writePNG(diff, diffPath)) fprintf(stderr, "can't write %s\n", diffPath);
    return failPercent <= maxPercent ? 0 : 1;
}
