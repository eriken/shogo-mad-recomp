#include "engine/smacker.h"

#include <cstdio>
#include <cstring>
#include <functional>

namespace shogo {

namespace {

constexpr int32_t kNode = 1 << 30;  // big-tree entry that is a node, not a value
constexpr size_t kHeaderSize = 104;

uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// Bits are read from the low bit of each byte up. Reading past the end gives zeros.
class BitReader {
public:
    BitReader(const uint8_t *p, size_t n) : m_p(p), m_bits(n * 8) {}
    unsigned bit()
    {
        if (m_pos >= m_bits) {
            ++m_pos;
            return 0;
        }
        unsigned b = (m_p[m_pos >> 3] >> (m_pos & 7)) & 1;
        ++m_pos;
        return b;
    }
    uint32_t bits(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint32_t(bit()) << i;
        return v;
    }
    bool overrun() const { return m_pos > m_bits; }

private:
    const uint8_t *m_p;
    size_t m_bits;
    size_t m_pos = 0;
};

// An 8-bit Huffman tree, stored as a node list (bit 0 = first child).
struct SmallTree {
    std::vector<int> kids;  // node i: kids[2i], kids[2i+1]; >= 0 a node, < 0 the leaf ~value
    int single = 0;         // value of a tree with no nodes (one leaf, or none)

    unsigned decode(BitReader &br) const
    {
        if (kids.empty()) return unsigned(single);
        int n = 0;
        for (int depth = 0; depth < 64; ++depth) {
            int k = kids[size_t(n) * 2 + br.bit()];
            if (k < 0) return unsigned(~k);
            n = k;
        }
        return 0;
    }
};

// A present bit, then the tree in preorder (1 = node, 0 = leaf with an 8-bit value), then a 0.
bool readSmallTree(BitReader &br, SmallTree &t)
{
    t = SmallTree{};
    if (!br.bit()) return true;
    int leaves = 0;
    bool ok = true;
    std::function<int(int)> build = [&](int depth) -> int {
        if (!ok || depth > 32) {
            ok = false;
            return ~0;
        }
        if (!br.bit()) {
            if (++leaves > 256) ok = false;
            return ~int(br.bits(8));
        }
        int idx = int(t.kids.size() / 2);
        t.kids.resize(t.kids.size() + 2);
        int a = build(depth + 1);
        int b = build(depth + 1);
        t.kids[size_t(idx) * 2] = a;
        t.kids[size_t(idx) * 2 + 1] = b;
        return idx;
    };
    int root = build(0);
    if (root < 0) t.single = ~root;
    br.bit();
    return ok && !br.overrun();
}

// A present bit; two 8-bit trees for the low and high bytes; three 16-bit
// escape values; the tree in preorder (a node stores the size of its 0
// subtree, a leaf's value is a low and a high byte); then a 0. Leaves holding
// an escape value become the three recently-used slots.
bool readBigTree(BitReader &br, SmackerVideo::BigTree &t, uint32_t allocBytes)
{
    t.table.assign(1, 0);
    t.last[0] = t.last[1] = t.last[2] = 0;
    if (!br.bit()) return true;
    SmallTree lo, hi;
    if (!readSmallTree(br, lo) || !readSmallTree(br, hi)) return false;
    uint32_t esc[3];
    for (uint32_t &e : esc) e = br.bits(16);
    t.table.clear();
    int last[3] = {-1, -1, -1};
    const size_t limit = allocBytes / 4 + 16;
    bool ok = true;
    std::function<int(int)> build = [&](int depth) -> int {
        if (!ok || depth > 64 || t.table.size() >= limit) {
            ok = false;
            return 1;
        }
        if (!br.bit()) {
            uint32_t v = lo.decode(br) | hi.decode(br) << 8;
            int idx = int(t.table.size());
            if (v == esc[0]) last[0] = idx, v = 0;
            else if (v == esc[1]) last[1] = idx, v = 0;
            else if (v == esc[2]) last[2] = idx, v = 0;
            t.table.push_back(int32_t(v));
            return 1;
        }
        size_t node = t.table.size();
        t.table.push_back(0);
        int left = build(depth + 1);
        t.table[node] = kNode | left;
        int right = build(depth + 1);
        return 1 + left + right;
    };
    build(0);
    br.bit();
    for (int i = 0; i < 3; ++i) {
        if (last[i] < 0) {
            last[i] = int(t.table.size());
            t.table.push_back(0);
        }
        t.last[i] = last[i];
    }
    return ok && !br.overrun();
}

unsigned getCode(BitReader &br, SmackerVideo::BigTree &t)
{
    std::vector<int32_t> &tab = t.table;
    size_t i = 0;
    while (tab[i] & kNode) {
        if (br.bit()) i += size_t(tab[i] & ~kNode);
        if (++i >= tab.size()) return 0;
    }
    int32_t v = tab[i];
    if (v != tab[size_t(t.last[0])]) {
        tab[size_t(t.last[2])] = tab[size_t(t.last[1])];
        tab[size_t(t.last[1])] = tab[size_t(t.last[0])];
        tab[size_t(t.last[0])] = v;
    }
    return unsigned(v);
}

const int kBlockRuns[64] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,  14,  15,  16,
                            17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,  30,  31,  32,
                            33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,  46,  47,  48,
                            49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 128, 256, 512, 1024, 2048};

uint32_t expand6(unsigned v) { return ((v & 0x3F) * 255 + 31) / 63; }

}  // namespace

void SmackerVideo::BigTree::reset()
{
    for (int l : last)
        if (size_t(l) < table.size()) table[size_t(l)] = 0;
}

bool SmackerVideo::open(const std::string &path, std::string *err)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "can't open " + path;
        return false;
    }
    std::vector<uint8_t> data;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        data.resize(size_t(n));
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    fclose(f);
    return load(std::move(data), err);
}

bool SmackerVideo::load(std::vector<uint8_t> data, std::string *err)
{
    auto fail = [&](const char *why) {
        if (err) *err = why;
        return false;
    };
    m_data = std::move(data);
    const uint8_t *h = m_data.data();
    if (m_data.size() < kHeaderSize || (memcmp(h, "SMK2", 4) != 0 && memcmp(h, "SMK4", 4) != 0))
        return fail("not a Smacker file");
    m_v4 = h[3] == '4';
    m_width = int(u32(h + 4));
    m_height = int(u32(h + 8));
    m_frames = int(u32(h + 12));
    int32_t rate = int32_t(u32(h + 16));
    m_flags = u32(h + 20);
    if (m_width <= 0 || m_height <= 0 || m_width > 4096 || m_height > 4096 || m_frames <= 0 || m_frames > 1000000)
        return fail("bad Smacker header");
    // Positive: milliseconds per frame; negative: hundredths of a millisecond; zero: 10 fps.
    m_frameSeconds = rate > 0 ? rate / 1000.0 : rate < 0 ? -double(rate) / 100000.0 : 0.1;
    const uint32_t treesSize = u32(h + 52);
    const uint32_t mmapSize = u32(h + 56), mclrSize = u32(h + 60), fullSize = u32(h + 64), typeSize = u32(h + 68);
    // Audio track 0: bit 31 compressed, 30 present, 29 16-bit, 28 stereo,
    // 26-27 codec (0 = DPCM, the others are Bink audio, which Shogo doesn't use).
    const uint32_t a = u32(h + 72);
    m_audio = AudioTrack{};
    if ((a & 0x40000000) && !(a & 0x0C000000)) {
        m_audio.rate = int(a & 0xFFFFFF);
        m_audio.compressed = (a & 0x80000000) != 0;
        m_audio.bits16 = (a & 0x20000000) != 0;
        m_audio.stereo = (a & 0x10000000) != 0;
    }

    // A ring frame (flag 1) repeats the first frame for looping: listed, never played.
    const size_t listed = size_t(m_frames) + ((m_flags & 1) ? 1 : 0);
    size_t off = kHeaderSize;
    if (m_data.size() < off + listed * 5 + treesSize) return fail("truncated Smacker file");
    m_frameSizes.resize(listed);
    for (size_t i = 0; i < listed; ++i) m_frameSizes[i] = u32(h + off + i * 4);
    off += listed * 4;
    m_frameTypes.assign(h + off, h + off + listed);
    off += listed;

    BitReader br(h + off, treesSize);
    if (!readBigTree(br, m_mmap, mmapSize) || !readBigTree(br, m_mclr, mclrSize) || !readBigTree(br, m_full, fullSize) ||
        !readBigTree(br, m_type, typeSize))
        return fail("bad Smacker Huffman trees");
    m_nextOffset = off + treesSize;
    m_current = -1;
    m_image.assign(size_t(m_width) * size_t(m_height), 0);
    memset(m_palette, 0, sizeof(m_palette));
    m_samples.clear();
    return true;
}

bool SmackerVideo::nextFrame()
{
    if (m_current + 1 >= m_frames) return false;
    const size_t i = size_t(m_current + 1);
    const size_t size = m_frameSizes[i] & ~3u;
    if (m_nextOffset + size > m_data.size()) return false;
    const uint8_t *p = m_data.data() + m_nextOffset;
    const uint8_t *end = p + size;
    m_nextOffset += size;
    m_current = int(i);
    m_samples.clear();

    const uint8_t type = m_frameTypes[i];
    if ((type & 1) && p < end) {
        size_t n = size_t(p[0]) * 4;  // counts its own length byte
        if (n == 0 || p + n > end) return false;
        decodePalette(p + 1, n - 1);
        p += n;
    }
    for (int track = 0; track < 7; ++track) {
        if (!(type & (2 << track))) continue;
        if (end - p < 4) return false;
        size_t n = u32(p);  // counts its own 4 bytes
        if (n < 4 || n > size_t(end - p)) return false;
        if (track == 0 && m_audio.rate) decodeAudio(p + 4, n - 4);
        p += n;
    }
    decodeVideo(p, size_t(end - p));
    return true;
}

// Runs of: skip (high bit, 1-128 entries kept), copy (0x40, 1-64 entries
// from an offset in the previous palette) or a new 6-bit RGB entry.
bool SmackerVideo::decodePalette(const uint8_t *p, size_t n)
{
    uint32_t old[256];
    memcpy(old, m_palette, sizeof(old));
    size_t at = 0, i = 0;
    while (at < 256 && i < n) {
        uint8_t t = p[i++];
        if (t & 0x80) {
            at += size_t(t & 0x7F) + 1;
        } else if (t & 0x40) {
            if (i >= n) return false;
            size_t from = p[i++];
            size_t count = size_t(t & 0x3F) + 1;
            if (from + count > 256) return false;
            while (count-- && at < 256) m_palette[at++] = old[from++];
        } else {
            if (i + 2 > n) return false;
            uint32_t r = expand6(t), g = expand6(p[i]), b = expand6(p[i + 1]);
            i += 2;
            m_palette[at++] = r << 16 | g << 8 | b;
        }
    }
    return true;
}

void SmackerVideo::decodeVideo(const uint8_t *p, size_t n)
{
    m_mmap.reset();
    m_mclr.reset();
    m_full.reset();
    m_type.reset();
    BitReader br(p, n);
    const int bw = m_width / 4, bh = m_height / 4;
    const int blocks = bw * bh;
    const size_t stride = size_t(m_width);
    auto blockAt = [&](int blk) { return &m_image[size_t(blk / bw) * 4 * stride + size_t(blk % bw) * 4]; };
    int blk = 0;
    while (blk < blocks && !br.overrun()) {
        const unsigned type = getCode(br, m_type);
        int run = kBlockRuns[(type >> 2) & 0x3F];
        switch (type & 3) {
        case 0:  // mono: two colors, a bit per pixel picks one
            for (; run-- && blk < blocks; ++blk) {
                unsigned clr = getCode(br, m_mclr);
                unsigned map = getCode(br, m_mmap);
                uint8_t hi = uint8_t(clr >> 8), lo = uint8_t(clr);
                uint8_t *out = blockAt(blk);
                for (int y = 0; y < 4; ++y, out += stride, map >>= 4)
                    for (int x = 0; x < 4; ++x) out[x] = (map >> x) & 1 ? hi : lo;
            }
            break;
        case 1: {  // full: every pixel (SMK4 adds doubled and half-resolution blocks)
            int mode = 0;
            if (m_v4) {
                if (br.bit()) mode = 1;
                else if (br.bit()) mode = 2;
            }
            for (; run-- && blk < blocks; ++blk) {
                uint8_t *out = blockAt(blk);
                auto put = [](uint8_t *o, unsigned v) {
                    o[0] = uint8_t(v);
                    o[1] = uint8_t(v >> 8);
                };
                if (mode == 0) {
                    for (int y = 0; y < 4; ++y, out += stride) {
                        put(out + 2, getCode(br, m_full));
                        put(out, getCode(br, m_full));
                    }
                } else if (mode == 1) {
                    for (int y = 0; y < 2; ++y) {
                        unsigned v = getCode(br, m_full);
                        for (int r = 0; r < 2; ++r, out += stride) {
                            out[0] = out[1] = uint8_t(v);
                            out[2] = out[3] = uint8_t(v >> 8);
                        }
                    }
                } else {
                    for (int y = 0; y < 2; ++y) {
                        unsigned right = getCode(br, m_full);
                        unsigned left = getCode(br, m_full);
                        for (int r = 0; r < 2; ++r, out += stride) {
                            put(out, left);
                            put(out + 2, right);
                        }
                    }
                }
            }
            break;
        }
        case 2:  // skip: unchanged from the previous frame
            blk += run;
            break;
        case 3: {  // fill: one color, from the type's high byte
            uint8_t c = uint8_t(type >> 8);
            for (; run-- && blk < blocks; ++blk) {
                uint8_t *out = blockAt(blk);
                for (int y = 0; y < 4; ++y, out += stride) memset(out, c, 4);
            }
            break;
        }
        }
    }
}

void SmackerVideo::decodeAudio(const uint8_t *p, size_t n)
{
    const int channels = m_audio.stereo ? 2 : 1;
    if (!m_audio.compressed) {
        if (m_audio.bits16) {
            for (size_t i = 0; i + 1 < n; i += 2) m_samples.push_back(int16_t(p[i] | p[i + 1] << 8));
        } else {
            for (size_t i = 0; i < n; ++i) m_samples.push_back(int16_t((int(p[i]) - 128) << 8));
        }
        return;
    }
    // DPCM: the unpacked size in bytes; then a present bit, stereo and 16-bit
    // bits; a delta tree per channel (16-bit: one each for the low and high
    // bytes); the first sample of each channel (last channel first); then deltas.
    if (n < 4) return;
    const uint32_t bytes = u32(p);
    BitReader br(p + 4, n - 4);
    if (!br.bit()) {
        m_samples.assign(m_audio.bits16 ? bytes / 2 : bytes, 0);
        return;
    }
    const bool stereo = br.bit() != 0;
    const bool bits16 = br.bit() != 0;
    if (stereo != m_audio.stereo || bits16 != m_audio.bits16) return;
    SmallTree trees[4];
    for (int i = 0; i < (1 << (int(bits16) + int(stereo))); ++i)
        if (!readSmallTree(br, trees[i])) return;
    const int s = stereo ? 1 : 0;
    if (bits16) {
        int16_t pred[2] = {0, 0};
        for (int ch = s; ch >= 0; --ch) {
            uint32_t v = br.bits(16);
            pred[ch] = int16_t(((v & 0xFF) << 8) | (v >> 8));
        }
        const size_t count = bytes / 2;
        m_samples.reserve(count);
        for (int ch = 0; ch < channels && m_samples.size() < count; ++ch) m_samples.push_back(pred[ch]);
        for (size_t i = m_samples.size(); i < count; ++i) {
            const int ch = int(i) & s;
            unsigned lo = trees[ch * 2].decode(br);
            unsigned hi = trees[ch * 2 + 1].decode(br);
            pred[ch] = int16_t(pred[ch] + int16_t(lo | hi << 8));
            m_samples.push_back(pred[ch]);
        }
    } else {
        uint8_t pred[2] = {0, 0};
        for (int ch = s; ch >= 0; --ch) pred[ch] = uint8_t(br.bits(8));
        const size_t count = bytes;
        m_samples.reserve(count);
        for (int ch = 0; ch < channels && m_samples.size() < count; ++ch)
            m_samples.push_back(int16_t((int(pred[ch]) - 128) << 8));
        for (size_t i = m_samples.size(); i < count; ++i) {
            const int ch = int(i) & s;
            pred[ch] = uint8_t(pred[ch] + int8_t(trees[ch].decode(br)));
            m_samples.push_back(int16_t((int(pred[ch]) - 128) << 8));
        }
    }
}

}  // namespace shogo
