// Reads GOG's Shogo installer (an Inno Setup 5.5.7 Unicode setup .exe) and
// copies the game's files out of it, without running it. Only what that
// installer uses is supported: one embedded slice, LZMA1/LZMA2 chunks, the
// 5.3.9 call-instruction filter on executables, no encryption.
// The format is described in docs/installer-format.md.
#ifndef SHOGO_ENGINE_INNO_SETUP_H
#define SHOGO_ENGINE_INNO_SETUP_H

#include <cstdint>
#include <cstdio>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace shogo {

class InnoSetup {
public:
    struct File {
        std::string destination;  // as the installer names it, e.g. "{app}\Music\RedRiot\a1.sec"
        std::string languages;    // "" (all) or e.g. "en-US"
        std::string check, beforeInstall, afterInstall;  // Pascal script calls (GOG names files there)
        uint32_t data = 0;        // index into data
    };
    struct Data {
        uint32_t chunkOffset = 0;   // the compressed chunk, from the start of the setup data
        uint64_t chunkSize = 0;     // compressed bytes after the chunk's magic
        uint64_t offset = 0;        // where this file starts in the chunk's decompressed stream
        uint64_t size = 0;
        uint8_t sha1[20] = {};
        bool callFilter = false;    // x86 CALL/JMP addresses were made absolute
        bool compressed = false;
    };

    bool open(const std::string &path, std::string *err);
    const std::string &appName() const { return m_appName; }
    const std::vector<File> &files() const { return m_files; }
    const std::vector<Data> &data() const { return m_data; }
    int compression() const { return m_compression; }  // 0 stored, 1 zlib, 2 bzip2, 3 LZMA1, 4 LZMA2

    // Streams data entry `index` (decompressed, filter undone, checksum
    // verified at the end) to sink, which returns false to stop (then err is
    // left for the sink to have set).
    bool read(uint32_t index, const std::function<bool(const uint8_t *, size_t)> &sink, std::string *err);

private:
    std::string m_path, m_appName;
    std::unique_ptr<FILE, int (*)(FILE *)> m_file{nullptr, fclose};
    uint32_t m_dataOffset = 0;  // the setup data (slice) inside the exe
    int m_compression = 0;      // 0 stored, 1 zlib, 2 bzip2, 3 LZMA1, 4 LZMA2
    std::vector<File> m_files;
    std::vector<Data> m_data;
};

// GOG Galaxy installers (like Shogo's) don't install the game through Inno
// Setup's file list: each game file is split into 10 MiB parts, each
// zlib-compressed and stored as a {tmp} file with a hashed name; script calls
// on those entries name the file and join the parts at install time:
//   before_install('<file md5>', 'Music/RedRiot/a1.sec', <parts>)   first part
//   after_install('<part md5>', <compressed size>, <size>)           every part
//   check_if_install('en-US#', '32#64#', '')                         languages
struct GogFile {
    std::string path;       // relative to the game folder, '/' separated
    std::string md5;        // of the whole file, lowercase hex
    std::string languages;  // '#'-separated, e.g. "en-US#fr-FR#"; "" for all
    std::string winver;     // e.g. "!winxp#" (variants for old Windows)
    bool dependency = false;  // a redistributable (check_if_install_dependency)
    uint64_t size = 0;
    std::vector<uint32_t> parts;  // data entries, in order
};
std::vector<GogFile> gogFiles(const InnoSetup &setup, std::string *err);

// Writes one GOG file (its parts inflated and joined) to `out`, checking its
// MD5; progress(bytes) after each piece.
bool extractGogFile(InnoSetup &setup, const GogFile &file, const std::string &out,
                    const std::function<void(uint64_t)> &progress, std::string *err);

}  // namespace shogo

#endif
