// shogo: the game in a window (SDL3). Run from the extracted game folder, or pass
// --data <dir>. Modules default to object.lto.dylib / cshell.dylib next to the executable.
#include "engine/game_host.h"
#include "engine/sound.h"

#include <SDL3/SDL.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
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

std::string exeDir(const char *argv0)
{
    const char *base = SDL_GetBasePath();
    if (base) return base;
    std::string s(argv0);
    size_t slash = s.rfind('/');
    return slash == std::string::npos ? "./" : s.substr(0, slash + 1);
}

}  // namespace

int main(int argc, char **argv)
{
    std::string dir = exeDir(argv[0]);
    std::string objectPath = dir + "object.lto.dylib", cshellPath = dir + "cshell.dylib";
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
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Shogo: Mobile Armor Division", 1280, 960, SDL_WINDOW_RESIZABLE |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    // Sound: the mixer runs on this thread; each frame tops the device queue up
    // to about 60 ms ahead.
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, shogo::Mixer::kRate};
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else fprintf(stderr, "No audio device: %s\n", SDL_GetError());
    std::vector<int16_t> audioBuf;
    SDL_Texture *texture = nullptr;
    int texW = 0, texH = 0;

    shogo::GameHost host;
    for (const std::string &v : conVars) host.session.vars.run(v);
    host.client.present = [&](const shogo::Surface &s) {
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

    std::string err;
    if (!host.mountGameData(&err) || !host.loadServer(objectPath.c_str(), &err) ||
        !host.startClient(cshellPath.c_str(), 640, 480, &err)) {
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
                if (captured || e.type == SDL_EVENT_MOUSE_BUTTON_UP) host.mouseButton(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                if (captured) host.mouseWheel(e.wheel.y);
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
    if (!host.client.quitMessage.empty())
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Shogo", host.client.quitMessage.c_str(), window);
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
