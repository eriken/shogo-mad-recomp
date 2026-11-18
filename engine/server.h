// The server half of the engine: the game's object classes, the objects
// created from them, world loading, messages and the per-frame update.
#ifndef SHOGO_ENGINE_SERVER_H
#define SHOGO_ENGINE_SERVER_H

#include "engine/collision.h"
#include "engine/messages.h"
#include "engine/objects.h"
#include "engine/rez_file.h"
#include "engine/strings.h"
#include "engine/world_dat.h"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace shogo {

class ConsoleVars;

// A message on its way between the client and the server shells.
struct PendingMessage {
    uint8_t id = 0;
    std::unique_ptr<Message> msg;
};

// The one (local) client connected to this server.
struct ServerClient {
    static constexpr uint32_t kMagic = 0x434C4E54;  // "CLNT"
    uint32_t magic = kMagic;
    void *userData = nullptr;
    std::string name = "Player";
    DDWORD infoFlags = 0;
    EngineObject *object = nullptr;
    bool inWorld = false;
};

class Server {
public:
    ServerDE *de = nullptr;
    ServerShellDE *shell = nullptr;
    ResourceFS *fs = nullptr;
    EngineStrings *strings = nullptr;
    ConsoleVars *vars = nullptr;
    std::set<std::string> gameVarNames;  // set through SetGameConVar (saved with SAVEOBJECTS_SAVEGAMECONSOLE)

    std::vector<ClassDef *> classes;
    mutable std::unordered_map<std::string, ClassDef *> m_classByName;  // findClass's index (first of a name)
    mutable size_t m_classesIndexed = 0;
    std::unique_ptr<WorldData> world;
    std::vector<CollisionModel> collisionModels;  // one per BSP model
    EngineObject *worldObject = nullptr;          // GetWorldObject
    std::string worldName;
    bool worldRunning = false;
    double time = 0, frameTime = 0;
    ServerClient client;
    DVector globalForce{0, -2000, 0};
    std::vector<uint8_t> gameInfo;  // StartGameRequest::m_pGameInfo (GetGameInfo)
    SkyDef sky{};
    bool haveSky = false;
    std::vector<std::pair<EngineObject *, DDWORD>> skyObjects;

    // Messages for the client shell (OnMessage), and special effects (SpecialEffectNotify).
    std::deque<PendingMessage> toClient;
    std::deque<std::unique_ptr<Message>> instantEffects;
    // Messages from the client shell, delivered at the start of the next update.
    std::deque<PendingMessage> fromClient;
    // GetLastCollision: set while MID_TOUCHNOTIFY is being delivered.
    CollisionInfo lastCollision{};

    // Called once the world is running and the client has entered it.
    std::function<void()> onClientEnteredWorld;
    // Called before the client's world goes away (a new world is loading).
    std::function<void()> onClientLeavingWorld;
    // Called before a server object the client knows about is freed.
    std::function<void(EngineObject *)> removedCallback;
    // The client's input (IsCommandOn on the server).
    std::function<bool(int)> commandOn;

    // Installs the ServerDE functions. classes comes from ObjectDLLSetup.
    void install(ServerDE *de, ClassDef **classes, int nClasses);

    // Client connection (StartGame).
    void addClient();
    DRESULT loadWorld(const char *name, DDWORD flags);
    DRESULT runWorld();
    void update(double dt);

    // Object lifecycle.
    ClassDef *findClass(const char *name) const;
    EngineObject *createObject(ClassDef *cls, ObjectCreateStruct *ocs, float precreateKind,
                               const WorldObject *props, bool deferInitialUpdate = false);
    void removeObject(EngineObject *o);
    // Sends a trigger message ("TRIGGER", "LOCK"...) from the player to every object with this name (tests, probe).
    size_t triggerObjects(const std::string &name, const std::string &message);
    DDWORD engineMessage(EngineObject *o, DDWORD msg, void *data, float fdata);
    DDWORD objectMessage(EngineObject *to, EngineObject *from, DDWORD msg, Message *m);

    // Property source during MID_PRECREATE.
    const WorldObject *currentProps = nullptr;
    std::vector<WorldProp> stringProps;  // from CreateObjectProps

    static Server *instance;

private:
    void flushRemovals();
    void enterClient();
};

// Console variables shared by client and server ("runworld", game settings...).
class ConsoleVars {
public:
    struct Var {
        std::string name, value;
        float number = 0;
        // A config line with more than one argument ("ModelAdd 0 0 0"): all of
        // them, so the line can be written back as it was.
        std::vector<std::string> args;
    };
    Var *find(const std::string &name);
    Var *set(const std::string &name, const std::string &value);
    // "+name value", "name value", or several separated by ';'. Commands the
    // `commands` hook accepts (input bindings...) are passed to it instead.
    void run(const std::string &command);
    std::function<bool(const std::vector<std::string> &)> commands;
    // Every variable, by lower-case name.
    const std::map<std::string, std::unique_ptr<Var>> &all() const { return m_vars; }

private:
    std::map<std::string, std::unique_ptr<Var>> m_vars;  // key: lower case
};

}  // namespace shogo

#endif
