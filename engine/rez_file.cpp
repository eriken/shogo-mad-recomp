#include "engine/rez_file.h"

#include <set>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace shogo {

namespace {
// Header layout (packed): CR LF, 60-char file type, CR LF, 60-char user title,
// CR LF, EOF byte, then uint32 fields starting at 0x7F.
constexpr long kHeaderSize = 0xA8;
constexpr long kVersionOffset = 0x7F;
constexpr uint32_t kTypeDirectory = 1;

uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool readAt(FILE *f, long pos, void *dst, size_t n)
{
    return fseek(f, pos, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

// Reads a NUL-terminated string at buf[p], advancing p. Fails if unterminated.
bool cstr(const std::vector<uint8_t> &buf, size_t &p, std::string &out)
{
    auto end = std::find(buf.begin() + (long)p, buf.end(), uint8_t(0));
    if (end == buf.end()) return false;
    out.assign(buf.begin() + (long)p, end);
    p = size_t(end - buf.begin()) + 1;
    return true;
}
}  // namespace

std::string RezFile::key(const std::string &path)
{
    std::string k;
    k.reserve(path.size());
    for (char c : path) k.push_back(c == '\\' ? '/' : (char)toupper((unsigned char)c));
    size_t start = k.find_first_not_of('/');
    return start == std::string::npos ? std::string() : k.substr(start);
}

std::unique_ptr<RezFile> RezFile::open(const std::string &path, std::string *error)
{
    auto fail = [&](const std::string &why) -> std::unique_ptr<RezFile> {
        if (error) *error = path + ": " + why;
        return nullptr;
    };
    std::unique_ptr<RezFile> rez(new RezFile);
    rez->m_path = path;
    rez->m_file = fopen(path.c_str(), "rb");
    if (!rez->m_file) return fail("cannot open");

    uint8_t hdr[kHeaderSize];
    if (!readAt(rez->m_file, 0, hdr, sizeof(hdr))) return fail("truncated header");
    if (memcmp(hdr + 2, "RezMgr Version 1", 16) != 0) return fail("not a RezMgr v1 archive");
    const uint8_t *fields = hdr + kVersionOffset;
    uint32_t version = u32(fields), rootPos = u32(fields + 4), rootSize = u32(fields + 8);
    if (version != 1) return fail("unsupported RezMgr version " + std::to_string(version));
    if (!rez->readDirectory(rootPos, rootSize, "", 0)) return fail("corrupt directory");

    for (size_t i = 0; i < rez->m_entries.size(); ++i) rez->m_index.emplace(key(rez->m_entries[i].path), i);
    return rez;
}

RezFile::~RezFile()
{
    if (m_file) fclose(m_file);
}

// Directory entries:
//   uint32 type (0 = resource, 1 = directory), uint32 pos, uint32 size, uint32 time, then
//   directory: name\0
//   resource:  uint32 id, char ext[4] (reversed, NUL padded), uint32 numKeys,
//              name\0, description\0, uint32 keys[numKeys]
bool RezFile::readDirectory(uint32_t pos, uint32_t size, const std::string &prefix, int depth)
{
    if (depth > 32) return false;
    std::vector<uint8_t> buf(size);
    if (size && !readAt(m_file, pos, buf.data(), size)) return false;
    size_t p = 0;
    while (p < buf.size()) {
        if (p + 16 > buf.size()) return false;
        uint32_t type = u32(&buf[p]), rpos = u32(&buf[p + 4]), rsize = u32(&buf[p + 8]);
        p += 16;
        std::string name;
        if (type == kTypeDirectory) {
            if (!cstr(buf, p, name)) return false;
            if (!readDirectory(rpos, rsize, prefix + name + "/", depth + 1)) return false;
        } else {
            if (p + 12 > buf.size()) return false;
            char ext[5] = {};
            memcpy(ext, &buf[p + 4], 4);
            uint32_t numKeys = u32(&buf[p + 8]);
            p += 12;
            std::string description;
            if (!cstr(buf, p, name) || !cstr(buf, p, description)) return false;
            p += 4 * size_t(numKeys);
            std::string extension(ext);
            std::reverse(extension.begin(), extension.end());
            Entry e;
            e.path = prefix + name + (extension.empty() ? "" : "." + extension);
            e.offset = rpos;
            e.size = rsize;
            m_entries.push_back(std::move(e));
        }
    }
    return true;
}

const RezFile::Entry *RezFile::find(const std::string &path) const
{
    auto it = m_index.find(key(path));
    return it == m_index.end() ? nullptr : &m_entries[it->second];
}

std::optional<std::vector<uint8_t>> RezFile::read(const std::string &path) const
{
    const Entry *e = find(path);
    if (!e) return std::nullopt;
    std::vector<uint8_t> data(e->size);
    if (e->size && !readAt(m_file, e->offset, data.data(), e->size)) return std::nullopt;
    return data;
}

bool ResourceFS::addArchive(const std::string &path, std::string *error)
{
    auto rez = RezFile::open(path, error);
    if (!rez) return false;
    m_archives.push_back(std::move(rez));
    return true;
}

std::optional<std::vector<uint8_t>> ResourceFS::read(const std::string &path) const
{
    for (auto it = m_archives.rbegin(); it != m_archives.rend(); ++it)
        if (auto data = (*it)->read(path)) return data;
    return std::nullopt;
}

bool ResourceFS::exists(const std::string &path) const
{
    for (const auto &a : m_archives)
        if (a->find(path)) return true;
    return false;
}

std::vector<std::string> ResourceFS::list(const std::string &dir, const std::string &ext) const
{
    std::string prefix = RezFile::key(dir);
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    std::string suffix = RezFile::key(ext);
    std::set<std::string> out;
    for (const auto &a : m_archives)
        for (const auto &e : a->entries()) {
            std::string k = RezFile::key(e.path);
            if (k.compare(0, prefix.size(), prefix) == 0 && k.size() >= suffix.size() &&
                k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0)
                out.insert(k);
        }
    return {out.begin(), out.end()};
}

}  // namespace shogo
