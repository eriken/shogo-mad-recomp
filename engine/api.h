// The LithTech 1.0 engine API as seen from the engine side.
// DIRECTENGINE_COMPILE selects the engine-side struct layouts in the headers
// (e.g. the pointer-sized slot that skips a game object's C++ vtable).
#ifndef SHOGO_ENGINE_API_H
#define SHOGO_ENGINE_API_H

#define DIRECTENGINE_COMPILE
#include "client_de.h"
#include "clientshell_de.h"
#include "server_de.h"
#include "servershell_de.h"

#endif
