// shogo-probe: loads the natively built Shogo game modules against the engine
// (real functions where implemented, logging stubs elsewhere), drives startup
// and a number of headless frames, and reports which engine calls the game made.
//
// Usage: shogo-probe [+var value ...] [--frames N] [--movies] [--save-config] [--gpu [--smooth] [--msaa N] [--gpu-scale S] | --gpu-reference] [--screenshot out.png] [--audio-out out.wav] [--key VK@frame ...] [--click BUTTON@frame ...] [--hold VK@from-to ...] [--mouse DX,DY@from-to ...]
//                    <object.lto.dylib> <cshell.dylib>
// Run it from the extracted game folder.
#include "engine/game_host.h"
#include "engine/sound.h"
#include "engine/image_write.h"
#include "engine/gpu_screen.h"
#include "engine/render_gpu.h"
#include "engine/movement.h"
#include "engine/server.h"
#include "engine/stub_support.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

const char *shogo_stub_last_call();
void shogo_stub_print_summary(FILE *out);

namespace {

// The engine's view of a game shell object: the C++ vtable pointer, then the
// callback table the game's constructor filled in.
static_assert(offsetof(ClientShellDE, OnEngineInitialized) == sizeof(void *),
              "client shell callbacks must follow the vtable pointer");

void onCrash(int sig)
{
    fprintf(stderr, "\n*** signal %d (%s). Last engine call: %s\n", sig, strsignal(sig), shogo_stub_last_call());
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    shogo_stub_print_summary(stderr);
    _exit(128 + sig);
}

[[noreturn]] void fail(const std::string &msg)
{
    fprintf(stderr, "%s\n", msg.c_str());
    exit(1);
}

struct KeyPress {
    int vk, frame;
};
struct KeyHold {
    int vk, from, to;  // held down from frame `from` until frame `to`
};

}  // namespace

int main(int argc, char **argv)
{
    int frames = 0;
    const char *screenshot = nullptr;
    const char *audioOut = nullptr;  // records the sound mix to a WAV file
    std::vector<int16_t> audio;
    std::vector<KeyPress> keys;
    std::vector<KeyPress> clicks;  // mouse button (0 left, 1 right, 2 middle) @ frame
    std::vector<KeyHold> holds;
    struct MouseMove { float dx, dy; int from, to; };
    std::vector<MouseMove> mouse;
    struct Trigger { std::string name; int frame; };
    std::vector<Trigger> triggers;  // a "TRIGGER" message from the player to named objects
    std::vector<const char *> paths;
    std::vector<std::string> conVars;  // "+name value" pairs, set before the game starts
    bool gpu = false;                   // draw the world with the GPU backend
    bool gpuReference = false;          // software drawing, with a GPU device created as for --gpu
    shogo::GpuSettings gpuSettings;     // --smooth, --msaa N
    float gpuScale = 1;                 // --gpu-scale S: the GPU screen at S times the game's resolution
    bool saveConfig = false;            // write autoexec.cfg at the end, as the game does at exit
    bool movies = false;               // play the intro movies (otherwise skipped, so key timings stay put)
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '+' && i + 1 < argc) {
            conVars.push_back(std::string(argv[i] + 1) + " \"" + argv[i + 1] + "\"");
            ++i;
            continue;
        }
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!strcmp(argv[i], "--audio-out") && i + 1 < argc) audioOut = argv[++i];
        else if (!strcmp(argv[i], "--movies")) movies = true;
        else if (!strcmp(argv[i], "--save-config")) saveConfig = true;
        else if (!strcmp(argv[i], "--gpu")) gpu = true;
        else if (!strcmp(argv[i], "--gpu-reference")) gpuReference = true;
        else if (!strcmp(argv[i], "--smooth")) gpuSettings.smoothTextures = true;
        else if (!strcmp(argv[i], "--msaa") && i + 1 < argc) gpuSettings.msaa = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gpu-scale") && i + 1 < argc) gpuScale = float(atof(argv[++i]));
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
            int b = 0, at = 0;
            if (sscanf(argv[++i], "%i@%d", &b, &at) == 2) clicks.push_back({b, at});
        } else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            int vk = 0, at = 0;
            if (sscanf(argv[++i], "%i@%d", &vk, &at) == 2) keys.push_back({vk, at});
        } else if (!strcmp(argv[i], "--hold") && i + 1 < argc) {
            int vk = 0, from = 0, to = 0;
            if (sscanf(argv[++i], "%i@%d-%d", &vk, &from, &to) == 3) holds.push_back({vk, from, to});
        } else if (!strcmp(argv[i], "--mouse") && i + 1 < argc) {
            MouseMove m{};
            if (sscanf(argv[++i], "%f,%f@%d-%d", &m.dx, &m.dy, &m.from, &m.to) == 4) mouse.push_back(m);
        } else if (!strcmp(argv[i], "--trigger") && i + 1 < argc) {
            std::string arg = argv[++i];
            size_t at = arg.rfind('@');
            if (at != std::string::npos) triggers.push_back({arg.substr(0, at), atoi(arg.c_str() + at + 1)});
        } else paths.push_back(argv[i]);
    }
    if (paths.size() != 2) {
        fprintf(stderr,
                "usage: %s [+var value ...] [--frames N] [--screenshot out.png] [--audio-out out.wav] [--key VK@frame ...] [--click BUTTON@frame ...] [--hold VK@from-to ...] [--mouse DX,DY@from-to ...] [--trigger NAME@frame ...] <object.lto.dylib> <cshell.dylib>\n",
                argv[0]);
        return 2;
    }
    for (int s : {SIGSEGV, SIGBUS, SIGABRT, SIGILL}) signal(s, onCrash);
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (!movies) conVars.insert(conVars.begin(), "DisableMovies \"1\"");
    // --gpu: the screen lives on the GPU (headless), drawn there by the GPU
    // world renderer and the 2D drawing. --gpu-reference: a software
    // screenshot taken under the same conditions (a GPU device exists), to
    // compare the two.
    std::unique_ptr<shogo::GpuScreen> gpuScreen;  // outlives host's renderer
    shogo::GameHost host;
    host.commandLine = conVars;  // run after autoexec.cfg, as the game does
    std::unique_ptr<shogo::RenderBackend> gpuDevice;
    if (gpu || gpuReference) {
        gpuScreen = shogo::GpuScreen::create(nullptr);
        if (!gpuScreen) return 1;
        auto backend = shogo::makeGpuBackend(gpuScreen->device(), gpuScreen.get(), gpuSettings);
        gpuScreen->setScale(gpuScale);
        if (!backend) return 1;
        if (gpuReference) {
            gpuDevice = std::move(backend);  // kept alive, not drawn with
        } else {
            host.session.renderer.setBackend(std::move(backend));
            host.client.screenTarget = gpuScreen.get();
            host.client.rendererOnScreenTarget = true;
        }
    }
    std::string err;
    if (!host.mountGameData(&err)) fail(err + "\n(run shogo-probe from the extracted game folder)");
    printf("Mounted %zu resource archive(s); CRES.DLL has %zu strings, SRES.DLL has %zu\n",
           host.resources.archiveCount(),
           host.clientStrings->resources() ? host.clientStrings->resources()->stringCount() : 0,
           host.serverStrings->resources() ? host.serverStrings->resources()->stringCount() : 0);

    printf("== Server game module: %s\n", paths[0]);
    if (!host.loadServer(paths[0], &err)) fail(err);
    printf("Server shell created\n");

    printf("\n== Client game module: %s\n", paths[1]);
    if (!host.startClient(paths[1], 640, 480, &err)) fail("Client failed to start: " + err);
    printf("Client initialized\n");

    for (int i = 0; i < frames && !host.quitRequested(); ++i) {
        for (const KeyPress &c : clicks)
            if (c.frame == i) {
                printf("[frame %d] mouse button %d\n", i, c.vk);
                host.mouseButton(c.vk, true);
                host.mouseButton(c.vk, false);
            }
        for (const KeyPress &k : keys)
            if (k.frame == i) {
                printf("[frame %d] key 0x%02X\n", i, k.vk);
                host.keyDown(k.vk, 1);
                host.keyUp(k.vk);
            }
        for (const Trigger &t : triggers)
            if (t.frame == i && shogo::Server::instance)
                printf("[frame %d] trigger %s: %zu object(s)\n", i, t.name.c_str(),
                       shogo::Server::instance->triggerObjects(t.name, "TRIGGER"));
        for (const KeyHold &k : holds) {
            if (k.from == i) host.keyDown(k.vk, 1);
            if (k.to == i) host.keyUp(k.vk);
        }
        for (const MouseMove &m : mouse)
            if (i >= m.from && i < m.to) host.mouseMotion(m.dx, m.dy);
        host.frame(1.0 / 30.0);
        {
            // 1/30 s of sound per frame, as the window would queue it.
            int16_t buf[shogo::Mixer::kRate / 30 * 2];
            shogo::mixer().mix(buf, shogo::Mixer::kRate / 30);
            if (audioOut) audio.insert(audio.end(), buf, buf + shogo::Mixer::kRate / 30 * 2);
        }
        if (getenv("SHOGO_TRACE_PLAYER") && host.session.server.client.object) {
            const DVector &p = host.session.server.client.object->pos;
            shogo::EngineObject *po = host.session.server.client.object;
            printf("[frame %d] player %.1f %.1f %.1f anim %d %s", i, p.x, p.y, p.z, po->anim, po->filename.c_str());
            for (shogo::EngineObject *o : shogo::objects().all())
                if (o->clientOnly && (o->flags & FLAG_STAIRSTEP))
                    printf("  mover %.1f %.1f %.1f v %.0f %.0f %.0f a %.0f %.0f %.0f dims %.0f %.0f %.0f ground %d", o->pos.x,
                           o->pos.y, o->pos.z, o->velocity.x, o->velocity.y, o->velocity.z, o->accel.x, o->accel.y,
                           o->accel.z, o->dims.x, o->dims.y, o->dims.z,
                           (int)shogo::standingOn(host.session.server, o, nullptr));
            for (shogo::EngineObject *o : shogo::objects().all())
                if (o->clientOnly && (o->flags & FLAG_STAIRSTEP)) {
                    // Facing: the rotated +z axis.
                    const DRotation &q = o->rot;
                    float x = q.m_Vec.x, y = q.m_Vec.y, z = q.m_Vec.z, w = q.m_Spin;
                    printf(" anim %d facing %.2f %.2f %.2f", o->anim, 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y));
                }
            printf("\n");
        }
    }
    printf("Ran %d frame(s), %u presented%s\n", frames, host.client.framesPresented,
           host.quitRequested() ? " (game requested shutdown)" : "");
    if (screenshot) {
        bool ok;
        if (gpuScreen && host.client.screenTarget && gpuScreen->scale() != 1) {  // at the texture's resolution
            ok = shogo::writePNG(gpuScreen->capture(*host.client.screen), screenshot);
        } else {
            if (host.client.screenTarget) host.client.screenTarget->toCPU(*host.client.screen);
            ok = shogo::writePNG(*host.client.screen, screenshot);
        }
        printf("Screenshot %s: %s\n", ok ? "written" : "FAILED", screenshot);
    }
    if (audioOut) {
        int peak = 0;
        for (int16_t v : audio) peak = std::max(peak, std::abs(int(v)));
        bool ok = shogo::writeWav(audioOut, audio, shogo::Mixer::kRate);
        printf("Audio %s: %s (%.1f s, %zu sounds started, peak %d)\n", ok ? "written" : "FAILED", audioOut,
               audio.size() / 2.0 / shogo::Mixer::kRate, shogo::mixer().started(), peak);
    }
    if (saveConfig) printf("autoexec.cfg %s\n", host.session.writeConfigFile("autoexec.cfg") ? "saved" : "NOT saved");
    shogo_stub_print_summary(stdout);
    return 0;
}
