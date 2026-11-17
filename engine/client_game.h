// The client's side of a running game: starting the (local) server, passing
// messages both ways, client objects and cameras, and drawing the world.
#ifndef SHOGO_ENGINE_CLIENT_GAME_H
#define SHOGO_ENGINE_CLIENT_GAME_H

#include "engine/client_api.h"
#include "engine/input.h"
#include "engine/render_world.h"
#include "engine/server.h"

#include <string>

namespace shogo {

class GameSession {
public:
    Server server;
    ConsoleVars vars;
    Input input;
    WorldRenderer renderer;

    ClientDE *de = nullptr;
    ClientContext *ctx = nullptr;
    ClientShellDE *shell = nullptr;

    bool connected = false;
    int gameMode = GAMEMODE_NONE;
    std::string pendingWorld;  // StartGame with a world name: loaded on the next update

    // Installs the client game functions (call after installClientAPI).
    void installClient(ClientDE *de, ClientContext *ctx);
    // Runs the server for dt seconds and delivers its messages to the client shell.
    void update(double dt);

    static GameSession *instance;

    // Runs a config file (autoexec.cfg...): from the game folder, or the GOG
    // installer's copy in __support/app.
    bool readConfigFile(const std::string &name);

    // Internal state.
    std::vector<int> commandsOn;  // for OnCommandOn / OnCommandOff edges
    const WorldData *renderedWorld = nullptr;
    bool enteredWorld = false;
};

}  // namespace shogo

#endif
