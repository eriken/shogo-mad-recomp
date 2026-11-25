// Shogo's game data for the app: importing it from GOG's installer or a
// Steam game folder into the folder the game runs in, checking a folder holds
// a complete import, and removing it again. The player's own files (saves,
// autoexec.cfg) live apart from the game data and outlive it.
#ifndef SHOGO_ENGINE_GAME_IMPORT_H
#define SHOGO_ENGINE_GAME_IMPORT_H

#include <functional>
#include <string>

namespace shogo {

// ~/Games/Shogo: the imported game data (the game runs in it).
std::string defaultGameDataDir();

// ~/Library/Application Support/Shogo Recomp/Game: the player's files, what
// the game writes (Save/, autoexec.cfg). Older versions kept the game data here too.
std::string playerDataDir();

// The environment variable naming the player's folder to the game: while it's
// set, the game's paths to its own files (Save\..., autoexec.cfg) lead there
// (shogo_native_path), not into the game data.
extern const char kPlayerDirVar[];

// Saves and settings: a path relative to the game folder ("Save/x.sav",
// "autoexec.cfg") that's the player's, not the game data's.
bool isPlayerFile(const std::string &relativePath);

// Moves an import made by an older version out of playerDataDir() into `dir`, leaving
// the player's files where they are. Nothing to do: true.
bool moveOldGameData(const std::string &dir, std::string *err);

// Removes the game data in `dir` (an import: it has the marker or SHOGO.REZ);
// the folder above it (~/Games) stays. The player's files are kept: none are
// in `dir`, and a folder with any in it (Save/, autoexec.cfg) is refused.
bool uninstallGame(const std::string &dir, std::string *err);

// The imports copy the game into `dir`; the player's files the source has
// (Steam's Save/ and autoexec.cfg, GOG's default autoexec.cfg) go into
// `playerDir`, unless the player already has them there. `playerDir` may be
// `dir` (a self-contained game folder, for testing).

// Copies the game's files out of GOG's Shogo installer
// (setup_shogo_-_mobile_armor_division_*.exe) into `dir`: the English game,
// without GOG's installer extras. progress(0..1, what) is called from the
// calling thread.
bool importFromGogInstaller(const std::string &installer, const std::string &dir, const std::string &playerDir,
                            const std::function<void(float, const std::string &)> &progress, std::string *err);

// Steam's Shogo (app 610860) is Windows-only, so the Mac Steam app won't
// download it; SteamCMD can. The launcher shows these commands for the
// player to copy into Terminal (the download goes to ~/Downloads/Shogo Steam);
// the player then picks that folder. Nothing looks for it on its own.
extern const char kSteamCmdInstall[];   // the Homebrew command that installs SteamCMD
extern const char kSteamCmdDownload[];  // the SteamCMD command, with a username to fill in

// Copies the game out of Shogo's game folder from Steam (SteamCMD's download,
// or steamapps/common/Shogo copied from a PC) into `dir`: `folder` itself must
// hold the game (SHOGO.REZ). Steam's own files are left out. An unfinished
// SteamCMD download is refused.
bool importFromGameFolder(const std::string &folder, const std::string &dir, const std::string &playerDir,
                          const std::function<void(float, const std::string &)> &progress, std::string *err);

// A folder (Steam) or a file (GOG's installer).
bool importGame(const std::string &source, const std::string &dir, const std::string &playerDir,
                const std::function<void(float, const std::string &)> &progress, std::string *err);

// The Dock icon file in imported data (GOG's goggame-*.ico, or the icon taken
// from Steam's Start.exe); empty if there's none.
std::string findIconFile(const std::string &dir);

// `dir` holds a finished import (the marker file is written last).
bool hasGameData(const std::string &dir);

}  // namespace shogo

#endif
