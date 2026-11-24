// Shogo's game data for the app: importing it from GOG's installer or a
// Steam game folder into the folder the game runs in, and checking a folder
// holds a complete import.
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

// Steam's Shogo (app 610860) is Windows-only, so the Mac Steam app won't
// download it; SteamCMD can. The launcher shows these commands for the
// player to copy into Terminal (the download goes to ~/Downloads/Shogo Steam);
// the player then picks that folder. Nothing looks for it on its own.
extern const char kSteamCmdInstall[];   // the Homebrew command that installs SteamCMD
extern const char kSteamCmdDownload[];  // the SteamCMD command, with a username to fill in

// Copies the game out of Shogo's game folder from Steam (SteamCMD's download,
// or steamapps/common/Shogo copied from a PC) into `dir`: `folder` itself must
// hold the game (SHOGO.REZ). Saves and configs already in `dir` are kept;
// Steam's own files are left out. An unfinished SteamCMD download is refused.
bool importFromGameFolder(const std::string &folder, const std::string &dir,
                          const std::function<void(float, const std::string &)> &progress, std::string *err);

// A folder (Steam) or a file (GOG's installer).
bool importGame(const std::string &source, const std::string &dir,
                const std::function<void(float, const std::string &)> &progress, std::string *err);

// The Dock icon file in imported data (GOG's goggame-*.ico, or the icon taken
// from Steam's Start.exe); empty if there's none.
std::string findIconFile(const std::string &dir);

// `dir` holds a finished import (the marker file is written last).
bool hasGameData(const std::string &dir);

}  // namespace shogo

#endif
