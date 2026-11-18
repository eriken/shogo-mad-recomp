#include "engine/inno_setup.h"

#include <CommonCrypto/CommonDigest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

// liblzma ships with macOS (/usr/lib/liblzma.dylib, liblzma.tbd in the SDK) but
// without headers: these are the few stable liblzma 5 declarations we use.
extern "C" {
typedef uint64_t lzma_vli;
typedef int lzma_ret;  // LZMA_OK 0, LZMA_STREAM_END 1
struct lzma_filter {
    lzma_vli id;
    void *options;
};
struct lzma_stream {
    const uint8_t *next_in;
    size_t avail_in;
    uint64_t total_in;
    uint8_t *next_out;
    size_t avail_out;
    uint64_t total_out;
    const void *allocator;
    void *internal;
    void *reserved_ptr1, *reserved_ptr2, *reserved_ptr3, *reserved_ptr4;
    uint64_t reserved_int1, reserved_int2;
    size_t reserved_int3, reserved_int4;
    int reserved_enum1, reserved_enum2;
};
lzma_ret lzma_properties_decode(lzma_filter *filter, const void *allocator, const uint8_t *props, size_t size);
lzma_ret lzma_raw_decoder(lzma_stream *strm, const lzma_filter *filters);
lzma_ret lzma_code(lzma_stream *strm, int action);
void lzma_end(lzma_stream *strm);
}

namespace shogo {

namespace {

const lzma_vli kLzma1 = 0x4000000000000001ULL, kLzma2 = 0x21, kVliUnknown = ~0ULL;

uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t u64(const uint8_t *p) { return u32(p) | uint64_t(u32(p + 4)) << 32; }

// A raw LZMA1/LZMA2 decoder fed from a FILE, at most `limit` input bytes.
class Lzma {
public:
    ~Lzma()
    {
        if (m_open) lzma_end(&m_s);
    }
    // props: Inno's 5-byte LZMA1 header or 1-byte LZMA2 one.
    bool start(FILE *f, uint64_t limit, bool lzma2)
    {
        m_f = f;
        m_left = limit;
        uint8_t props[5];
        size_t n = lzma2 ? 1 : 5;
        if (m_left < n || fread(props, 1, n, f) != n) return false;
        m_left -= n;
        lzma_filter filters[2] = {{lzma2 ? kLzma2 : kLzma1, nullptr}, {kVliUnknown, nullptr}};
        if (lzma_properties_decode(&filters[0], nullptr, props, n) != 0) return false;
        memset(&m_s, 0, sizeof m_s);
        lzma_ret r = lzma_raw_decoder(&m_s, filters);
        free(filters[0].options);
        m_open = r == 0;
        return m_open;
    }
    // Fills out[0..n); false at the end of the data or on an error.
    bool read(uint8_t *out, size_t n)
    {
        m_s.next_out = out;
        m_s.avail_out = n;
        while (m_s.avail_out) {
            if (!m_s.avail_in && m_left) {
                size_t want = size_t(std::min<uint64_t>(m_left, sizeof m_in));
                size_t got = fread(m_in, 1, want, m_f);
                if (!got) return false;
                m_left -= got;
                m_s.next_in = m_in;
                m_s.avail_in = got;
            }
            size_t before = m_s.avail_out;
            lzma_ret r = lzma_code(&m_s, 0);
            if (r == 1 && m_s.avail_out) return false;  // stream ended early
            if (r != 0 && r != 1) return false;
            if (m_s.avail_out == before && !m_s.avail_in && !m_left) return false;
        }
        return true;
    }

private:
    FILE *m_f = nullptr;
    uint64_t m_left = 0;
    lzma_stream m_s{};
    bool m_open = false;
    uint8_t m_in[1 << 16];
};

// One of the setup header's compressed blocks: u32 CRC, u32 stored size,
// u8 compressed, then the stored bytes in 4 KiB pieces each led by its CRC32.
bool readBlock(FILE *f, std::vector<uint8_t> &out, std::string *err)
{
    uint8_t head[9];
    if (fread(head, 1, 9, f) != 9 || crc32(0, head + 4, 5) != u32(head)) {
        *err = "the installer's header is damaged";
        return false;
    }
    uint32_t stored = u32(head + 4);
    std::vector<uint8_t> raw;
    raw.reserve(stored);
    std::vector<uint8_t> piece(4096 + 4);
    for (uint32_t done = 0; done < stored;) {
        uint32_t n = std::min<uint32_t>(stored - done, 4096 + 4);
        if (n <= 4 || fread(piece.data(), 1, n, f) != n || crc32(0, piece.data() + 4, n - 4) != u32(piece.data())) {
            *err = "the installer's header is damaged";
            return false;
        }
        raw.insert(raw.end(), piece.begin() + 4, piece.begin() + n);
        done += n;
    }
    if (!head[8]) {
        out = std::move(raw);
        return true;
    }
    // LZMA1: 5 property bytes, then the stream (no end marker; ends with the input).
    lzma_filter filters[2] = {{kLzma1, nullptr}, {kVliUnknown, nullptr}};
    lzma_stream s{};
    if (raw.size() < 5 || lzma_properties_decode(&filters[0], nullptr, raw.data(), 5) != 0 ||
        lzma_raw_decoder(&s, filters) != 0) {
        free(filters[0].options);
        *err = "can't decompress the installer's header";
        return false;
    }
    free(filters[0].options);
    s.next_in = raw.data() + 5;
    s.avail_in = raw.size() - 5;
    out.clear();
    uint8_t buf[1 << 16];
    for (;;) {
        s.next_out = buf;
        s.avail_out = sizeof buf;
        lzma_ret r = lzma_code(&s, 0);
        size_t got = sizeof buf - s.avail_out;
        out.insert(out.end(), buf, buf + got);
        if (r == 1 || (!got && !s.avail_in)) break;
        if (r != 0) {
            lzma_end(&s);
            *err = "can't decompress the installer's header";
            return false;
        }
    }
    lzma_end(&s);
    return true;
}

// Sequential reads over a decompressed header block.
struct Cursor {
    const std::vector<uint8_t> &d;
    size_t pos = 0;
    bool ok = true;
    bool need(size_t n)
    {
        if (pos + n > d.size()) ok = false;
        return ok;
    }
    void skip(size_t n)
    {
        if (need(n)) pos += n;
    }
    uint32_t get32()
    {
        if (!need(4)) return 0;
        pos += 4;
        return u32(&d[pos - 4]);
    }
    uint64_t get64()
    {
        if (!need(8)) return 0;
        pos += 8;
        return u64(&d[pos - 8]);
    }
    // A length-prefixed string; Unicode installers store UTF-16LE.
    std::string string()
    {
        uint32_t n = get32();
        if (!need(n)) return {};
        std::string s;
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            uint16_t c = uint16_t(d[pos + i] | d[pos + i + 1] << 8);
            if (c < 0x80) s += char(c);
            else if (c < 0x800) s += char(0xC0 | c >> 6), s += char(0x80 | (c & 0x3F));
            else s += char(0xE0 | c >> 12), s += char(0x80 | (c >> 6 & 0x3F)), s += char(0x80 | (c & 0x3F));
        }
        pos += n;
        return s;
    }
    void strings(int n)
    {
        for (int i = 0; i < n; ++i) skip(get32());
    }
};

const size_t kWinVer = 20;  // two (build u16, minor u8, major u8, sp minor, sp major)… pairs

// Inno Setup's 5.3.9+ CALL/JMP filter, undone in place: the installer turned
// each E8/E9 rel32 into an absolute address (in 64 KiB blocks, high byte flipped).
void undoCallFilter(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint8_t b = p[i++];
        if (b != 0xE8 && b != 0xE9) continue;
        if (0x10000 - ((i - 1) % 0x10000) < 5) continue;  // instruction crosses a block
        if (i + 4 > n) break;
        if (p[i + 3] == 0x00 || p[i + 3] == 0xFF) {
            uint32_t addr = uint32_t(i + 4) & 0xFFFFFF;
            uint32_t rel = (p[i] | p[i + 1] << 8 | p[i + 2] << 16) - addr;
            p[i] = uint8_t(rel);
            p[i + 1] = uint8_t(rel >> 8);
            p[i + 2] = uint8_t(rel >> 16);
            if (rel & 0x800000) p[i + 3] = uint8_t(~p[i + 3]);
        }
        i += 4;
    }
}

}  // namespace

bool InnoSetup::open(const std::string &path, std::string *err)
{
    m_path = path;
    m_files.clear();
    m_data.clear();
    std::unique_ptr<FILE, int (*)(FILE *)> f(fopen(path.c_str(), "rb"), fclose);
    if (!f) {
        *err = "can't open " + path;
        return false;
    }
    // The loader's offset table ("rDlPtS" + 6 id bytes), in the exe's resources.
    std::vector<uint8_t> head(1 << 20);
    head.resize(fread(head.data(), 1, head.size(), f.get()));
    static const uint8_t kId[12] = {'r', 'D', 'l', 'P', 't', 'S', 0xCD, 0xE6, 0xD7, 0x7B, 0x0B, 0x2A};
    auto it = std::search(head.begin(), head.end(), kId, kId + 12);
    if (it == head.end() || size_t(head.end() - it) < 44) {
        *err = "this isn't an Inno Setup installer";
        return false;
    }
    const uint8_t *t = &*it;
    // version 1: total size, exe offset, exe size, exe CRC, header offset, data offset, table CRC
    if (u32(t + 12) != 1 || crc32(0, t, 40) != u32(t + 40)) {
        *err = "unsupported installer (loader table)";
        return false;
    }
    uint32_t headerOffset = u32(t + 32);
    m_dataOffset = u32(t + 36);

    char version[64] = {};
    if (fseek(f.get(), headerOffset, SEEK_SET) != 0 || fread(version, 1, 64, f.get()) != 64) {
        *err = "the installer is cut short";
        return false;
    }
    if (strncmp(version, "Inno Setup Setup Data (5.5.7) (u)", 64) != 0) {
        *err = std::string("unsupported installer version: ") + std::string(version, strnlen(version, 64));
        return false;
    }
    std::vector<uint8_t> main, second;
    if (!readBlock(f.get(), main, err) || !readBlock(f.get(), second, err)) return false;

    // The setup header (5.5.7 Unicode): 32 strings, then the entry counts.
    Cursor c{main};
    m_appName = c.string();
    c.strings(31);
    uint32_t nLanguages = c.get32(), nMessages = c.get32(), nPermissions = c.get32(), nTypes = c.get32(),
             nComponents = c.get32(), nTasks = c.get32(), nDirectories = c.get32(), nFiles = c.get32(),
             nData = c.get32();
    c.skip(7 * 4);  // icons, ini, registry, delete, uninstall delete, run, uninstall run
    // winver, back colours, alpha format, password SHA-1 + salt, disk space,
    // slices per disk, uninstall log mode, dir exists warning, privileges,
    // language dialog + detection; then the compression method.
    c.skip(kWinVer + 4 + 4 + 1 + 20 + 8 + 8 + 4 + 1 + 1 + 1 + 1 + 1);
    if (c.need(1)) m_compression = c.d[c.pos];
    c.skip(1);
    // architectures (2), dir/group pages (2), uninstall display size, 47 option flags (6 bytes)
    c.skip(2 + 2 + 8 + 6);
    for (uint32_t i = 0; i < nLanguages; ++i) c.strings(10), c.skip(4 + 16 + 1);
    for (uint32_t i = 0; i < nMessages; ++i) c.strings(2), c.skip(4);
    for (uint32_t i = 0; i < nPermissions; ++i) c.strings(1);
    for (uint32_t i = 0; i < nTypes; ++i) c.strings(4), c.skip(kWinVer + 1 + 1 + 8);
    for (uint32_t i = 0; i < nComponents; ++i) c.strings(5), c.skip(8 + 4 + 1 + kWinVer + 1 + 8);
    for (uint32_t i = 0; i < nTasks; ++i) c.strings(6), c.skip(4 + 1 + kWinVer + 1);
    for (uint32_t i = 0; i < nDirectories; ++i) c.strings(7), c.skip(4 + kWinVer + 2 + 1);
    for (uint32_t i = 0; i < nFiles && c.ok; ++i) {
        File file;
        c.string();  // source
        file.destination = c.string();
        c.strings(2);  // font name, assembly name
        c.strings(2);  // components, tasks
        file.languages = c.string();
        file.check = c.string();
        file.afterInstall = c.string();
        file.beforeInstall = c.string();
        c.skip(kWinVer);
        file.data = c.get32();
        c.skip(4 + 8 + 2 + 4 + 1);  // attributes, external size, permission, flags, type
        // Entries without data (external files, the uninstaller) have location -1.
        if (file.data != 0xFFFFFFFFu) m_files.push_back(file);
    }
    if (!c.ok) {
        *err = "can't read the installer's file list";
        return false;
    }

    // The second block: the data entries, 74 bytes each.
    if (second.size() != size_t(nData) * 74) {
        *err = "can't read the installer's data list";
        return false;
    }
    for (uint32_t i = 0; i < nData; ++i) {
        const uint8_t *p = &second[size_t(i) * 74];
        Data d;
        if (u32(p) != 0 || u32(p + 4) != 0) {
            *err = "multi-part installers aren't supported";
            return false;
        }
        d.chunkOffset = u32(p + 8);
        d.offset = u64(p + 12);
        d.size = u64(p + 20);
        d.chunkSize = u64(p + 28);
        memcpy(d.sha1, p + 36, 20);
        uint16_t flags = uint16_t(p[72] | p[73] << 8);
        // VersionInfoValid, VersionInfoNotValid, TimeStampInUTC, IsUninstallerExe,
        // CallInstructionOptimized, Touch, ChunkEncrypted, ChunkCompressed, ...
        d.callFilter = flags & (1 << 4);
        d.compressed = flags & (1 << 7);
        if (flags & (1 << 6)) {
            *err = "encrypted installers aren't supported";
            return false;
        }
        m_data.push_back(d);
    }
    for (const File &file : m_files)
        if (file.data >= m_data.size()) {
            *err = "the installer's file list is inconsistent";
            return false;
        }
    if (m_compression != 3 && m_compression != 4 && m_compression != 0) {
        *err = "unsupported compression in the installer";
        return false;
    }
    return true;
}

bool InnoSetup::read(uint32_t index, const std::function<bool(const uint8_t *, size_t)> &sink, std::string *err)
{
    if (index >= m_data.size()) {
        *err = "no such data entry";
        return false;
    }
    const Data &d = m_data[index];
    if (!m_file) m_file.reset(fopen(m_path.c_str(), "rb"));
    FILE *f = m_file.get();
    char magic[4];
    if (!f || fseeko(f, off_t(m_dataOffset) + off_t(d.chunkOffset), SEEK_SET) != 0 || fread(magic, 1, 4, f) != 4 ||
        memcmp(magic, "zlb\x1a", 4) != 0) {
        *err = "the installer's data is damaged";
        return false;
    }
    bool compressed = d.compressed && m_compression != 0;
    Lzma lzma;
    if (compressed && !lzma.start(f, d.chunkSize, m_compression == 4)) {
        *err = "can't decompress the installer's data";
        return false;
    }
    auto get = [&](uint8_t *out, size_t n) { return compressed ? lzma.read(out, n) : fread(out, 1, n, f) == n; };
    std::vector<uint8_t> buf(size_t(std::min<uint64_t>(d.callFilter ? d.size : 1 << 20, 1 << 30)));
    for (uint64_t skip = d.offset; skip;) {  // earlier files in a solid chunk
        size_t n = size_t(std::min<uint64_t>(skip, buf.size()));
        if (!buf.size() || !get(buf.data(), n)) {
            *err = "the installer's data is damaged";
            return false;
        }
        skip -= n;
    }
    CC_SHA1_CTX sha;
    CC_SHA1_Init(&sha);
    if (d.callFilter) {  // executables are small: filter the whole file at once
        if (d.size && !get(buf.data(), buf.size())) {
            *err = "the installer's data is damaged";
            return false;
        }
        undoCallFilter(buf.data(), buf.size());
        CC_SHA1_Update(&sha, buf.data(), CC_LONG(buf.size()));
        if (!sink(buf.data(), buf.size())) return false;
    } else {
        for (uint64_t left = d.size; left;) {
            size_t n = size_t(std::min<uint64_t>(left, buf.size()));
            if (!get(buf.data(), n)) {
                *err = "the installer's data is damaged";
                return false;
            }
            CC_SHA1_Update(&sha, buf.data(), CC_LONG(n));
            if (!sink(buf.data(), n)) return false;
            left -= n;
        }
    }
    uint8_t digest[20];
    CC_SHA1_Final(digest, &sha);
    if (memcmp(digest, d.sha1, 20) != 0) {
        *err = "the installer's data is damaged (checksum mismatch)";
        return false;
    }
    return true;
}

namespace {

// A Pascal script call "name('a', 'b', 3)": its name and arguments (quotes removed).
bool parseCall(const std::string &s, std::string &name, std::vector<std::string> &args)
{
    size_t open = s.find('(');
    if (open == std::string::npos || s.empty() || s.back() != ')') return false;
    name = s.substr(0, open);
    args.clear();
    std::string cur;
    bool quoted = false, any = false;
    for (size_t i = open + 1; i + 1 < s.size(); ++i) {
        char ch = s[i];
        if (ch == '\'') {
            if (quoted && i + 2 < s.size() && s[i + 1] == '\'') cur += '\'', ++i;  // '' inside quotes
            else quoted = !quoted;
            any = true;
        } else if (ch == ',' && !quoted) {
            args.push_back(cur);
            cur.clear();
            any = false;
        } else if (quoted || ch != ' ') {
            cur += ch;
            any = true;
        }
    }
    if (any || !args.empty()) args.push_back(cur);
    return true;
}

std::string lowerHex(std::string s)
{
    for (char &c : s) c = char(tolower((unsigned char)c));
    return s;
}

}  // namespace

std::vector<GogFile> gogFiles(const InnoSetup &setup, std::string *err)
{
    std::vector<GogFile> out;
    size_t partsLeft = 0;
    for (const InnoSetup::File &f : setup.files()) {
        std::string name;
        std::vector<std::string> args;
        if (parseCall(f.beforeInstall, name, args) && (name == "before_install" || name == "before_install_dependency")) {
            if (partsLeft) {
                *err = "the installer's file list is inconsistent (a file is missing parts)";
                return {};
            }
            if (args.size() < 3) continue;
            GogFile g;
            g.md5 = lowerHex(args[0]);
            g.path = args[1];
            std::replace(g.path.begin(), g.path.end(), '\\', '/');
            g.dependency = name == "before_install_dependency";
            partsLeft = size_t(strtoul(args[2].c_str(), nullptr, 10));
            std::string cname;
            std::vector<std::string> cargs;
            if (parseCall(f.check, cname, cargs) && cname == "check_if_install" && cargs.size() >= 3) {
                g.languages = cargs[0];
                g.winver = cargs[2];
            }
            out.push_back(g);
        }
        if (partsLeft && parseCall(f.afterInstall, name, args) && name.rfind("after_install", 0) == 0 && args.size() >= 3) {
            out.back().parts.push_back(f.data);
            out.back().size += strtoull(args[2].c_str(), nullptr, 10);
            --partsLeft;
        }
    }
    if (partsLeft) {
        *err = "the installer's file list is inconsistent (a file is missing parts)";
        return {};
    }
    return out;
}

bool extractGogFile(InnoSetup &setup, const GogFile &file, const std::string &out,
                    const std::function<void(uint64_t)> &progress, std::string *err)
{
    std::unique_ptr<FILE, int (*)(FILE *)> o(fopen(out.c_str(), "wb"), fclose);
    if (!o) {
        *err = "can't write " + out;
        return false;
    }
    CC_MD5_CTX md5;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5_Init(&md5);
    std::vector<uint8_t> inflated(1 << 18);
    for (uint32_t part : file.parts) {
        z_stream z{};
        if (inflateInit(&z) != Z_OK) {
            *err = "can't decompress " + file.path;
            return false;
        }
        bool ended = false;
        bool ok = setup.read(part, [&](const uint8_t *p, size_t n) {
            z.next_in = const_cast<Bytef *>(p);
            z.avail_in = uInt(n);
            while (z.avail_in && !ended) {
                z.next_out = inflated.data();
                z.avail_out = uInt(inflated.size());
                int r = inflate(&z, Z_NO_FLUSH);
                if (r != Z_OK && r != Z_STREAM_END) {
                    *err = file.path + " is damaged in the installer";
                    return false;
                }
                ended = r == Z_STREAM_END;
                size_t got = inflated.size() - z.avail_out;
                CC_MD5_Update(&md5, inflated.data(), CC_LONG(got));
                if (fwrite(inflated.data(), 1, got, o.get()) != got) {
                    *err = "can't write " + out + " (disk full?)";
                    return false;
                }
                if (progress) progress(got);
            }
            return true;
        }, err);
        inflateEnd(&z);
        if (!ok) return false;
        if (!ended) {
            *err = file.path + " is cut short in the installer";
            return false;
        }
    }
    uint8_t digest[16];
    CC_MD5_Final(digest, &md5);
#pragma clang diagnostic pop
    if (fclose(o.release()) != 0) {
        *err = "can't write " + out + " (disk full?)";
        return false;
    }
    char hex[33];
    for (int i = 0; i < 16; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (!file.md5.empty() && file.md5 != hex) {
        *err = file.path + " is damaged in the installer (checksum mismatch)";
        return false;
    }
    return true;
}

}  // namespace shogo
