// shogo: the app. Started without a game folder (from Finder, Shogo.app) it
// opens the launcher, which imports the game data and starts the game as
// `shogo --play` (game data from ~/Games/Shogo, saves and settings in
// Application Support). Run from a game folder, or
// with --data <dir> or +console variables, it plays straight away (testing).
// The game modules (object.lto.dylib, cshell.dylib) sit next to the executable.
#include "engine/game_host.h"
#include "engine/game_import.h"
#include "engine/inno_setup.h"
#include "engine/gpu_present.h"
#include "engine/gpu_screen.h"
#include "engine/launcher.h"
#include "engine/render_gpu.h"
#include "engine/sound.h"

#include <SDL3/SDL.h>

#include <algorithm>

#include <mach-o/dyld.h>
#include <unistd.h>

#include <climits>
#include <cmath>
#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>  // VK_ codes

namespace {

int vkFromSDL(SDL_Keycode k)
{
    if (k >= SDLK_A && k <= SDLK_Z) return 'A' + int(k - SDLK_A);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + int(k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return VK_F1 + int(k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return VK_NUMPAD1 + int(k - SDLK_KP_1);
    switch (k) {
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_TAB: return VK_TAB;
    case SDLK_RETURN: case SDLK_KP_ENTER: return VK_RETURN;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_SPACE: return VK_SPACE;
    case SDLK_PAGEUP: return VK_PRIOR;
    case SDLK_PAGEDOWN: return VK_NEXT;
    case SDLK_END: return VK_END;
    case SDLK_HOME: return VK_HOME;
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_UP: return VK_UP;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_INSERT: return VK_INSERT;
    case SDLK_DELETE: return VK_DELETE;
    case SDLK_PAUSE: return VK_PAUSE;
    case SDLK_CAPSLOCK: return VK_CAPITAL;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return VK_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return VK_CONTROL;
    case SDLK_LALT: case SDLK_RALT: return VK_MENU;
    case SDLK_KP_0: return VK_NUMPAD0;
    case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
    case SDLK_KP_PLUS: return VK_ADD;
    case SDLK_KP_MINUS: return VK_SUBTRACT;
    case SDLK_KP_PERIOD: return VK_DECIMAL;
    case SDLK_KP_DIVIDE: return VK_DIVIDE;
    case SDLK_MINUS: return 0xBD;         // VK_OEM_MINUS
    case SDLK_EQUALS: return 0xBB;        // VK_OEM_PLUS
    case SDLK_COMMA: return 0xBC;         // VK_OEM_COMMA
    case SDLK_PERIOD: return 0xBE;        // VK_OEM_PERIOD
    case SDLK_SLASH: return 0xBF;         // VK_OEM_2
    case SDLK_GRAVE: return 0xC0;         // VK_OEM_3 (console)
    case SDLK_SEMICOLON: return 0xBA;     // VK_OEM_1
    case SDLK_APOSTROPHE: return 0xDE;    // VK_OEM_7
    case SDLK_LEFTBRACKET: return 0xDB;   // VK_OEM_4
    case SDLK_RIGHTBRACKET: return 0xDD;  // VK_OEM_6
    case SDLK_BACKSLASH: return 0xDC;     // VK_OEM_5
    default: return 0;
    }
}

// This program's own path (inside Shogo.app: Contents/MacOS/shogo).
std::string exePath(const char *argv0)
{
    char buf[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0 && realpath(buf, real)) return real;
    return argv0;
}

}  // namespace

int main(int argc, char **argv)
{
    // --installer-list <setup.exe>: what GOG's installer holds (testing).
    if (argc == 3 && !strcmp(argv[1], "--installer-list")) {
        shogo::InnoSetup setup;
        std::string err;
        if (!setup.open(argv[2], &err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::vector<shogo::GogFile> files = shogo::gogFiles(setup, &err);
        printf("%s: %zu entries, %zu GOG files%s%s\n", setup.appName().c_str(), setup.files().size(), files.size(),
               err.empty() ? "" : ": ", err.c_str());
        size_t packed = 0, filtered = 0;
        for (const shogo::InnoSetup::Data &d : setup.data()) packed += d.compressed, filtered += d.callFilter;
        printf("compression %d; %zu of %zu chunks compressed, %zu filtered\n", setup.compression(), packed,
               setup.data().size(), filtered);
        for (const shogo::GogFile &f : files)
            printf("%10llu %2zu %-12s %s%s\n", (unsigned long long)f.size, f.parts.size(), f.languages.c_str(), f.path.c_str(),
                   f.winver.empty() ? "" : (" [" + f.winver + "]").c_str());
        return 0;
    }
    // --import <setup.exe or Steam folder> <dir>: the launcher's import, from
    // the command line, into one self-contained folder (saves and all).
    if (argc == 4 && !strcmp(argv[1], "--import")) {
        std::string err;
        int last = -1;
        bool ok = shogo::importGame(argv[2], argv[3], argv[3], [&](float p, const std::string &what) {
            int pct = int(p * 100);
            if (pct / 10 != last / 10) printf("import: %3d%% %s\n", pct, what.c_str()), fflush(stdout), last = pct;
        }, &err);
        printf("import: %s\n", ok ? "done" : ("failed: " + err).c_str());
        return ok ? 0 : 1;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);  // logs survive the game being killed
    const std::string exe = exePath(argv[0]);
    const std::string dir = exe.substr(0, exe.rfind('/') + 1);
    // No game folder given and none here: the launcher.
    bool play = access("SHOGO.REZ", F_OK) == 0, dataGiven = false;
    for (int i = 1; i < argc; ++i)
        play |= argv[i][0] == '+' || !strcmp(argv[i], "--play") || !strcmp(argv[i], "--data"),
            dataGiven |= !strcmp(argv[i], "--data");
    if (!play) return shogo::runLauncher(exe);
    bool fromLauncher = false;
    for (int i = 1; i < argc; ++i) fromLauncher |= !strcmp(argv[i], "--play");
    if (fromLauncher && !dataGiven) {
        std::string data = shogo::defaultGameDataDir(), player = shogo::playerDataDir(), err;
        if (!shogo::moveOldGameData(data, &err)) fprintf(stderr, "Couldn't move the game data: %s\n", err.c_str());
        // The game's own files (Save\..., autoexec.cfg) go to the player's folder.
        std::error_code ec;
        std::filesystem::create_directories(player, ec);
        setenv(shogo::kPlayerDirVar, player.c_str(), 1);
        if (!shogo::hasGameData(data) || chdir(data.c_str()) != 0) {
            std::string msg = "No Shogo game data in " + data + ". Open Shogo and import it from GOG or Steam.";
            fprintf(stderr, "%s\n", msg.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Shogo", msg.c_str(), nullptr);
            return 1;
        }
    }
    // In Shogo.app the game runs from its own app inside it
    // (Contents/Helpers/Shogo.app), so the Dock tells it from the launcher;
    // the game modules stay next to the launcher's executable.
    std::string modules = dir;
    if (access((modules + "cshell.dylib").c_str(), F_OK) != 0) modules = dir + "../../../../MacOS/";
    std::string objectPath = modules + "object.lto.dylib", cshellPath = modules + "cshell.dylib";
    std::vector<std::string> conVars;  // "+name value": console variables, e.g. +runworld Worlds\01_Ambush
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '+' && i + 1 < argc) {
            conVars.push_back(std::string(argv[i] + 1) + " \"" + argv[i + 1] + "\"");
            ++i;
        } else if (!strcmp(argv[i], "--data") && i + 1 < argc) {
            if (chdir(argv[++i]) != 0) {
                fprintf(stderr, "cannot enter %s\n", argv[i]);
                return 1;
            }
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // The game runs at 800 wide and the display's shape, scaled up to the
    // window (DisplayMode, below, sizes the window; it's shown once sized).
    SDL_Rect display{0, 0, 1600, 1000};
    SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &display);
    const int gameW = 800, gameH = std::max(2, int(lround(800.0 * display.h / std::max(1, display.w) / 2)) * 2);
    SDL_Window *window = SDL_CreateWindow("Shogo: Mobile Armor Division", gameW, gameH,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return 1;
    }
    // Frames go out through the GPU; SDL's 2D renderer is the fallback for
    // machines without a usable GPU device.
    shogo::GpuPresenter gpu;
    SDL_Renderer *renderer = nullptr;
    if (gpu.init(window)) {
        gpu.setVSync(true);
        printf("GPU: %s\n", gpu.driver());
    } else {
        fprintf(stderr, "No GPU device (%s); using SDL's renderer\n", SDL_GetError());
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
            return 1;
        }
        SDL_SetRenderVSync(renderer, 1);
    }
    if (SDL_Surface *icon = shogo::loadDockIcon(".")) {  // GOG's or Steam's icon, if the data has it
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }

    // Sound: the mixer runs on this thread; each frame tops the device queue up
    // to about 60 ms ahead.
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, shogo::Mixer::kRate};
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else fprintf(stderr, "No audio device: %s\n", SDL_GetError());
    std::vector<int16_t> audioBuf;
    SDL_Texture *texture = nullptr;
    int texW = 0, texH = 0;

    std::unique_ptr<shogo::GpuScreen> gpuScreen;  // outlives host's renderer
    shogo::GameHost host;
    host.commandLine = conVars;
    host.client.present = [&](const shogo::Surface &s) {
        if (!renderer) {
            gpu.present(s);
            return;
        }
        if (!texture || texW != s.width || texH != s.height) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, s.width,
                                        s.height);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
            SDL_SetRenderLogicalPresentation(renderer, s.width, s.height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            texW = s.width;
            texH = s.height;
        }
        SDL_UpdateTexture(texture, nullptr, s.pixels.data(), s.width * 4);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    };

    // Graphics settings: console variables, set from the game's display menu
    // (or autoexec.cfg, or "+name value" on the command line) and applied
    // between frames whenever they change:
    //   DisplayMode 0..3                0 Original: an 800-wide window, 3D at the
    //                                   game's resolution; 1 Enhanced and 2
    //                                   Enhanced 2x: windows 55% and 85% of the
    //                                   display; 3 Native: full screen. 1-3 draw
    //                                   3D at the window's pixel size.
    //   TextureFilter 1 | 0             smooth (mipmapped) or sharp textures
    //   MSAA 4                          samples per pixel: 1, 2, 4 or 8
    //   RenderBackend "metal" | "software"  for tests: Metal puts the screen on
    //                                   the GPU and draws the world and 2D there
    // SHOGO_RENDERER=gpu|software overrides RenderBackend.
    struct Graphics {
        bool metal = true, smoothTextures = true;
        int msaa = 4, mode = 2;
        bool operator==(const Graphics &) const = default;
    };
    auto wanted = [&] {
        auto var = [&](const char *name) { return host.session.vars.find(name); };
        auto number = [&](const char *name, float def) {
            const auto *v = var(name);
            return v && !v->value.empty() ? v->number : def;
        };
        Graphics g;
        std::string which = var("RenderBackend") ? var("RenderBackend")->value : "metal";
        for (char &c : which) c = char(tolower((unsigned char)c));
        if (const char *e = getenv("SHOGO_RENDERER")) which = strcmp(e, "software") ? "metal" : "software";
        g.metal = which != "software" && gpu.device();
        g.smoothTextures = number("TextureFilter", 1) != 0;
        g.msaa = int(number("MSAA", 4));
        g.mode = std::clamp(int(number("DisplayMode", 2)), 0, 3);
        return g;
    };
    auto applyWindow = [&](int mode) {
        if (mode == 3) {
            SDL_SetWindowFullscreen(window, true);
        } else {
            static const float kShare[] = {0, 0.55f, 0.85f};
            int w = mode == 0 ? gameW : int(lround(display.w * kShare[mode]));
            int h = mode == 0 ? gameH : int(lround(display.h * kShare[mode]));
            SDL_SetWindowFullscreen(window, false);
            SDL_SetWindowSize(window, w, h);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        SDL_ShowWindow(window);
        SDL_SyncWindow(window);
    };
    std::optional<Graphics> applied;
    auto applyGraphics = [&] {
        Graphics g = wanted();
        if (applied && *applied == g) return;
        bool rebuild = !applied || applied->metal != g.metal ||
                       (g.metal && (applied->smoothTextures != g.smoothTextures || applied->msaa != g.msaa));
        if (!applied || applied->mode != g.mode) applyWindow(g.mode);
        applied = g;
        if (!rebuild) return;
        host.session.renderer.setBackend(nullptr);  // the GPU backend goes before its screen
        host.client.screenTarget = nullptr;
        host.client.rendererOnScreenTarget = false;
        host.client.rendererName = "Software";
        if (!g.metal) {
            gpuScreen.reset();
            printf("Renderer: software\n");
            return;
        }
        shogo::GpuSettings settings;
        settings.smoothTextures = g.smoothTextures;
        settings.msaa = g.msaa;
        if (!gpuScreen) gpuScreen = shogo::GpuScreen::create(&gpu);
        auto backend = gpuScreen ? shogo::makeGpuBackend(gpu.device(), gpuScreen.get(), settings) : nullptr;
        if (!backend) {
            gpuScreen.reset();
            fprintf(stderr, "Metal renderer unavailable; drawing in software\n");
            return;
        }
        host.session.renderer.setBackend(std::move(backend));
        host.client.screenTarget = gpuScreen.get();
        host.client.rendererOnScreenTarget = true;
        host.client.rendererName = "Metal";
        printf("Renderer: Metal (textures %s, MSAA %d)\n", settings.smoothTextures ? "smooth" : "sharp", settings.msaa);
    };
    host.onConfigured = applyGraphics;

    std::string err;
    if (!host.mountGameData(&err) || !host.loadServer(objectPath.c_str(), &err) ||
        !host.startClient(cshellPath.c_str(), gameW, gameH, &err)) {
        fprintf(stderr, "%s\n(run from the extracted game folder, or pass --data <dir>)\n", err.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Shogo", err.c_str(), window);
        return 1;
    }

    Uint64 last = SDL_GetTicksNS();
    bool running = true, captured = false;
    while (running && !host.quitRequested()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (int vk = vkFromSDL(e.key.key)) host.keyDown(vk, e.key.repeat ? 2 : 1);
                break;
            case SDL_EVENT_KEY_UP:
                if (int vk = vkFromSDL(e.key.key)) host.keyUp(vk);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (captured) host.mouseMotion(e.motion.xrel, e.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                // SDL: 1 left, 2 middle, 3 right. DirectInput: 0 left, 1 right, 2 middle.
                int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1
                      : e.button.button == SDL_BUTTON_MIDDLE ? 2 : e.button.button - 1;
                // Clicks reach the game while it has the mouse, or while a menu
                // waits for a button to bind; otherwise a click is just the
                // player focusing the window.
                if (captured || host.wantsMouseButtons() || e.type == SDL_EVENT_MOUSE_BUTTON_UP)
                    host.mouseButton(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                // Up is the wheel turned away from the player, whatever the
                // system's scroll direction ("natural" flips it).
                if (captured || host.wantsMouseButtons())
                    host.mouseWheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
                break;
            default: break;
            }
        }
        // Mouse look: capture the mouse while the game is taking input in a
        // world and the window has focus; menus and pauses release it.
        bool want = host.wantsMouseCapture() && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
        if (want != captured) {
            SDL_SetWindowRelativeMouseMode(window, want);
            captured = want;
        }
        Uint64 now = SDL_GetTicksNS();
        double dt = double(now - last) / 1e9;
        last = now;
        applyGraphics();  // what the menus changed last frame
        if (gpuScreen && host.client.screen) {
            // The 3D at the window's pixel size: as many texture pixels per
            // screen pixel as fit.
            float scale = 1;
            if (applied && applied->mode != 0) {
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(window, &pw, &ph);
                scale = std::min(float(pw) / float(host.client.screen->width),
                                 float(ph) / float(host.client.screen->height));
            }
            gpuScreen->setScale(scale);
        }
        host.frame(dt > 0.25 ? 0.25 : dt);
        if (audio) {
            const int ahead = shogo::Mixer::kRate * 6 / 100;  // frames
            int queued = SDL_GetAudioStreamQueued(audio) / 4;
            if (queued < ahead) {
                int n = ahead - queued;
                audioBuf.resize(size_t(n) * 2);
                shogo::mixer().mix(audioBuf.data(), n);
                SDL_PutAudioStreamData(audio, audioBuf.data(), n * 4);
            }
        }
    }
    if (audio) SDL_DestroyAudioStream(audio);
    // Settings (display, sound, controls, keys) for next time; the game
    // writes its menu settings into console variables as it shuts down.
    host.stopClient();
    if (!host.session.writeConfigFile("autoexec.cfg")) fprintf(stderr, "Couldn't save autoexec.cfg\n");
    if (!host.client.quitMessage.empty())
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Shogo", host.client.quitMessage.c_str(), window);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    host.session.renderer.setBackend(nullptr);  // GPU objects go before their device
    host.client.screenTarget = nullptr;
    gpuScreen.reset();
    gpu.shutdown();  // before the window goes
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
