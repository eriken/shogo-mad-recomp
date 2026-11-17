// Reader for LithTech 1.0 resource archives (.REZ, "RezMgr Version 1").
#ifndef SHOGO_ENGINE_REZ_FILE_H
#define SHOGO_ENGINE_REZ_FILE_H

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace shogo {

class RezFile {
public:
    // Opens and indexes an archive. Returns nullptr (with a message in *error) on failure.
    static std::unique_ptr<RezFile> open(const std::string &path, std::string *error = nullptr);
    ~RezFile();

    struct Entry {
        std::string path;  // as stored, e.g. "WORLDS/01_AMBUSH.DAT"
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    // Lookup is case-insensitive and accepts '\' or '/' separators.
    const Entry *find(const std::string &path) const;
    std::optional<std::vector<uint8_t>> read(const std::string &path) const;
    const std::vector<Entry> &entries() const { return m_entries; }
    const std::string &archivePath() const { return m_path; }

    // Normalized lookup key: upper case, forward slashes, no leading slash.
    static std::string key(const std::string &path);

private:
    RezFile() = default;
    bool readDirectory(uint32_t pos, uint32_t size, const std::string &prefix, int depth);

    std::string m_path;
    FILE *m_file = nullptr;
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, size_t> m_index;
};

// A stack of archives searched last-added first, the way LithTech layers
// patch archives (e.g. ShogoP.rez) over the main one.
class ResourceFS {
public:
    bool addArchive(const std::string &path, std::string *error = nullptr);
    std::optional<std::vector<uint8_t>> read(const std::string &path) const;
    bool exists(const std::string &path) const;
    // Paths under dir (recursively) ending in ext, both case-insensitive; sorted, no duplicates.
    std::vector<std::string> list(const std::string &dir, const std::string &ext) const;
    size_t archiveCount() const { return m_archives.size(); }

private:
    std::vector<std::unique_ptr<RezFile>> m_archives;
};

}  // namespace shogo

#endif
