// Minimal RIFF chunk walking for the music formats (DLS, IMA styles and sections).
#ifndef SHOGO_ENGINE_RIFF_H
#define SHOGO_ENGINE_RIFF_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace shogo::riff {

inline uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

struct Chunk {
    char id[5] = {};    // "RIFF", "LIST" or the chunk's own id
    char form[5] = {};  // list/form type for RIFF and LIST
    const uint8_t *data = nullptr;  // body (after the form type for lists)
    size_t size = 0;
    bool is(const char *i) const { return strcmp(id, i) == 0; }
    bool isList(const char *f) const { return (is("LIST") || is("RIFF")) && strcmp(form, f) == 0; }
};

// The chunks directly inside [p, p+n).
inline std::vector<Chunk> children(const uint8_t *p, size_t n)
{
    std::vector<Chunk> out;
    size_t off = 0;
    while (off + 8 <= n) {
        Chunk c;
        memcpy(c.id, p + off, 4);
        size_t size = u32(p + off + 4);
        if (size > n - off - 8) size = n - off - 8;
        c.data = p + off + 8;
        c.size = size;
        if ((c.is("RIFF") || c.is("LIST")) && size >= 4) {
            memcpy(c.form, c.data, 4);
            c.data += 4;
            c.size -= 4;
        }
        out.push_back(c);
        off += 8 + size + (size & 1);
    }
    return out;
}
inline std::vector<Chunk> children(const Chunk &c) { return children(c.data, c.size); }

inline const Chunk *find(const std::vector<Chunk> &cs, const char *id)
{
    for (const Chunk &c : cs)
        if (c.is(id)) return &c;
    return nullptr;
}
// The result points into the vector: keep it alive.
const Chunk *find(std::vector<Chunk> &&, const char *) = delete;

// A UTF-16 name field (at most n characters), as ASCII.
inline std::string wideName(const uint8_t *p, size_t chars)
{
    std::string s;
    for (size_t i = 0; i < chars; ++i) {
        uint16_t ch = u16(p + i * 2);
        if (!ch) break;
        s += ch < 128 ? char(ch) : '?';
    }
    return s;
}

}  // namespace shogo::riff

#endif
