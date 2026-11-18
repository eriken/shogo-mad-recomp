#include "engine/game_host.h"

#include "engine/interfaces.h"
#include "engine/effects.h"
#include "engine/sound.h"
#include "engine/server.h"
#include "engine/model.h"
#include "engine/messages.h"
#include "engine/stub_support.h"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>

namespace shogo {

namespace {
void *load(const char *path, std::string *err)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h && err) *err = std::string("dlopen ") + path + " failed: " + dlerror();
    return h;
}

template <class T> T sym(void *lib, const char *name, std::string *err)
{
    void *p = dlsym(lib, name);
    if (!p && err) *err = std::string("missing export ") + name;
    return reinterpret_cast<T>(p);
}

std::unique_ptr<PEResources> loadDll(const ResourceFS &fs, const char *name)
{
    auto image = fs.read(name);
    if (!image) {
        fprintf(stderr, "%s not found in the resource archives\n", name);
        return nullptr;
    }
    std::string why;
    auto res = PEResources::parse(std::move(*image), &why);
    if (!res) fprintf(stderr, "%s: %s\n", name, why.c_str());
    return res;
}
}  // namespace

bool GameHost::mountGameData(std::string *err)
{
    if (!resources.addArchive("SHOGO.REZ", err)) return false;
    resources.addArchive("ShogoP.rez");  // optional patch archive
    if (!resources.addArchive("SOUND.REZ")) fprintf(stderr, "SOUND.REZ not found: no sound effects\n");
    mixer().setResources(&resources);
    clientStrings = std::make_unique<EngineStrings>(loadDll(resources, "CRES.DLL"), "client");
    serverStrings = std::make_unique<EngineStrings>(loadDll(resources, "SRES.DLL"), "server");
    return true;
}

bool GameHost::loadServer(const char *path, std::string *err)
{
    serverModule = load(path, err);
    if (!serverModule) return false;
    serverDE = shogo_create_stub_server();
    installServerStrings(serverDE, serverStrings.get());
    installInterfaces(serverDE);
    installModelAPI(serverDE);
    installSoundAPI(serverDE, [] { return Server::instance ? Server::instance->client.object : nullptr; });
    using SetupFn = ClassDef **(*)(int *, ServerDE *, int *);
    auto setup = sym<SetupFn>(serverModule, "ObjectDLLSetup", err);
    auto getFns = sym<void (*)(CreateServerShellFn *, DeleteServerShellFn *)>(serverModule, "GetServerShellFunctions", err);
    if (!setup || !getFns) return false;
    int nDefs = 0, version = 0;
    ClassDef **defs = setup(&nDefs, serverDE, &version);
    session.server.fs = &resources;
    setModelFS(&resources);
    session.server.vars = &session.vars;
    session.server.strings = serverStrings.get();
    session.server.install(serverDE, defs, nDefs);
    CreateServerShellFn create = nullptr;
    DeleteServerShellFn destroy = nullptr;
    getFns(&create, &destroy);
    serverShell = create(serverDE);
    session.server.shell = serverShell;
    return serverShell != nullptr;
}

bool GameHost::startClient(const char *path, int width, int height, std::string *err)
{
    void *lib = load(path, err);
    if (!lib) return false;
    clientDE = shogo_create_stub_client();
    client.resources = &resources;
    client.strings = clientStrings.get();
    installClientAPI(clientDE, &client, width, height);
    session.installClient(clientDE, &client);
    installInterfaces(clientDE);
    installModelAPI(clientDE);
    installSoundAPI(clientDE);
    installEffectsAPI(clientDE);
    installMessageAPI(clientDE, clientStrings.get(), serverDE, serverStrings.get());
    if (!session.readConfigFile("autoexec.cfg")) fprintf(stderr, "[input] autoexec.cfg not found: no key bindings\n");
    else printf("[input] autoexec.cfg: %zu bindings\n", session.input.bindingCount());
    session.runCommandLine(commandLine);
    if (onConfigured) onConfigured();

    auto setInstance = sym<void (*)(void *)>(lib, "SetInstanceHandle", err);
    auto getFns = sym<void (*)(CreateClientShellFn *, DeleteClientShellFn *)>(lib, "GetClientShellFunctions", err);
    if (!setInstance || !getFns) return false;
    setInstance(nullptr);
    CreateClientShellFn create = nullptr;
    DeleteClientShellFn destroy = nullptr;
    getFns(&create, &destroy);
    clientShell = create(clientDE);
    session.shell = clientShell;
    if (!clientShell) {
        if (err) *err = "CreateClientShell returned null";
        return false;
    }

    RMode mode = {};
    clientDE->GetRenderMode(&mode);
    DGUID guid = {};
    DRESULT r = clientShell->OnEngineInitialized(clientShell, &mode, &guid);
    if (r != LT_OK || client.quitRequested) {
        if (err) *err = client.quitMessage.empty() ? "OnEngineInitialized failed" : client.quitMessage;
        return false;
    }
    return true;
}

void GameHost::frame(double dt)
{
    client.frameTime = dt;
    client.time += dt;
    session.update(dt);
    clientShell->PreUpdate(clientShell);
    clientShell->Update(clientShell);
    clientShell->PostUpdate(clientShell);
}

void GameHost::keyDown(int vk, int rep)
{
    if (rep <= 1) session.input.keyDown(scanCodeFromVK(vk));
    if (getenv("SHOGO_TRACE_INPUT"))
        printf("[input] key vk 0x%02X -> scan %d, forward=%d enabled=%d\n", vk, scanCodeFromVK(vk), session.input.isCommandOn(0),
               session.input.enabled);
    clientShell->OnKeyDown(clientShell, vk, rep);
}
void GameHost::keyUp(int vk)
{
    session.input.keyUp(scanCodeFromVK(vk));
    clientShell->OnKeyUp(clientShell, vk);
}

void GameHost::mouseMotion(float dx, float dy) { session.input.mouseMotion(dx, dy); }
void GameHost::mouseButton(int button, bool down) { session.input.mouseButton(button, down); }
void GameHost::mouseWheel(float notches) { session.input.mouseWheel(notches); }
bool GameHost::wantsMouseCapture() const { return session.server.worldRunning && session.input.enabled; }

}  // namespace shogo
