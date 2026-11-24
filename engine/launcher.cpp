#include "engine/launcher.h"

#include "engine/game_import.h"
#include "engine/image_read.h"
#include "engine/rez_file.h"
#include "engine/surfaces.h"

#include <SDL3/SDL.h>

#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
// creators and their email addresses, and to say on the opening screen that
// Monolith didn't make them. The address is the project's own alias.
const char kCreators[] = "Shogo Recomp by Erik Wessel";
const char kContact[] = "shogo-recomp.unlocking364@passmail.net";
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
// At 3/4 size (6 px a character): long single lines.
void smallText(SDL_Renderer *r, float x, float y, const std::string &s, Color c)
{
    SDL_SetRenderScale(r, 0.75f, 0.75f);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderDebugText(r, x / 0.75f, y / 0.75f, s.c_str());
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
// The game's splash screen (640x480), from the imported data.
std::optional<Surface> loadSplashImage(const std::string &dataDir)
{
    ResourceFS fs;
    if (!fs.addArchive(dataDir + "/SHOGO.REZ")) return std::nullopt;
    auto data = fs.read("Interface/SPLASH.PCX");
    if (!data) return std::nullopt;
    return decodePCX(*data);
}
SDL_Texture *loadSplash(SDL_Renderer *r, const std::string &dataDir)
{
    std::optional<Surface> img = loadSplashImage(dataDir);
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
    void start(const std::string &source, const std::string &dir)
    {
        if (m_busy) return;
        if (m_thread.joinable()) m_thread.join();
        m_busy = true;
        m_finished = false;
        m_error.clear();
        m_thread = std::thread([this, source, dir] {
            std::string err;
            bool ok = importGame(source, dir, [this](float p, const std::string &what) {
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

enum Row { RowPlay, RowImport, RowSteam, RowQuit, RowCount };
const char *kRows[RowCount] = {"Play", "Import from GOG...", "Import from Steam...", "Quit"};

// The Steam help under the menu: how to get the game with SteamCMD, a step
// and a button each (copying the two Terminal commands, picking the folder
// SteamCMD downloaded the game into). Each line fits the panel (44 chars).
enum Button { ButtonInstall, ButtonDownload, ButtonFolder, ButtonCount };
const char kSteamIntro[] = "Shogo on Steam is Windows-only. To get it:";
const char *kSteps[ButtonCount] = {"1. Install SteamCMD with Homebrew (brew.sh):", "2. Download Shogo (add your Steam username):",
                                   "3. Pick the folder: Downloads/Shogo Steam"};
// The copy buttons show what they copy (the download command is too long to
// fit, so it's named), with a copy icon at their right end.
const char *kButtons[ButtonCount] = {"brew install --cask steamcmd", "SteamCMD download command", "Choose the Shogo folder..."};

// The copy icon: two overlapping pages; a tick for a moment after copying.
void copyIcon(SDL_Renderer *r, float x, float y, Color c, bool copied)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    if (copied) {
        for (float d = 0; d < 2; ++d) {  // 2 px thick
            SDL_RenderLine(r, x + 1, y + 6 + d, x + 4, y + 9 + d);
            SDL_RenderLine(r, x + 4, y + 9 + d, x + 11, y + 1 + d);
        }
        return;
    }
    SDL_FRect back{x + 4, y, 8, 9}, front{x, y + 3, 8, 9};
    SDL_RenderRect(r, &back);
    fill(r, front.x, front.y, front.w, front.h, kBarBg);  // hides the back page's corner
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    SDL_RenderRect(r, &front);
}

// The launcher used to keep the graphics settings and intro movies in
// ~/Library/Application Support/Shogo Recomp/launcher.cfg ("name value"
// lines); they're in the game's display menu now, saved in autoexec.cfg.
// Moves the player's old choices there, once the game data exists.
void migrateLauncherSettings(const std::string &dataDir)
{
    std::string old = dataDir.substr(0, dataDir.rfind('/')) + "/launcher.cfg";
    FILE *in = fopen(old.c_str(), "r");
    if (!in) return;
    std::string lines;
    char name[64];
    int value = 0;
    while (fscanf(in, "%63s %d", name, &value) == 2) {
        auto line = [&](const char *var, int v) { lines += "\"" + std::string(var) + "\" \"" + std::to_string(v) + "\"\n"; };
        if (!strcmp(name, "introMovies")) line("DisableMovies", value ? 0 : 1);
        else if (!strcmp(name, "metal")) lines += std::string("\"RenderBackend\" \"") + (value ? "metal" : "software") + "\"\n";
        else if (!strcmp(name, "smoothTextures")) line("TextureFilter", value ? 1 : 0);
        else if (!strcmp(name, "msaa")) line("MSAA", value);
        else if (!strcmp(name, "nativeResolution")) line("NativeResolution", value ? 1 : 0);
    }
    fclose(in);
    // Appended, so they win over earlier lines; the game rewrites the file as it quits.
    std::string config = dataDir + "/autoexec.cfg";
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dataDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (!strcasecmp(it->path().filename().c_str(), "autoexec.cfg")) config = it->path().string();
    if (FILE *out = fopen(config.c_str(), "a")) {
        bool ok = fwrite(lines.data(), 1, lines.size(), out) == lines.size();
        if (fclose(out) == 0 && ok) remove(old.c_str());
    }
}
const float kRowX = kArtW + 12, kRowY = 112, kRowH = 32, kRowW = kW - kArtW - 24;

}  // namespace

SDL_Surface *loadDockIcon(const std::string &dataDir)
{
    std::string path = findIconFile(dataDir);
    if (path.empty()) return nullptr;
    std::vector<uint8_t> data;
    if (FILE *f = fopen(path.c_str(), "rb")) {
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
        fclose(f);
    }
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!decodeImageRGBA(data, w, h, rgba)) return nullptr;
    SDL_Surface *icon = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!icon) return nullptr;
    for (int y = 0; y < h; ++y)
        memcpy(static_cast<uint8_t *>(icon->pixels) + size_t(y) * size_t(icon->pitch), &rgba[size_t(y) * size_t(w) * 4],
               size_t(w) * 4);
    return icon;
}

namespace {

// The launcher's Dock icon, so it isn't mistaken for the game (whose icon is
// GOG's or Steam's): a dark tile with the splash screen's red "SHOGO" banner (or the
// launcher's own "SHOGO" title before the data is imported) over "LAUNCHER".
// Drawn here from the player's data, so the app ships no art of its own.
SDL_Surface *makeLauncherIcon(const Surface *splash)
{
    const int S = 256;
    SDL_Surface *icon = SDL_CreateSurface(S, S, SDL_PIXELFORMAT_RGBA32);
    if (!icon) return nullptr;
    SDL_FillSurfaceRect(icon, nullptr, 0);
    // The tile: a rounded square, smoothed at its corners (4x4 samples).
    const float m = S * 0.06f, rad = S * 0.18f;
    auto inside = [&](float x, float y) {
        float qx = std::max(std::max(m + rad - x, x - (S - m - rad)), 0.0f);
        float qy = std::max(std::max(m + rad - y, y - (S - m - rad)), 0.0f);
        return qx * qx + qy * qy <= rad * rad;
    };
    for (int y = 0; y < S; ++y) {
        auto *row = static_cast<Uint8 *>(icon->pixels) + size_t(y) * size_t(icon->pitch);
        for (int x = 0; x < S; ++x) {
            int hits = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) hits += inside(x + (i + 0.5f) / 4, y + (j + 0.5f) / 4);
            Uint8 *p = row + x * 4;
            p[0] = kPanel.r, p[1] = kPanel.g, p[2] = kPanel.b, p[3] = Uint8(hits * 255 / 16);
        }
    }
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(icon);
    if (!r) return icon;
    float bottom = S * 0.5f;
    // The banner: the red box with "SHOGO" and the kanji, top centre of the splash.
    const SDL_FRect bannerSrc{160, 12, 320, 54};
    SDL_Texture *t = nullptr;
    if (splash && splash->width >= 480 && splash->height >= 66) {
        if (SDL_Surface *img = SDL_CreateSurfaceFrom(splash->width, splash->height, SDL_PIXELFORMAT_XRGB8888,
                                                     const_cast<uint32_t *>(splash->pixels.data()), splash->width * 4)) {
            t = SDL_CreateTextureFromSurface(r, img);
            SDL_DestroySurface(img);
        }
    }
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        float w = S - 2 * m - 24, h = w * bannerSrc.h / bannerSrc.w;
        SDL_FRect dst{(S - w) / 2, S * 0.5f - h / 2 - 12, w, h};
        SDL_RenderTexture(r, t, &bannerSrc, &dst);
        SDL_DestroyTexture(t);
        bottom = dst.y + dst.h;
    } else {
        text(r, (S - 5 * 32) / 2.0f, S * 0.5f - 44, "SHOGO", 4, kAccent);
        bottom = S * 0.5f - 12;
    }
    text(r, (S - 8 * 16) / 2.0f, bottom + 20, "LAUNCHER", 2, kText);
    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);
    return icon;
}

}  // namespace

int runLauncher(const std::string &exe)
{
    // SDL blocks the screen saver and display sleep by default, which suits the
    // game but not a launcher left open on the desktop.
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Shogo: Mobile Armor Division - Launcher", kW, kH, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderLogicalPresentation(renderer, kW, kH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 1);

    const std::string dataDir = defaultGameDataDir();
    bool ready = hasGameData(dataDir);
    SDL_Texture *splash = nullptr;
    auto loadArt = [&] {  // the splash and the Dock icon, from the imported data once there is some
        if (ready && !splash) splash = loadSplash(renderer, dataDir);
        std::optional<Surface> splashImage = ready ? loadSplashImage(dataDir) : std::nullopt;
        if (SDL_Surface *icon = makeLauncherIcon(splashImage ? &*splashImage : nullptr)) {
            if (const char *dump = getenv("SHOGO_LAUNCHER_ICON")) SDL_SaveBMP(icon, dump);  // testing
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
    };
    loadArt();
    if (ready) migrateLauncherSettings(dataDir);
    int selected = ready ? RowPlay : RowImport;
    std::string message;
    bool messageIsError = false;
    Importer importer;
    Picked picked;
    pid_t game = 0;
    // The Steam steps: shown once Import from Steam is selected, and kept
    // while the mouse crosses Quit on its way down to their buttons.
    bool steamHelp = false;
    // Quit, under the mouse while the Steam steps show: selected only once the
    // pointer rests there, so crossing it on the way down to the steps' buttons
    // doesn't hide them.
    Uint64 quitHoverSince = 0;
    int copied = -1;  // the copy button just pressed, and when
    Uint64 copiedAt = 0;
    SDL_FRect buttons[ButtonCount] = {};  // where the help's buttons were drawn

    auto startImport = [&](const std::string &source) {
        if (game > 0) {
            message = "Quit Shogo before importing again.";
            messageIsError = true;
            return;
        }
        message.clear();
        importer.start(source, dataDir);
    };
    auto press = [&](int button) {
        switch (button) {
        case ButtonInstall:
        case ButtonDownload:
            SDL_SetClipboardText(button == ButtonInstall ? kSteamCmdInstall : kSteamCmdDownload);
            copied = button, copiedAt = SDL_GetTicks();  // the icon turns to a tick: no message
            break;
        case ButtonFolder: {
            const char *home = getenv("HOME");
            std::string downloads = std::string(home ? home : "") + "/Downloads";
            SDL_ShowOpenFolderDialog(onPicked, &picked, window, downloads.c_str(), false);
            break;
        }
        default: break;
        }
    };
    auto choose = [&](int row) -> bool {  // false: quit
        if (importer.busy()) return true;
        switch (row) {
        case RowPlay: {
            if (!ready) {
                message = "Import Shogo from GOG or Steam first.";
                messageIsError = true;
                break;
            }
            if (game > 0) {
                message = "Shogo is already running.";
                messageIsError = false;
                break;
            }
            // The game's own app inside Shogo.app, so it has its own name in
            // the Dock; this program itself outside the app (development).
            std::string gamePath = exe.substr(0, exe.rfind('/')) + "/../Helpers/Shogo.app/Contents/MacOS/shogo";
            if (access(gamePath.c_str(), X_OK) != 0) gamePath = exe;
            std::vector<std::string> args = {gamePath, "--play"};
            std::vector<char *> argv;
            for (std::string &a : args) argv.push_back(a.data());
            argv.push_back(nullptr);
            if (posix_spawn(&game, args[0].c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
                game = 0;
                message = "Shogo couldn't be started.";
                messageIsError = true;
            } else {
                message = "Shogo is running.";
                messageIsError = false;
            }
            break;
        }
        case RowImport: {
            static const SDL_DialogFileFilter filters[] = {{"GOG's Shogo installer (setup_shogo_*.exe)", "exe"}};
            const char *home = getenv("HOME");
            std::string downloads = std::string(home ? home : "") + "/Downloads";
            SDL_ShowOpenFileDialog(onPicked, &picked, window, filters, 1, downloads.c_str(), false);
            break;
        }
        case RowSteam:
            // The folder picker, with the steps to download the game behind it
            // (cancel the picker to read them).
            message.clear();
            press(ButtonFolder);
            break;
        case RowQuit: return false;
        }
        return true;
    };
    auto buttonAt = [&](float x, float y) {
        if (!steamHelp || importer.busy()) return -1;
        for (int i = 0; i < ButtonCount; ++i)
            if (x >= buttons[i].x && x < buttons[i].x + buttons[i].w && y >= buttons[i].y && y < buttons[i].y + buttons[i].h)
                return i;
        return -1;
    };
    int hoverButton = -1;
    auto rowAt = [&](float x, float y) {
        for (int i = 0; i < RowCount; ++i)
            if (x >= kRowX && x < kRowX + kRowW && y >= kRowY + i * kRowH - 10 && y < kRowY + i * kRowH + kRowH - 10)
                return i;
        return -1;
    };

    // Testing: SHOGO_LAUNCHER_IMPORT=<installer> starts an import,
    // SHOGO_LAUNCHER_PLAY=1 presses Play once the data is ready, and
    // SHOGO_LAUNCHER_SHOT=<file.bmp>@<seconds> saves the window then quits,
    // SHOGO_LAUNCHER_ICON=<file.bmp> saves the Dock icon, and
    // SHOGO_LAUNCHER_STEAM=1 selects Import from Steam.
    if (const char *imp = getenv("SHOGO_LAUNCHER_IMPORT")) startImport(imp);
    if (getenv("SHOGO_LAUNCHER_STEAM")) selected = RowSteam;
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
        bool waiting = quitHoverSince || copied >= 0;  // something to redraw soon
        bool got = SDL_WaitEventTimeout(&e, importer.busy() || game > 0 || waiting ? 50 : 250);
        while (got) {
            SDL_ConvertEventToRenderCoordinates(renderer, &e);
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                quitHoverSince = 0;
                if (e.key.key == SDLK_UP) selected = (selected + RowCount - 1) % RowCount;
                else if (e.key.key == SDLK_DOWN) selected = (selected + 1) % RowCount;
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE)
                    running = choose(selected) && running;
                else if (e.key.key == SDLK_ESCAPE) running = false;
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                int row = rowAt(e.motion.x, e.motion.y);
                if (row == RowQuit && selected == RowSteam && steamHelp) {
                    if (!quitHoverSince) quitHoverSince = SDL_GetTicks();
                } else {
                    quitHoverSince = 0;
                    if (row >= 0) selected = row;
                }
                hoverButton = buttonAt(e.motion.x, e.motion.y);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    int row = rowAt(e.button.x, e.button.y);
                    if (row >= 0) running = choose(row) && running;
                    else if (int b = buttonAt(e.button.x, e.button.y); b >= 0) press(b);
                }
                break;
            case SDL_EVENT_DROP_FILE:  // the installer or a Steam folder dropped on the window
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
            if (ready) migrateLauncherSettings(dataDir);
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

        if (quitHoverSince && SDL_GetTicks() - quitHoverSince >= 250) selected = RowQuit, quitHoverSince = 0;
        if (copied >= 0 && SDL_GetTicks() - copiedAt >= 1500) copied = -1;
        steamHelp = selected == RowSteam;

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
            bool disabled = (i == RowPlay && !ready) || (i != RowQuit && importer.busy());
            if (sel) fill(renderer, kRowX, y - 10, kRowW, kRowH - 4, kRowHi);
            if (sel) text(renderer, kRowX + 8, y, ">", 2, kAccent);
            text(renderer, kRowX + 32, y, kRows[i], 2, disabled ? kDim : sel ? kGold : kText);
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
                                         : "No game data yet: import Shogo from GOG or Steam.";
            // The game data's path on one line, in the small font (it's too long for the panel otherwise).
            if (message.empty() && ready) smallText(renderer, kPanelX, y, status, kGold), y += 20;
            else y = paragraph(renderer, kPanelX, y, status, messageIsError ? kAccent : kGold, 4) + 10;
            if (steamHelp) {
                text(renderer, kPanelX, y, kSteamIntro, 1, kText);
                y += 20;
                for (int i = 0; i < ButtonCount; ++i) {
                    text(renderer, kPanelX, y, kSteps[i], 1, kDim);
                    y += 14;
                    buttons[i] = {kPanelX, y, kPanelW, 22};
                    bool hot = i == hoverButton;
                    fill(renderer, kPanelX, y, kPanelW, 22, hot ? kRowHi : kBarBg);
                    fill(renderer, kPanelX, y, 2, 22, kAccent);
                    text(renderer, kPanelX + 10, y + 7, kButtons[i], 1, hot ? kGold : kText);
                    if (i != ButtonFolder) copyIcon(renderer, kPanelX + kPanelW - 22, y + 5, hot ? kGold : kDim, i == copied);
                    y += 30;
                }
            } else if (selected == RowPlay && ready)
                paragraph(renderer, kPanelX, y, "Graphics settings and intro movies: Options, Display in the game.", kDim);
            else if (selected == RowImport)
                paragraph(renderer, kPanelX, y,
                          "Pick setup_shogo_-_mobile_armor_division_*.exe from GOG, or drop it on this window. "
                          "Nothing is installed or run.",
                          kDim);
        }
        paragraph(renderer, kPanelX, kH - 126, kNotice, kText);
        std::string credit = std::string(kCreators) + "  v" + SHOGO_RECOMP_VERSION;
        text(renderer, kPanelX, kH - 70, credit, 1, kDim);
        text(renderer, kPanelX, kH - 56, kContact, 1, kDim);
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
