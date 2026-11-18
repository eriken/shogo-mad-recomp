#include "engine/game_import.h"

#include "engine/inno_setup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace shogo {

namespace {

const char kMarker[] = ".shogo-import";  // written when an import has finished

bool isFile(const std::filesystem::path &p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

std::string lower(std::string s)
{
    for (char &c : s) c = char(tolower((unsigned char)c));
    return s;
}

}  // namespace

std::string defaultGameDataDir()
{
    const char *home = getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/Shogo Recomp/Game";
}

bool hasGameData(const std::string &dir)
{
    std::filesystem::path d(dir);
    return isFile(d / kMarker) && isFile(d / "SHOGO.REZ") && isFile(d / "SOUND.REZ");
}

bool importFromGogInstaller(const std::string &installer, const std::string &dir,
                            const std::function<void(float, const std::string &)> &progress, std::string *err)
{
    namespace fs = std::filesystem;
    if (progress) progress(0, "reading the installer");
    InnoSetup setup;
    if (!setup.open(installer, err)) return false;
    if (setup.appName().find("Shogo") == std::string::npos) {
        *err = "this is the installer for " + (setup.appName().empty() ? std::string("another game") : setup.appName()) +
               ", not Shogo";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(fs::path(dir) / kMarker, ec);
    if (ec && fs::exists(fs::path(dir) / kMarker)) {
        *err = "can't write to " + dir;
        return false;
    }
    std::vector<GogFile> files = gogFiles(setup, err);
    if (files.empty()) {
        if (err->empty()) *err = "this installer has no GOG game files in it";
        return false;
    }
    // The English game; not GOG's extras (redistributables, Galaxy files), nor
    // the Windows XP variant of a file. GOG's installer copies the default
    // configs from __support/app into the game folder: so does the import,
    // keeping configs the player already has (key bindings in autoexec.cfg).
    struct Wanted {
        const GogFile *file;
        std::string path;
    };
    std::vector<Wanted> wanted;
    uint64_t total = 0, done = 0;
    for (const GogFile &f : files) {
        std::string path = f.path, l = lower(f.path);
        // GOG's Galaxy files are left out, except its icon (goggame-*.ico): the Dock icon.
        bool gogIcon = l.rfind("goggame-", 0) == 0 && l.size() > 4 && l.compare(l.size() - 4, 4, ".ico") == 0;
        if (f.dependency || l.rfind("__redist/", 0) == 0 || (l.rfind("goggame-", 0) == 0 && !gogIcon)) continue;
        if (!f.languages.empty() && f.languages.find("en-US#") == std::string::npos) continue;
        if (f.winver.find("!") == std::string::npos && f.winver.find("winxp") != std::string::npos) continue;
        if (path.empty() || path[0] == '/' || path.find("..") != std::string::npos) continue;
        if (l.rfind("__support/app/", 0) == 0) {
            path = path.substr(strlen("__support/app/"));
            if (fs::exists(fs::path(dir) / path, ec)) continue;
        } else if (l.rfind("__support/", 0) == 0) {
            continue;
        }
        wanted.push_back({&f, path});
        total += f.size;
    }
    for (const Wanted &w : wanted) {
        fs::path out = fs::path(dir) / w.path;
        fs::create_directories(out.parent_path(), ec);
        if (progress) progress(total ? float(double(done) / double(total)) : 0.0f, w.path);
        uint64_t before = done;
        if (!extractGogFile(setup, *w.file, out.string(), [&](uint64_t n) {
                done += n;
                if (progress) progress(total ? float(double(done) / double(total)) : 0.0f, w.path);
            }, err))
            return false;
        done = before + w.file->size;
    }
    if (!isFile(fs::path(dir) / "SHOGO.REZ") || !isFile(fs::path(dir) / "SOUND.REZ")) {
        *err = "the installer doesn't contain Shogo's game data (SHOGO.REZ)";
        return false;
    }
    std::ofstream(fs::path(dir) / kMarker) << "imported from " << fs::path(installer).filename().string() << "\n";
    if (progress) progress(1, "done");
    return true;
}

}  // namespace shogo
