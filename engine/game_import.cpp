#include "engine/game_import.h"

#include "engine/inno_setup.h"
#include "engine/pe_resources.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace shogo {

namespace {

const char kMarker[] = ".shogo-import";  // written when an import has finished
const char kSteamIcon[] = "shogo-icon.ico";  // Start.exe's icon, from a Steam import

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

// Port defaults on top of GOG's key bindings, added to a newly copied
// autoexec.cfg only (an existing one is the player's and stays as it is):
// W/S forward and back, A/D strafe, the mouse turning. S was the tractor
// beam's, which moves to F (free in GOG's bindings). The arrow keys and , .
// stay bound too. A later rangebind for a key replaces the earlier one.
const char kPortBindings[] =
    "\n// Shogo native: WASD movement\n"
    "rangebind \"##keyboard\" \"##17\" 0.000000 0.000000 \"Forward\"\n"
    "rangebind \"##keyboard\" \"##31\" 0.000000 0.000000 \"Backward\"\n"
    "rangebind \"##keyboard\" \"##30\" 0.000000 0.000000 \"StrafeLeft\"\n"
    "rangebind \"##keyboard\" \"##32\" 0.000000 0.000000 \"StrafeRight\"\n"
    "rangebind \"##keyboard\" \"##33\" 0.000000 0.000000 \"TractorBeam\"\n";

// Appends the port's key bindings to a newly copied autoexec.cfg.
bool addPortBindings(const std::filesystem::path &cfg, std::string *err)
{
    std::ofstream out(cfg, std::ios::app | std::ios::binary);
    if (!(out << kPortBindings)) {
        *err = "can't write to " + cfg.string();
        return false;
    }
    return true;
}

bool sameFolder(const std::filesystem::path &a, const std::filesystem::path &b)
{
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec);
}

}  // namespace

const char kSteamCmdInstall[] = "brew install --cask steamcmd";
const char kSteamCmdDownload[] =
    "steamcmd +@sSteamCmdForcePlatformType windows +force_install_dir \"$HOME/Downloads/Shogo Steam\" "
    "+login YOUR_STEAM_USERNAME +app_update 610860 validate +quit";

std::string findIconFile(const std::string &dir)
{
    namespace fs = std::filesystem;
    std::string steam;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string name = lower(it->path().filename().string());
        if (name.rfind("goggame-", 0) == 0 && it->path().extension() == ".ico") return it->path().string();
        if (name == kSteamIcon) steam = it->path().string();
    }
    return steam;
}

namespace {

// `folder` holds Shogo (SHOGO.REZ, SOUND.REZ); *why says why not.
bool isGameFolder(const std::filesystem::path &folder, std::string *why)
{
    if (!isFile(folder / "SHOGO.REZ") || !isFile(folder / "SOUND.REZ")) {
        *why = "there's no Shogo in " + folder.string() + " (pick the folder with SHOGO.REZ in it)";
        return false;
    }
    // SteamCMD's manifest says whether its download finished (StateFlags 4).
    std::ifstream acf(folder / "steamapps" / "appmanifest_610860.acf");
    std::string line;
    while (acf && std::getline(acf, line))
        if (line.find("\"StateFlags\"") != std::string::npos && line.find("\"4\"") == std::string::npos) {
            *why = "the Steam download hasn't finished";
            return false;
        }
    return true;
}

}  // namespace

bool importFromGameFolder(const std::string &folder, const std::string &dir,
                          const std::function<void(float, const std::string &)> &progress, std::string *err)
{
    namespace fs = std::filesystem;
    if (progress) progress(0, "reading the folder");
    fs::path src(folder);
    if (!isGameFolder(src, err)) return false;
    if (sameFolder(src, dir)) {
        *err = "that's the folder the game is imported into";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(fs::path(dir) / kMarker, ec);
    if (ec && fs::exists(fs::path(dir) / kMarker)) {
        *err = "can't write to " + dir;
        return false;
    }
    // Everything but Steam's own files (SteamCMD's steamapps/, the install
    // script for Steam on Windows). Files the game writes are the player's:
    // configs and saves already in `dir` stay as they are.
    struct Wanted {
        fs::path from, rel;
        uint64_t size;
    };
    std::vector<Wanted> wanted;
    uint64_t total = 0, done = 0;
    bool newAutoexec = false;
    for (auto it = fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        fs::path rel = it->path().lexically_relative(src);
        std::string l = lower(rel.generic_string());
        std::error_code fileEc;  // a file that can't be read is left out, not the end of the walk
        if (it->is_directory(fileEc)) {
            if (l == "steamapps") it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(fileEc) || l == "installscript.vdf" || l == ".shogo-import" || l == kSteamIcon) continue;
        bool players = l == "autoexec.cfg" || l.rfind("save/", 0) == 0;
        if (players && fs::exists(fs::path(dir) / rel, fileEc)) continue;
        if (l == "autoexec.cfg") newAutoexec = true;
        uint64_t size = it->file_size(fileEc);
        if (fileEc) size = 0;
        wanted.push_back({it->path(), rel, size});
        total += size;
    }
    if (ec) {
        *err = "can't read " + src.string() + ": " + ec.message();
        return false;
    }
    std::vector<char> buf(1 << 20);
    for (const Wanted &w : wanted) {
        fs::path out = fs::path(dir) / w.rel;
        fs::create_directories(out.parent_path(), ec);
        std::string name = w.rel.generic_string();
        if (progress) progress(total ? float(double(done) / double(total)) : 0.0f, name);
        std::ifstream in(w.from, std::ios::binary);
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        if (!in || !o) {
            *err = "can't copy " + name;
            return false;
        }
        while (in) {
            in.read(buf.data(), std::streamsize(buf.size()));
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            if (!o.write(buf.data(), n)) {
                *err = "can't write " + out.string();
                return false;
            }
            done += uint64_t(n);
            if (progress) progress(total ? float(double(done) / double(total)) : 0.0f, name);
        }
        if (in.bad() || !o.flush()) {
            *err = "can't copy " + name;
            return false;
        }
    }
    if (newAutoexec && !addPortBindings(fs::path(dir) / "autoexec.cfg", err)) return false;
    // The Dock icon: Steam has no GOG icon file, but its Start.exe has one.
    for (const char *exe : {"Start.exe", "start.exe"}) {
        std::ifstream in(src / exe, std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (auto pe = PEResources::parse(std::move(image)))
            if (auto ico = pe->iconFile()) std::ofstream(fs::path(dir) / kSteamIcon, std::ios::binary).write(
                reinterpret_cast<const char *>(ico->data()), std::streamsize(ico->size()));
        break;
    }
    std::ofstream(fs::path(dir) / kMarker) << "imported from the Steam folder " << src.string() << "\n";
    if (progress) progress(1, "done");
    return true;
}

bool importGame(const std::string &source, const std::string &dir,
                const std::function<void(float, const std::string &)> &progress, std::string *err)
{
    std::error_code ec;
    if (std::filesystem::is_directory(source, ec)) return importFromGameFolder(source, dir, progress, err);
    return importFromGogInstaller(source, dir, progress, err);
}

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
    std::string newAutoexec;  // GOG's autoexec.cfg, when the player had none
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
            if (lower(path) == "autoexec.cfg") newAutoexec = path;
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
    if (!newAutoexec.empty() && !addPortBindings(fs::path(dir) / newAutoexec, err)) return false;
    std::ofstream(fs::path(dir) / kMarker) << "imported from " << fs::path(installer).filename().string() << "\n";
    if (progress) progress(1, "done");
    return true;
}

}  // namespace shogo
