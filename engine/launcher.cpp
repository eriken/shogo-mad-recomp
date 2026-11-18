#include "engine/launcher.h"

#include "engine/game_import.h"
#include "engine/image_read.h"
#include "engine/rez_file.h"
#include "engine/surfaces.h"

#include <SDL3/SDL.h>

#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

#ifndef SHOGO_RECOMP_VERSION
#define SHOGO_RECOMP_VERSION "dev"
#endif

namespace shogo {

namespace {

// Logical size (the window scales it): the game's splash on the left
// (kArtW), the menu on a panel to its right.
const int kW = 1200, kH = 600, kArtW = 800;
const float kPanelX = kArtW + 24, kPanelW = kW - kArtW - 48;
const size_t kPanelChars = size_t(kPanelW / 8);  // at the 8x8 font's scale 1

// Monolith's source licence (section 8c) asks variations to name their
// creators and to say on the opening screen that Monolith didn't make them.
const char kCreators[] = "Shogo Recomp by Erik Wessel";
const char kNotice[] = "THIS PORT IS NOT MADE BY OR SUPPORTED BY Monolith Productions, or any of its "
                       "affiliates and subsidiaries.";

struct Color {
    Uint8 r, g, b;
};
const Color kBg{12, 14, 20}, kPanel{18, 20, 30}, kText{214, 218, 226}, kDim{120, 126, 140}, kAccent{232, 92, 48}, kGold{240, 188, 84},
    kRowHi{36, 40, 54}, kBarBg{40, 44, 56};

// SDL's built-in 8x8 font, at an integer scale.
void text(SDL_Renderer *r, float x, float y, const std::string &s, int scale, Color c)
{
    SDL_SetRenderScale(r, float(scale), float(scale));
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderDebugText(r, x / scale, y / scale, s.c_str());
    SDL_SetRenderScale(r, 1, 1);
}
// Centred in [x0, x0 + w).
void centered(SDL_Renderer *r, float x0, float w, float y, const std::string &s, int scale, Color c)
{
    text(r, x0 + (w - float(s.size()) * 8 * scale) / 2, y, s, scale, c);
}

// "~/..." for paths in the home folder.
std::string tildePath(const std::string &path)
{
    const char *home = getenv("HOME");
    size_t n = home ? strlen(home) : 0;
    if (n && path.compare(0, n, home) == 0 && (path.size() == n || path[n] == '/')) return "~" + path.substr(n);
    return path;
}

// Word-wrapped to `chars` per line (words longer than a line are cut).
std::vector<std::string> wrap(const std::string &s, size_t chars)
{
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < s.size()) {
        size_t end = s.find(' ', i);
        std::string word = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
        i = end == std::string::npos ? s.size() : end + 1;
        while (word.size() > chars) {  // a long path
            if (!line.empty()) lines.push_back(line), line.clear();
            lines.push_back(word.substr(0, chars));
            word.erase(0, chars);
        }
        if (line.empty()) line = word;
        else if (line.size() + 1 + word.size() <= chars) line += " " + word;
        else lines.push_back(line), line = word;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}
float paragraph(SDL_Renderer *r, float x, float y, const std::string &s, Color c, size_t maxLines = 99)
{
    std::vector<std::string> lines = wrap(s, kPanelChars);
    for (size_t i = 0; i < lines.size() && i < maxLines; ++i, y += 14) text(r, x, y, lines[i], 1, c);
    return y;
}

// The game's splash screen (SHOGO.REZ: Interface/SPLASH.PCX), from the
// player's imported data; none before the import.
SDL_Texture *loadSplash(SDL_Renderer *r, const std::string &dataDir)
{
    ResourceFS fs;
    if (!fs.addArchive(dataDir + "/SHOGO.REZ")) return nullptr;
    auto data = fs.read("Interface/SPLASH.PCX");
    if (!data) return nullptr;
    std::optional<Surface> img = decodePCX(*data);
    if (!img) return nullptr;
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, img->width, img->height);
    if (t) {
        SDL_UpdateTexture(t, nullptr, img->pixels.data(), img->width * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    return t;
}
void fill(SDL_Renderer *r, float x, float y, float w, float h, Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

// Long paths and messages, shortened to fit `chars` characters.
std::string fit(const std::string &s, size_t chars)
{
    if (s.size() <= chars) return s;
    return "..." + s.substr(s.size() - (chars - 3));
}

class Importer {
public:
    ~Importer()
    {
        if (m_thread.joinable()) m_thread.join();
    }
    bool busy() const { return m_busy; }
    void start(const std::string &installer, const std::string &dir)
    {
        if (m_busy) return;
        if (m_thread.joinable()) m_thread.join();
        m_busy = true;
        m_finished = false;
        m_error.clear();
        m_thread = std::thread([this, installer, dir] {
            std::string err;
            bool ok = importFromGogInstaller(installer, dir, [this](float p, const std::string &what) {
                std::lock_guard<std::mutex> g(m_lock);
                m_progress = p;
                m_what = what;
            }, &err);
            {
                std::lock_guard<std::mutex> g(m_lock);
                m_error = ok ? "" : (err.empty() ? "the import failed" : err);
            }
            m_busy = false;
            m_finished = true;
        });
    }
    // True once after an import ends; error() says how.
    bool finished()
    {
        if (!m_finished) return false;
        m_finished = false;
        if (m_thread.joinable()) m_thread.join();
        return true;
    }
    std::string error()
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_error;
    }
    void progress(float &p, std::string &what)
    {
        std::lock_guard<std::mutex> g(m_lock);
        p = m_progress;
        what = m_what;
    }

private:
    std::thread m_thread;
    std::atomic<bool> m_busy{false}, m_finished{false};
    std::mutex m_lock;
    float m_progress = 0;
    std::string m_what, m_error;
};

// The file dialog answers on another thread.
struct Picked {
    std::mutex lock;
    std::string path;
};

void SDLCALL onPicked(void *user, const char *const *files, int)
{
    if (!files || !files[0]) return;  // cancelled or failed
    Picked *p = static_cast<Picked *>(user);
    std::lock_guard<std::mutex> g(p->lock);
    p->path = files[0];
}

enum Row { RowPlay, RowIntros, RowRenderer, RowTextures, RowMSAA, RowResolution, RowImport, RowQuit, RowCount };
const char *kRows[RowCount] = {"Play",          "Intro movies", "Renderer", "Textures",
                               "Anti-aliasing", "Resolution",   "Import from GOG...", "Quit"};
const int kMSAA[] = {1, 2, 4, 8};

// The launcher's settings: ~/Library/Application Support/Shogo Recomp/launcher.cfg,
// "name value" lines. The graphics ones go to the game as console variables.
struct Settings {
    bool introMovies = true;  // the Monolith logo and the intro before the menu
    bool metal = true;        // the GPU renderer; off: software
    bool smoothTextures = true;
    int msaa = 4;                   // samples per pixel
    bool nativeResolution = true;   // 3D at the window's pixel size

    bool isOption(int row) const { return row >= RowIntros && row <= RowResolution; }
    bool enabled(int row) const { return row < RowTextures || row > RowResolution || metal; }
    // Steps an option row's value (dir -1 or +1).
    void step(int row, int dir)
    {
        switch (row) {
        case RowIntros: introMovies = !introMovies; break;
        case RowRenderer: metal = !metal; break;
        case RowTextures: smoothTextures = !smoothTextures; break;
        case RowMSAA: {
            int i = 0;
            while (i < 3 && kMSAA[i] < msaa) ++i;
            msaa = kMSAA[(i + dir + 4) % 4];
            break;
        }
        case RowResolution: nativeResolution = !nativeResolution; break;
        default: break;
        }
    }
    std::string value(int row) const
    {
        switch (row) {
        case RowIntros: return introMovies ? "On" : "Off";
        case RowRenderer: return metal ? "Metal" : "Software";
        case RowTextures: return smoothTextures ? "Smooth" : "Sharp";
        case RowMSAA: return msaa > 1 ? std::to_string(msaa) + "x" : "Off";
        case RowResolution: return nativeResolution ? "Native" : "Original";
        default: return "";
        }
    }
};
std::string settingsPath()
{
    std::string game = defaultGameDataDir();
    return game.substr(0, game.rfind('/')) + "/launcher.cfg";
}
Settings loadSettings()
{
    Settings s;
    if (FILE *f = fopen(settingsPath().c_str(), "r")) {
        char name[64];
        int value = 0;
        while (fscanf(f, "%63s %d", name, &value) == 2) {
            if (!strcmp(name, "introMovies")) s.introMovies = value != 0;
            else if (!strcmp(name, "metal")) s.metal = value != 0;
            else if (!strcmp(name, "smoothTextures")) s.smoothTextures = value != 0;
            else if (!strcmp(name, "msaa")) s.msaa = value;
            else if (!strcmp(name, "nativeResolution")) s.nativeResolution = value != 0;
        }
        fclose(f);
    }
    return s;
}
void saveSettings(const Settings &s)
{
    std::string path = settingsPath();
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);
    if (FILE *f = fopen(path.c_str(), "w")) {
        fprintf(f, "introMovies %d\nmetal %d\nsmoothTextures %d\nmsaa %d\nnativeResolution %d\n", s.introMovies ? 1 : 0,
                s.metal ? 1 : 0, s.smoothTextures ? 1 : 0, s.msaa, s.nativeResolution ? 1 : 0);
        fclose(f);
    }
}
const float kRowX = kArtW + 12, kRowY = 112, kRowH = 32, kRowW = kW - kArtW - 24;

}  // namespace

SDL_Surface *loadDockIcon(const std::string &dataDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dataDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        for (char &c : name) c = char(tolower((unsigned char)c));
        if (name.rfind("goggame-", 0) != 0 || it->path().extension() != ".ico") continue;
        std::vector<uint8_t> data;
        if (FILE *f = fopen(it->path().c_str(), "rb")) {
            uint8_t buf[65536];
            for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
            fclose(f);
        }
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (!decodeImageRGBA(data, w, h, rgba)) continue;
        SDL_Surface *icon = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        if (!icon) return nullptr;
        for (int y = 0; y < h; ++y)
            memcpy(static_cast<uint8_t *>(icon->pixels) + size_t(y) * size_t(icon->pitch), &rgba[size_t(y) * size_t(w) * 4],
                   size_t(w) * 4);
        return icon;
    }
    return nullptr;
}

int runLauncher(const std::string &exe)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Shogo: Mobile Armor Division", kW, kH, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderLogicalPresentation(renderer, kW, kH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 1);

    const std::string dataDir = defaultGameDataDir();
    bool ready = hasGameData(dataDir);
    SDL_Texture *splash = nullptr;
    auto loadArt = [&] {  // from the imported data: the splash and the Dock icon
        if (!ready) return;
        if (!splash) splash = loadSplash(renderer, dataDir);
        if (SDL_Surface *icon = loadDockIcon(dataDir)) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
    };
    loadArt();
    int selected = ready ? RowPlay : RowImport;
    Settings settings = loadSettings();
    std::string message;
    bool messageIsError = false;
    Importer importer;
    Picked picked;
    pid_t game = 0;

    auto startImport = [&](const std::string &installer) {
        if (game > 0) {
            message = "Quit Shogo before importing again.";
            messageIsError = true;
            return;
        }
        message.clear();
        importer.start(installer, dataDir);
    };
    auto choose = [&](int row) -> bool {  // false: quit
        if (importer.busy()) return true;
        switch (row) {
        case RowPlay: {
            if (!ready) {
                message = "Import Shogo from GOG's installer first.";
                messageIsError = true;
                break;
            }
            if (game > 0) {
                message = "Shogo is already running.";
                messageIsError = false;
                break;
            }
            std::vector<std::string> args = {exe, "--play"};
            if (!settings.introMovies) args.insert(args.end(), {"+DisableMovies", "1"});
            args.insert(args.end(), {"+RenderBackend", settings.metal ? "metal" : "software", "+TextureFilter",
                                     settings.smoothTextures ? "1" : "0", "+MSAA", std::to_string(settings.msaa),
                                     "+NativeResolution", settings.nativeResolution ? "1" : "0"});
            std::vector<char *> argv;
            for (std::string &a : args) argv.push_back(a.data());
            argv.push_back(nullptr);
            if (posix_spawn(&game, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
                game = 0;
                message = "Shogo couldn't be started.";
                messageIsError = true;
            } else {
                message = "Shogo is running.";
                messageIsError = false;
            }
            break;
        }
        case RowIntros:
        case RowRenderer:
        case RowTextures:
        case RowMSAA:
        case RowResolution:
            if (!settings.enabled(row)) break;
            settings.step(row, 1);
            saveSettings(settings);
            break;
        case RowImport: {
            static const SDL_DialogFileFilter filters[] = {{"GOG's Shogo installer (setup_shogo_*.exe)", "exe"}};
            const char *home = getenv("HOME");
            std::string downloads = std::string(home ? home : "") + "/Downloads";
            SDL_ShowOpenFileDialog(onPicked, &picked, window, filters, 1, downloads.c_str(), false);
            break;
        }
        case RowQuit: return false;
        }
        return true;
    };
    auto rowAt = [&](float x, float y) {
        for (int i = 0; i < RowCount; ++i)
            if (x >= kRowX && x < kRowX + kRowW && y >= kRowY + i * kRowH - 10 && y < kRowY + i * kRowH + kRowH - 10)
                return i;
        return -1;
    };

    // Testing: SHOGO_LAUNCHER_IMPORT=<installer> starts an import,
    // SHOGO_LAUNCHER_PLAY=1 presses Play once the data is ready, and
    // SHOGO_LAUNCHER_SHOT=<file.bmp>@<seconds> saves the window then quits.
    if (const char *imp = getenv("SHOGO_LAUNCHER_IMPORT")) startImport(imp);
    bool autoPlay = getenv("SHOGO_LAUNCHER_PLAY") != nullptr;
    std::string shotFile;
    double shotAt = 0;
    if (const char *shot = getenv("SHOGO_LAUNCHER_SHOT")) {
        shotFile = shot;
        size_t at = shotFile.rfind('@');
        if (at != std::string::npos) shotAt = atof(shotFile.c_str() + at + 1), shotFile.resize(at);
    }
    const Uint64 started = SDL_GetTicks();

    bool running = true;
    while (running) {
        SDL_Event e;
        bool got = SDL_WaitEventTimeout(&e, importer.busy() || game > 0 ? 50 : 250);
        while (got) {
            SDL_ConvertEventToRenderCoordinates(renderer, &e);
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_UP) selected = (selected + RowCount - 1) % RowCount;
                else if (e.key.key == SDLK_DOWN) selected = (selected + 1) % RowCount;
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE)
                    running = choose(selected) && running;
                else if ((e.key.key == SDLK_LEFT || e.key.key == SDLK_RIGHT) && settings.isOption(selected) &&
                         settings.enabled(selected)) {
                    settings.step(selected, e.key.key == SDLK_LEFT ? -1 : 1);
                    saveSettings(settings);
                }
                else if (e.key.key == SDLK_ESCAPE) running = false;
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                int row = rowAt(e.motion.x, e.motion.y);
                if (row >= 0) selected = row;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    int row = rowAt(e.button.x, e.button.y);
                    if (row >= 0) running = choose(row) && running;
                }
                break;
            case SDL_EVENT_DROP_FILE:  // the installer dropped on the window
                if (e.drop.data && !importer.busy()) startImport(e.drop.data), SDL_RaiseWindow(window);
                break;
            default: break;
            }
            got = SDL_PollEvent(&e);
        }
        {
            std::lock_guard<std::mutex> g(picked.lock);
            if (!picked.path.empty()) {
                startImport(picked.path);
                picked.path.clear();
                // The file dialog leaves the launcher without keyboard focus.
                SDL_RaiseWindow(window);
            }
        }
        if (importer.finished()) {
            SDL_RaiseWindow(window);  // ready for Enter on Play (a drop leaves Finder in front)
            std::string err = importer.error();
            ready = hasGameData(dataDir);
            loadArt();
            if (err.empty()) {
                message = "Imported. Ready to play.";
                messageIsError = false;
                selected = RowPlay;
            } else {
                message = "Import failed: " + err;
                messageIsError = true;
            }
        }
        if (autoPlay && ready && !importer.busy() && game == 0) {
            autoPlay = false;
            choose(RowPlay);
            printf("launcher: play -> %s\n", message.c_str());
        }
        if (game > 0) {
            int status = 0;
            if (waitpid(game, &status, WNOHANG) == game) {
                game = 0;
                bool crashed = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
                message = crashed ? "Shogo stopped with an error (see the log in Console)." : "";
                messageIsError = crashed;
            }
        }

        // Draw: the splash (or the title, before the import) on the left,
        // the menu panel on the right.
        fill(renderer, 0, 0, kW, kH, kBg);
        if (splash) {
            SDL_FRect art{0, 0, float(kArtW), float(kH)};
            SDL_RenderTexture(renderer, splash, nullptr, &art);
        } else {
            centered(renderer, 0, kArtW, 220, "SHOGO", 9, kAccent);
            centered(renderer, 0, kArtW, 312, "MOBILE ARMOR DIVISION", 3, kText);
        }
        fill(renderer, kArtW, 0, kW - kArtW, kH, kPanel);
        fill(renderer, kArtW, 0, 2, kH, kAccent);
        text(renderer, kPanelX, 32, "SHOGO", 4, kAccent);
        text(renderer, kPanelX, 72, "MOBILE ARMOR DIVISION for macOS", 1, kDim);
        for (int i = 0; i < RowCount; ++i) {
            float y = kRowY + i * kRowH;
            bool sel = i == selected;
            bool disabled = (i == RowPlay && !ready) || (i != RowQuit && importer.busy()) || !settings.enabled(i);
            if (sel) fill(renderer, kRowX, y - 10, kRowW, kRowH - 4, kRowHi);
            if (sel) text(renderer, kRowX + 8, y, ">", 2, kAccent);
            text(renderer, kRowX + 32, y, kRows[i], 2, disabled ? kDim : sel ? kGold : kText);
            if (settings.isOption(i)) {
                std::string v = "< " + settings.value(i) + " >";
                text(renderer, kRowX + kRowW - float(v.size()) * 16 - 8, y, v, 2,
                     disabled ? kDim : sel ? kGold : kText);
            }
        }
        float y = kRowY + RowCount * kRowH + 12;
        if (importer.busy()) {
            float p = 0;
            std::string what;
            importer.progress(p, what);
            char line[64];
            snprintf(line, sizeof line, "Importing... %d%%", int(p * 100));
            text(renderer, kPanelX, y, line, 1, kGold);
            fill(renderer, kPanelX, y + 16, kPanelW, 8, kBarBg);
            fill(renderer, kPanelX, y + 16, kPanelW * p, 8, kAccent);
            text(renderer, kPanelX, y + 32, fit(what, kPanelChars), 1, kDim);
        } else {
            std::string status = !message.empty() ? message
                                 : ready ? "Game data: " + tildePath(dataDir)
                                         : "No game data yet: import Shogo from GOG's installer.";
            y = paragraph(renderer, kPanelX, y, status, messageIsError ? kAccent : kGold, 4) + 10;
            if (selected == RowIntros)
                paragraph(renderer, kPanelX, y,
                          "The Monolith logo and the intro movie before the main menu. In the game, Escape skips them.",
                          kDim);
            static const char *const kHelp[RowCount] = {
                nullptr, nullptr,
                "Metal draws with the GPU. Software is the 1998 renderer's look, on the CPU.",
                "Smooth filters textures. Sharp shows each texel, as the software renderer.",
                "Smooths the edges of 3D polygons. Off, 2x, 4x or 8x samples per pixel.",
                "Native draws 3D at the window's full resolution. Original: the game's own, scaled up.",
                nullptr, nullptr};
            if (kHelp[selected]) paragraph(renderer, kPanelX, y, kHelp[selected], kDim);
            if (selected == RowImport)
                paragraph(renderer, kPanelX, y,
                          "Pick setup_shogo_-_mobile_armor_division_*.exe from GOG, or drop it on this window. "
                          "Nothing is installed or run.",
                          kDim);
        }
        paragraph(renderer, kPanelX, kH - 112, kNotice, kText);
        std::string credit = std::string(kCreators) + "  v" + SHOGO_RECOMP_VERSION;
        text(renderer, kPanelX, kH - 56, credit, 1, kDim);
        text(renderer, kPanelX, kH - 30, "Arrows select  Enter choose  Esc quit", 1, kDim);
        if (!shotFile.empty() && (SDL_GetTicks() - started) / 1000.0 >= shotAt) {
            if (SDL_Surface *shot = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SaveBMP(shot, shotFile.c_str());
                SDL_DestroySurface(shot);
            }
            running = false;
        }
        SDL_RenderPresent(renderer);
    }
    // The game keeps running on its own if the launcher closes first.
    if (splash) SDL_DestroyTexture(splash);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}  // namespace shogo
