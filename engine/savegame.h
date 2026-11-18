// Saved games and level changes: SaveObjects / RestoreObjects and the client
// references a restore leaves behind.
//
// The file format is our own ("SHSV"); Shogo's original .sav files are not
// read. Each object stores what the engine needs to recreate it, the game's
// own MID_SAVEOBJECT data, and its inter-object links. Restoring creates every
// object first (MID_PRECREATE with PRECREATE_SAVEGAME), then sends each one
// MID_LOADOBJECT, then MID_INITIALUPDATE with INITIALUPDATE_SAVEGAME. Object
// references inside the game's data are saved object ids, mapped to the new
// objects while loading (ids of objects that weren't saved read as null).
//
// The object that was a client's player becomes a client reference
// (GetNextClientRef): the game's OnClientEnterWorld finds it there and puts
// the player back into it instead of making a new one. Level changes use the
// same path for the "keep alive" objects (the player and what it carries).
#ifndef SHOGO_ENGINE_SAVEGAME_H
#define SHOGO_ENGINE_SAVEGAME_H

#include "engine/api.h"

#include <string>

namespace shogo {

struct EngineObject;

struct ClientRef {
    static constexpr uint32_t kMagic = 0x43524546;  // "CREF"
    uint32_t magic = kMagic;
    DDWORD infoFlags = 0;
    std::string name;
    EngineObject *object = nullptr;
};

// Fills SaveObjects, RestoreObjects and the client-reference functions.
void installSaveAPI(ServerDE *server);

// Drops the client references (a new world is loading).
void clearClientRefs();

// Forgets references to an object that is being freed.
void saveObjectRemoved(EngineObject *o);

// Where a save file named by the game lives ('\' becomes '/', folders are made).
std::string savePath(const char *name);

}  // namespace shogo

#endif
