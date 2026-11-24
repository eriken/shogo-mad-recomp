// The launcher: the app's first window when it's started without a game
// folder (from Finder). It imports Shogo's data from GOG's installer or a
// Steam game folder into Application Support, shows the notice Monolith's
// source licence asks of variations, and starts the game as a separate
// process (this app with --play), staying open while it runs.
#ifndef SHOGO_ENGINE_LAUNCHER_H
#define SHOGO_ENGINE_LAUNCHER_H

#include <string>

struct SDL_Surface;

namespace shogo {

// The Dock icon: GOG's icon (goggame-*.ico), or the one taken from Steam's
// Start.exe, in the imported game data, as an SDL surface for
// SDL_SetWindowIcon; null if there's none. Never part of the app itself: the
// art is Monolith's, GOG's and Steam's.
SDL_Surface *loadDockIcon(const std::string &dataDir);

// exe: this program's path, run with --play to start the game.
int runLauncher(const std::string &exe);

}  // namespace shogo

#endif
