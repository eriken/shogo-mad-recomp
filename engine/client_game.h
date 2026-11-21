// The client's side of a running game: starting the (local) server, passing
// messages both ways, client objects and cameras, and drawing the world.
#ifndef SHOGO_ENGINE_CLIENT_GAME_H
#define SHOGO_ENGINE_CLIENT_GAME_H

#include "engine/client_api.h"
#include "engine/input.h"
#include "engine/render_world.h"
#include "engine/server.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

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
    // Hands the first `count` queued server messages to the client shell.
    void deliverMessages(size_t count);

    static GameSession *instance;

    // Runs a config file (autoexec.cfg...): from the game folder, or the GOG
    // installer's copy in __support/app.
    bool readConfigFile(const std::string &name);
    // Saves every console variable and the key bindings to a config file in
    // the game folder (the game reads autoexec.cfg at start, as the original
    // engine wrote it at exit). Variables the command line set are saved
    // with the value they had before it.
    bool writeConfigFile(const std::string &name);
    // Runs "+name value" settings from the command line, over the config.
    void runCommandLine(const std::vector<std::string> &commands);

    // Internal state.
    std::vector<int> commandsOn;  // for OnCommandOn / OnCommandOff edges
    const WorldData *renderedWorld = nullptr;
    bool enteredWorld = false;
    // The last camera drawn (F12 writes it to positions.txt).
    DVector lastCameraPos{}, lastCameraForward{0, 0, 1};
    // Command-line variables: lower-case name -> value before (absent: none).
    std::map<std::string, std::optional<std::string>> commandLineVars;
};

}  // namespace shogo

#endif
