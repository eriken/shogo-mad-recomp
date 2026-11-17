// Real implementations of ClientDE entry points, installed over the generated stubs.
#ifndef SHOGO_ENGINE_CLIENT_API_H
#define SHOGO_ENGINE_CLIENT_API_H

#include "engine/fonts.h"
#include "engine/rez_file.h"
#include "engine/strings.h"
#include "engine/surfaces.h"

#include <functional>
#include <memory>
#include <string>

namespace shogo {

struct ClientContext {
    ResourceFS *resources = nullptr;
    EngineStrings *strings = nullptr;

    // The display: the screen surface is what FlipScreen presents.
    std::unique_ptr<Surface> screen;
    std::function<void(const Surface &)> present;  // called by FlipScreen (may be empty)
    unsigned framesPresented = 0;

    // Set when the game asks the engine to shut down.
    bool quitRequested = false;
    std::string quitMessage;

    // Game clock, advanced by the host loop (GetTime / GetFrameTime).
    double time = 0.0;
    double frameTime = 0.0;

    // Fallback font when the game passes a null HDEFONT.
    std::unique_ptr<Font> defaultFont;
};

// Installs everything implemented so far into the client's function table.
// screenWidth/Height is the initial render mode.
void installClientAPI(ClientDE *client, ClientContext *ctx, int screenWidth, int screenHeight);

}  // namespace shogo

#endif
