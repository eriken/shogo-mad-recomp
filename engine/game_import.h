// Shogo's game data for the app: importing it from GOG's installer into the
// folder the game runs in, and checking a folder holds a complete import.
#ifndef SHOGO_ENGINE_GAME_IMPORT_H
#define SHOGO_ENGINE_GAME_IMPORT_H

#include <functional>
#include <string>

namespace shogo {

// ~/Library/Application Support/Shogo Recomp/Game: the imported data, plus
// what the game writes there (saves, autoexec.cfg).
std::string defaultGameDataDir();

// Copies the game's files out of GOG's Shogo installer
// (setup_shogo_-_mobile_armor_division_*.exe) into `dir`: the English game,
// without GOG's installer extras. Saves already in `dir` are kept.
// progress(0..1, what) is called from the calling thread.
bool importFromGogInstaller(const std::string &installer, const std::string &dir,
                            const std::function<void(float, const std::string &)> &progress, std::string *err);

// `dir` holds a finished import (the marker file is written last).
bool hasGameData(const std::string &dir);

}  // namespace shogo

#endif
