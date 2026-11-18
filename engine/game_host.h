// Loads the game modules against the engine and drives the client shell.
// Shared by the windowed game (shogo) and the headless probe (shogo-probe).
#ifndef SHOGO_ENGINE_GAME_HOST_H
#define SHOGO_ENGINE_GAME_HOST_H

#include "engine/client_api.h"
#include "engine/client_game.h"

#include <string>

namespace shogo {

class GameHost {
public:
    ResourceFS resources;
    std::unique_ptr<EngineStrings> clientStrings, serverStrings;
    ClientContext client;
    GameSession session;  // the local server and the client's view of the game
    ClientDE *clientDE = nullptr;
    ServerDE *serverDE = nullptr;
    ClientShellDE *clientShell = nullptr;
    ServerShellDE *serverShell = nullptr;
    void *serverModule = nullptr;  // object.lto.dylib, for callers that want more exports
    // "+name value" settings from the command line: run after autoexec.cfg.
    std::vector<std::string> commandLine;

    // Mounts SHOGO.REZ (+ ShogoP.rez) from the working directory and loads the string DLLs.
    bool mountGameData(std::string *err);
    bool loadServer(const char *path, std::string *err);
    // Creates the client shell and calls OnEngineInitialized at width x height.
    bool startClient(const char *path, int width, int height, std::string *err);
    // One game frame of dt seconds.
    void frame(double dt);
    void keyDown(int vk, int rep);
    void keyUp(int vk);
    // Mouse: relative motion in pixels, DirectInput buttons (0 left, 1 right,
    // 2 middle), wheel in notches.
    void mouseMotion(float dx, float dy);
    void mouseButton(int button, bool down);
    void mouseWheel(float notches);
    // True while the game wants mouse look (in a world, input enabled).
    bool wantsMouseCapture() const;
    // True while a menu waits for a mouse button to bind (the keyboard menu).
    bool wantsMouseButtons() const { return session.input.trackingMouse(); }
    bool quitRequested() const { return client.quitRequested; }
};

}  // namespace shogo

#endif
