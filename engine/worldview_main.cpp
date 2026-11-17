// shogo-worldview: flies through a level with the world renderer.
//
//   shogo-worldview WORLDS/01_AMBUSH.DAT                 window: WASD/arrows move, mouse looks,
//                                                         Q/E down/up, Shift = fast, Esc quits
//   shogo-worldview WORLDS/01_AMBUSH.DAT --screenshot out.png [--pos x y z] [--yaw deg] [--pitch deg]
//
// Run from the extracted game folder.
#include "engine/image_write.h"
#include "engine/render_world.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace shogo;

int main(int argc, char **argv)
{
    const char *worldName = nullptr, *shot = nullptr;
    int width = 640, height = 480;
    bool havePos = false;
    Camera cam;
    float yawDeg = NAN, pitchDeg = NAN;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--pos") && i + 3 < argc) {
            cam.pos = {strtof(argv[i + 1], nullptr), strtof(argv[i + 2], nullptr), strtof(argv[i + 3], nullptr)};
            havePos = true;
            i += 3;
        } else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) yawDeg = strtof(argv[++i], nullptr);
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) pitchDeg = strtof(argv[++i], nullptr);
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            width = atoi(argv[i + 1]);
            height = atoi(argv[i + 2]);
            i += 2;
        } else worldName = argv[i];
    }
    if (!worldName) {
        fprintf(stderr, "usage: %s WORLDS/<name>.DAT [--screenshot out.png] [--pos x y z] [--yaw deg] [--pitch deg] [--size w h]\n", argv[0]);
        return 2;
    }

    ResourceFS fs;
    std::string err;
    if (!fs.addArchive("SHOGO.REZ", &err)) {
        fprintf(stderr, "%s (run from the game folder)\n", err.c_str());
        return 1;
    }
    fs.addArchive("ShogoP.rez");
    auto data = fs.read(worldName);
    if (!data) {
        fprintf(stderr, "%s not found\n", worldName);
        return 1;
    }
    auto t0 = std::chrono::steady_clock::now();
    auto world = WorldData::parse(*data, &err);
    if (!world) {
        fprintf(stderr, "%s: %s\n", worldName, err.c_str());
        return 1;
    }
    WorldRenderer renderer;
    renderer.load(*world, fs);
    auto t1 = std::chrono::steady_clock::now();
    const auto &st = renderer.stats();
    printf("%s: %s\n  %zu polys, %zu sky polys, %zu textures (%zu missing), loaded in %.0f ms\n", worldName,
           world->info.c_str(), st.polys, st.skyPolys, st.textures, st.missingTextures,
           std::chrono::duration<double, std::milli>(t1 - t0).count());

    Camera start;
    if (renderer.startPoint(start) && !havePos) cam = start;
    else if (!havePos) {
        const WorldBsp &m = world->models[0];
        cam.pos = {(m.boxMin.x + m.boxMax.x) / 2, (m.boxMin.y + m.boxMax.y) / 2, (m.boxMin.z + m.boxMax.z) / 2};
    }
    if (!std::isnan(yawDeg)) cam.yaw = yawDeg * float(M_PI / 180);
    if (!std::isnan(pitchDeg)) cam.pitch = pitchDeg * float(M_PI / 180);
    printf("  camera at (%.0f, %.0f, %.0f) yaw %.0f pitch %.0f\n", cam.pos.x, cam.pos.y, cam.pos.z,
           cam.yaw * 180 / M_PI, cam.pitch * 180 / M_PI);

    Surface frame(width, height);
    if (shot) {
        auto r0 = std::chrono::steady_clock::now();
        renderer.render(frame, cam);
        auto r1 = std::chrono::steady_clock::now();
        printf("  rendered %zu polys in %.1f ms\n", renderer.stats().drawnPolys,
               std::chrono::duration<double, std::milli>(r1 - r0).count());
        bool ok = writePNG(frame, shot);
        printf("  screenshot %s: %s\n", ok ? "written" : "FAILED", shot);
        return ok ? 0 : 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = nullptr;
    SDL_Renderer *sdl = nullptr;
    SDL_CreateWindowAndRenderer("Shogo world viewer", width * 2, height * 2,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &sdl);
    SDL_SetRenderVSync(sdl, 1);
    SDL_SetRenderLogicalPresentation(sdl, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture *tex = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetWindowRelativeMouseMode(window, true);

    Uint64 last = SDL_GetTicksNS();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = false;
            else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                cam.yaw += e.motion.xrel * 0.004f;
                cam.pitch = std::clamp(cam.pitch + e.motion.yrel * 0.004f, -1.5f, 1.5f);
            }
        }
        Uint64 now = SDL_GetTicksNS();
        float dt = float(now - last) / 1e9f;
        last = now;
        const bool *k = SDL_GetKeyboardState(nullptr);
        float speed = (k[SDL_SCANCODE_LSHIFT] ? 2400.0f : 600.0f) * dt;
        float fx = std::sin(cam.yaw) * std::cos(cam.pitch), fy = -std::sin(cam.pitch),
              fz = std::cos(cam.yaw) * std::cos(cam.pitch);
        float rx = std::cos(cam.yaw), rz = -std::sin(cam.yaw);
        float f = (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) - (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]);
        float s = (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) - (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]);
        float u = k[SDL_SCANCODE_E] - k[SDL_SCANCODE_Q];
        cam.pos.x += (fx * f + rx * s) * speed;
        cam.pos.y += (fy * f + u) * speed;
        cam.pos.z += (fz * f + rz * s) * speed;

        std::fill(frame.pixels.begin(), frame.pixels.end(), 0);
        renderer.render(frame, cam);
        SDL_UpdateTexture(tex, nullptr, frame.pixels.data(), width * 4);
        SDL_RenderClear(sdl);
        SDL_RenderTexture(sdl, tex, nullptr, nullptr);
        SDL_RenderPresent(sdl);
    }
    SDL_Quit();
    return 0;
}
