#include "engine/pe_resources.h"

#include <cctype>
#include <functional>

namespace shogo {

namespace {
constexpr uint32_t kRtString = 6;
constexpr uint32_t kResourceDirectoryIndex = 2;  // IMAGE_DIRECTORY_ENTRY_RESOURCE

struct Reader {
    const std::vector<uint8_t> &d;
    bool ok = true;
    uint32_t u16(size_t o) { return o + 2 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8) : (ok = false, 0); }
    uint32_t u32(size_t o) { return o + 4 <= d.size() ? u16(o) | u16(o + 2) << 16 : (ok = false, 0); }
};

struct Section {
    uint32_t va, vsize, rawPtr, rawSize;
};

std::string upper(std::string s)
{
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
}
}  // namespace

char utf16ToCp1252(uint16_t c)
{
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) return (char)c;
    // Windows-1252 0x80..0x9F.
    static const uint16_t kHigh[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                       0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                       0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    for (int i = 0; i < 32; ++i)
        if (kHigh[i] && kHigh[i] == c) return (char)(0x80 + i);
    return '?';
}

std::unique_ptr<PEResources> PEResources::parse(std::vector<uint8_t> image, std::string *error)
{
    auto fail = [&](const char *why) -> std::unique_ptr<PEResources> {
        if (error) *error = why;
        return nullptr;
    };
    Reader r{image};
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return fail("not an MZ executable");
    size_t pe = r.u32(0x3C);
    if (r.u32(pe) != 0x00004550) return fail("missing PE signature");
    uint32_t numSections = r.u16(pe + 6), optSize = r.u16(pe + 20);
    size_t opt = pe + 24;
    uint32_t magic = r.u16(opt);
    size_t dataDirs = opt + (magic == 0x20B ? 112 : 96);
    uint32_t rsrcRva = r.u32(dataDirs + 8 * kResourceDirectoryIndex);
    if (!r.ok || !rsrcRva) return fail("no resource directory");

    std::vector<Section> sections;
    size_t sh = opt + optSize;
    for (uint32_t i = 0; i < numSections; ++i, sh += 40)
        sections.push_back({r.u32(sh + 12), r.u32(sh + 8), r.u32(sh + 20), r.u32(sh + 16)});
    auto fileOffset = [&](uint32_t rva) -> std::optional<size_t> {
        for (const Section &s : sections) {
            uint32_t span = s.vsize > s.rawSize ? s.vsize : s.rawSize;
            if (rva >= s.va && rva < s.va + span) return size_t(rva - s.va + s.rawPtr);
        }
        return std::nullopt;
    };
    auto base = fileOffset(rsrcRva);
    if (!base) return fail("resource directory outside any section");

    std::unique_ptr<PEResources> res(new PEResources);

    // Entry id: high bit set = offset (from the resource base) of a UTF-16 name.
    auto entryName = [&](uint32_t id) -> std::string {
        if (!(id & 0x80000000)) return std::to_string(id);
        size_t p = *base + (id & 0x7FFFFFFF);
        uint32_t len = r.u16(p);
        std::string s;
        for (uint32_t i = 0; i < len; ++i) s.push_back(utf16ToCp1252((uint16_t)r.u16(p + 2 + 2 * i)));
        return s;
    };
    // Calls fn(nameId, offset) for each entry of the directory table at offset dir.
    auto forEach = [&](size_t dir, const std::function<void(uint32_t, uint32_t)> &fn) {
        uint32_t n = r.u16(dir + 12) + r.u16(dir + 14);
        for (uint32_t i = 0; i < n && r.ok; ++i) fn(r.u32(dir + 16 + 8 * i), r.u32(dir + 20 + 8 * i));
    };
    auto leafData = [&](uint32_t off) -> std::optional<std::vector<uint8_t>> {
        // off points to a language directory; take its first leaf.
        size_t langDir = *base + (off & 0x7FFFFFFF);
        if (!(off & 0x80000000)) return std::nullopt;
        if (r.u16(langDir + 12) + r.u16(langDir + 14) == 0) return std::nullopt;
        uint32_t leaf = r.u32(langDir + 20);
        if (leaf & 0x80000000) return std::nullopt;
        size_t entry = *base + leaf;
        uint32_t rva = r.u32(entry), size = r.u32(entry + 4);
        auto at = fileOffset(rva);
        if (!r.ok || !at || *at + size > image.size()) return std::nullopt;
        return std::vector<uint8_t>(image.begin() + (long)*at, image.begin() + (long)(*at + size));
    };

    forEach(*base, [&](uint32_t typeId, uint32_t typeOff) {
        if (!(typeOff & 0x80000000)) return;
        size_t nameDir = *base + (typeOff & 0x7FFFFFFF);
        forEach(nameDir, [&](uint32_t nameId, uint32_t nameOff) {
            auto data = leafData(nameOff);
            if (!data) return;
            if (typeId == kRtString) {
                // Block N holds 16 length-prefixed UTF-16 strings for IDs (N-1)*16 .. +15.
                uint32_t first = (nameId - 1) * 16;
                Reader b{*data};
                size_t p = 0;
                for (uint32_t i = 0; i < 16 && p + 2 <= data->size(); ++i) {
                    uint32_t len = b.u16(p);
                    p += 2;
                    if (len == 0) continue;
                    std::string s;
                    for (uint32_t k = 0; k < len && p + 2 <= data->size(); ++k, p += 2)
                        s.push_back(utf16ToCp1252((uint16_t)b.u16(p)));
                    res->m_strings[first + i] = std::move(s);
                }
            } else if (typeId & 0x80000000) {
                res->m_named[upper(entryName(typeId)) + "/" + upper(entryName(nameId))] = std::move(*data);
            }
        });
    });
    if (!r.ok) return fail("corrupt resource directory");
    return res;
}

std::optional<std::string> PEResources::string(uint32_t id) const
{
    auto it = m_strings.find(id);
    if (it == m_strings.end()) return std::nullopt;
    return it->second;
}

std::optional<std::vector<uint8_t>> PEResources::named(const std::string &type, const std::string &name) const
{
    auto it = m_named.find(upper(type) + "/" + upper(name));
    if (it == m_named.end()) return std::nullopt;
    return it->second;
}

}  // namespace shogo
