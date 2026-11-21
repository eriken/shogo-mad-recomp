#include "engine/savegame.h"

#include "engine/messages.h"
#include "engine/model.h"
#include "engine/objects.h"
#include "engine/server.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <strings.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace shogo {

namespace {

constexpr uint32_t kVersion = 2;

std::vector<std::unique_ptr<ClientRef>> g_clientRefs;

bool tracing() { return getenv("SHOGO_TRACE_SAVE") != nullptr; }

void putStr(Message &m, const std::string &s)
{
    m.put<uint32_t>((uint32_t)s.size());
    m.put(s.data(), s.size());
}
std::string getStr(Message &m)
{
    uint32_t n = m.get<uint32_t>();
    if (n > m.data.size() - m.pos) {
        m.pos = m.data.size();
        return {};
    }
    std::string s((const char *)&m.data[m.pos], n);
    m.pos += n;
    return s;
}

// Everything about one saved object.
struct Record {
    std::string className;
    uint32_t id = 0;
    int32_t type = OT_NORMAL;
    DDWORD flags = 0, userFlags = 0, clientFlags = 0;
    DVector pos{}, scale{}, dims{}, velocity{}, accel{};
    DRotation rot{0, 0, 0, 1};
    float color[4] = {1, 1, 1, 1};
    float mass = 1, friction = 1, forceIgnoreLimit = 0;
    float lightColor[3] = {1, 1, 1};
    float lightRadius = 0;
    std::string filename, skin, name;
    uint64_t userData = 0;
    D_WORD containerCode = 0;
    DBYTE blockingPriority = 0;
    bool active = true;
    float deactivationTime = 0;
    int32_t state = 0;
    double nextUpdateIn = 0;  // seconds from the save, 0 = none
    int32_t anim = -1;
    float animTime = 0;
    bool animLoop = true, animDone = false;
    std::vector<uint8_t> hiddenNodes;
    int32_t bsp = -1;
    DVector basePos{};
    DRotation baseRot{0, 0, 0, 1};
    std::vector<uint8_t> sfx;
    std::vector<uint32_t> links;
    bool client = false;  // a client's player object
    std::string clientName;
    DDWORD clientFlags2 = 0;
    std::vector<uint8_t> gameData;  // MID_SAVEOBJECT
};

template <class T> void putVec(Message &m, const std::vector<T> &v)
{
    m.put<uint32_t>((uint32_t)v.size());
    if (!v.empty()) m.put(v.data(), v.size() * sizeof(T));
}
template <class T> std::vector<T> getVec(Message &m)
{
    uint32_t n = m.get<uint32_t>();
    std::vector<T> v;
    if (size_t(n) * sizeof(T) > m.data.size() - m.pos) {
        m.pos = m.data.size();
        return v;
    }
    v.resize(n);
    if (n) m.get(v.data(), n * sizeof(T));
    return v;
}

void writeRecord(Message &out, const Record &r)
{
    putStr(out, r.className);
    out.put(r.id);
    out.put(r.type);
    out.put(r.flags);
    out.put(r.userFlags);
    out.put(r.clientFlags);
    out.put(r.pos);
    out.put(r.rot);
    out.put(r.scale);
    out.put(r.dims);
    out.put(r.velocity);
    out.put(r.accel);
    out.put(r.color);
    out.put(r.mass);
    out.put(r.friction);
    out.put(r.forceIgnoreLimit);
    out.put(r.lightColor);
    out.put(r.lightRadius);
    putStr(out, r.filename);
    putStr(out, r.skin);
    putStr(out, r.name);
    out.put(r.userData);
    out.put(r.containerCode);
    out.put(r.blockingPriority);
    out.put<uint8_t>(r.active);
    out.put(r.deactivationTime);
    out.put(r.state);
    out.put(r.nextUpdateIn);
    out.put(r.anim);
    out.put(r.animTime);
    out.put<uint8_t>(r.animLoop);
    out.put<uint8_t>(r.animDone);
    putVec(out, r.hiddenNodes);
    out.put(r.bsp);
    out.put(r.basePos);
    out.put(r.baseRot);
    putVec(out, r.sfx);
    putVec(out, r.links);
    out.put<uint8_t>(r.client);
    putStr(out, r.clientName);
    out.put(r.clientFlags2);
    putVec(out, r.gameData);
}

Record readRecord(Message &in)
{
    Record r;
    r.className = getStr(in);
    r.id = in.get<uint32_t>();
    r.type = in.get<int32_t>();
    r.flags = in.get<DDWORD>();
    r.userFlags = in.get<DDWORD>();
    r.clientFlags = in.get<DDWORD>();
    r.pos = in.get<DVector>();
    r.rot = in.get<DRotation>();
    r.scale = in.get<DVector>();
    r.dims = in.get<DVector>();
    r.velocity = in.get<DVector>();
    r.accel = in.get<DVector>();
    in.get(r.color, sizeof(r.color));
    r.mass = in.get<float>();
    r.friction = in.get<float>();
    r.forceIgnoreLimit = in.get<float>();
    in.get(r.lightColor, sizeof(r.lightColor));
    r.lightRadius = in.get<float>();
    r.filename = getStr(in);
    r.skin = getStr(in);
    r.name = getStr(in);
    r.userData = in.get<uint64_t>();
    r.containerCode = in.get<D_WORD>();
    r.blockingPriority = in.get<DBYTE>();
    r.active = in.get<uint8_t>() != 0;
    r.deactivationTime = in.get<float>();
    r.state = in.get<int32_t>();
    r.nextUpdateIn = in.get<double>();
    r.anim = in.get<int32_t>();
    r.animTime = in.get<float>();
    r.animLoop = in.get<uint8_t>() != 0;
    r.animDone = in.get<uint8_t>() != 0;
    r.hiddenNodes = getVec<uint8_t>(in);
    r.bsp = in.get<int32_t>();
    r.basePos = in.get<DVector>();
    r.baseRot = in.get<DRotation>();
    r.sfx = getVec<uint8_t>(in);
    r.links = getVec<uint32_t>(in);
    r.client = in.get<uint8_t>() != 0;
    r.clientName = getStr(in);
    r.clientFlags2 = in.get<DDWORD>();
    r.gameData = getVec<uint8_t>(in);
    return r;
}

DRESULT saveObjects(char *fileName, ObjectList *list, DDWORD param, DDWORD flags)
{
    Server *s = Server::instance;
    if (!s || !fileName || !list) return LT_ERROR;
    std::vector<EngineObject *> objs;
    for (ObjectLink *l = list->m_pFirstLink; l; l = l->m_pNext)
        if (EngineObject *o = toObject(l->m_hObject); o && !o->removed && o->game && o != s->worldObject)
            objs.push_back(o);  // the world itself comes with the world

    Message out;
    out.put("SHSV", 4);
    out.put<uint32_t>(kVersion);
    out.put<uint32_t>(param);
    out.put<double>(s->time);
    putStr(out, s->worldName);
    out.put<uint32_t>(s->worldObject ? s->worldObject->id : 0);
    out.put<uint32_t>((uint32_t)objs.size());
    for (EngineObject *o : objs) {
        Record r;
        r.className = o->cls && o->cls->m_ClassName ? o->cls->m_ClassName : "";
        r.id = o->id;
        r.type = o->type;
        r.flags = o->flags;
        r.userFlags = o->userFlags;
        r.clientFlags = o->clientFlags;
        r.pos = o->pos;
        r.rot = o->rot;
        r.scale = o->scale;
        r.dims = o->dims;
        r.velocity = o->velocity;
        r.accel = o->accel;
        memcpy(r.color, o->color, sizeof(r.color));
        r.mass = o->mass;
        r.friction = o->friction;
        r.forceIgnoreLimit = o->forceIgnoreLimit;
        memcpy(r.lightColor, o->lightColor, sizeof(r.lightColor));
        r.lightRadius = o->lightRadius;
        r.filename = o->filename;
        r.skin = o->skin;
        r.name = o->name;
        r.userData = o->userData;
        r.containerCode = o->containerCode;
        r.blockingPriority = o->blockingPriority;
        r.active = o->active;
        r.deactivationTime = o->deactivationTime;
        r.state = o->state;
        r.nextUpdateIn = o->nextUpdate > 0 ? std::max(o->nextUpdate - s->time, 0.0001) : 0;
        r.anim = o->anim;
        r.animTime = o->animTime;
        r.animLoop = o->animLoop;
        r.animDone = o->animDone;
        for (bool h : o->hiddenNodes) r.hiddenNodes.push_back(h);
        r.bsp = o->bsp;
        r.basePos = o->basePos;
        r.baseRot = o->baseRot;
        r.sfx = o->sfx;
        for (EngineObject *l : o->links)
            if (l && !l->removed) r.links.push_back(l->id);
        if (o == s->client.object) {
            r.client = true;
            r.clientName = s->client.name;
            r.clientFlags2 = s->client.infoFlags | CIF_LOCAL;
        }
        Message state;
        s->engineMessage(o, MID_SAVEOBJECT, toWriteHandle(&state), (float)param);
        state.magic = 0;
        r.gameData = std::move(state.data);
        writeRecord(out, r);
    }
    // The game console: per-level settings the game keeps in console variables.
    std::vector<std::pair<std::string, std::string>> vars;
    if (flags & SAVEOBJECTS_SAVEGAMECONSOLE)
        for (const std::string &name : s->gameVarNames)
            if (ConsoleVars::Var *v = s->vars->find(name)) vars.push_back({v->name, v->value});
    out.put<uint32_t>((uint32_t)vars.size());
    for (auto &[name, value] : vars) {
        putStr(out, name);
        putStr(out, value);
    }
    // Attachments between saved objects (after the console, so saves from
    // before they were kept still load).
    std::vector<AttachmentRecord> attachments;
    for (const AttachmentRecord &a : attachmentRecords()) {
        auto saved = [&](uint32_t id) {
            return std::any_of(objs.begin(), objs.end(), [id](EngineObject *o) { return o->id == id; });
        };
        if (saved(a.parent) && saved(a.child)) attachments.push_back(a);
    }
    out.put<uint32_t>((uint32_t)attachments.size());
    for (const AttachmentRecord &a : attachments) {
        out.put(a.parent);
        out.put(a.child);
        out.put(a.node);
        out.put(a.offset);
        out.put(a.rotOffset);
    }
    out.magic = 0;

    std::string path = savePath(fileName);
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[save] can't write %s\n", path.c_str());
        return LT_ERROR;
    }
    bool ok = fwrite(out.data.data(), 1, out.data.size(), f) == out.data.size();
    ok = fclose(f) == 0 && ok;
    if (tracing()) printf("[save] %s: %zu object(s), %zu bytes\n", path.c_str(), objs.size(), out.data.size());
    return ok ? LT_OK : LT_ERROR;
}

DRESULT restoreObjects(char *fileName, DDWORD param, DDWORD flags)
{
    Server *s = Server::instance;
    if (!s || !fileName) return LT_ERROR;
    std::string path = savePath(fileName);
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[save] can't read %s\n", path.c_str());
        return LT_ERROR;
    }
    Message in;
    for (uint8_t buf[65536];;) {
        size_t n = fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        in.put(buf, n);
    }
    fclose(f);
    char tag[4] = {};
    in.get(tag, 4);
    uint32_t version = in.get<uint32_t>();
    if (memcmp(tag, "SHSV", 4) != 0 || version != kVersion) {
        fprintf(stderr, "[save] %s: not a save of this version\n", path.c_str());
        in.magic = 0;
        return LT_ERROR;
    }
    in.get<uint32_t>();  // param it was saved with
    double savedTime = in.get<double>();
    std::string savedWorld = getStr(in);
    uint32_t worldObjectId = in.get<uint32_t>();
    uint32_t count = in.get<uint32_t>();
    std::vector<Record> records;
    for (uint32_t i = 0; i < count && in.pos < in.data.size(); ++i) records.push_back(readRecord(in));
    std::vector<std::pair<std::string, std::string>> vars;
    for (uint32_t i = 0, n = in.get<uint32_t>(); i < n && in.pos < in.data.size(); ++i) {
        std::string name = getStr(in);
        vars.push_back({name, getStr(in)});
    }
    std::vector<AttachmentRecord> attachments;
    if (in.pos < in.data.size())
        for (uint32_t i = 0, n = in.get<uint32_t>(); i < n && in.pos < in.data.size(); ++i) {
            AttachmentRecord a;
            a.parent = in.get<uint32_t>();
            a.child = in.get<uint32_t>();
            a.node = in.get<int32_t>();
            a.offset = in.get<DVector>();
            a.rotOffset = in.get<DRotation>();
            attachments.push_back(a);
        }
    in.magic = 0;
    if (records.size() != count) {
        fprintf(stderr, "[save] %s: truncated\n", path.c_str());
        return LT_ERROR;
    }

    if (flags & RESTOREOBJECTS_RESTORETIME) {
        // Game data holds absolute times; continue the clock where it was.
        double shift = savedTime - s->time;
        for (EngineObject *o : objects().all())
            if (!o->clientOnly && o->nextUpdate > 0) o->nextUpdate += shift;
        s->time = savedTime;
    }
    for (auto &[name, value] : vars) {
        s->vars->set(name, value);
        s->gameVarNames.insert(name);
    }
    const bool sameWorld = strcasecmp(savedWorld.c_str(), s->worldName.c_str()) == 0;

    // 1. Create everything.
    std::unordered_map<uint32_t, EngineObject *> byId;
    if (worldObjectId && s->worldObject) byId[worldObjectId] = s->worldObject;
    std::vector<std::pair<const Record *, EngineObject *>> made;
    for (const Record &r : records) {
        ClassDef *cls = s->findClass(r.className.c_str());
        if (!cls) {
            fprintf(stderr, "[save] unknown class %s\n", r.className.c_str());
            continue;
        }
        ObjectCreateStruct ocs;
        memset(&ocs, 0, sizeof(ocs));
        ocs.m_ObjectType = (unsigned short)r.type;
        ocs.m_Flags = r.flags;
        ocs.m_Pos = r.pos;
        ocs.m_Scale = r.scale;
        ocs.m_Rotation = r.rot;
        ocs.m_ContainerCode = r.containerCode;
        ocs.m_UserData = (uintptr_t)r.userData;
        snprintf(ocs.m_Filename, sizeof(ocs.m_Filename), "%s", r.filename.c_str());
        snprintf(ocs.m_SkinName, sizeof(ocs.m_SkinName), "%s", r.skin.c_str());
        snprintf(ocs.m_Name, sizeof(ocs.m_Name), "%s", r.name.c_str());
        ocs.m_NextUpdate = (float)r.nextUpdateIn;
        ocs.m_fDeactivationTime = r.deactivationTime;
        EngineObject *o = s->createObject(cls, &ocs, PRECREATE_SAVEGAME, nullptr, true);
        if (!o) {
            fprintf(stderr, "[save] %s declined to be restored\n", r.className.c_str());
            continue;
        }
        // The saved state wins over whatever MID_PRECREATE set up.
        o->type = r.type;
        o->flags = r.flags;
        o->userFlags = r.userFlags;
        o->clientFlags = r.clientFlags;
        o->pos = r.pos;
        o->rot = r.rot;
        o->scale = r.scale;
        o->dims = r.dims;
        o->velocity = r.velocity;
        o->accel = r.accel;
        memcpy(o->color, r.color, sizeof(r.color));
        o->mass = r.mass;
        o->friction = r.friction;
        o->forceIgnoreLimit = r.forceIgnoreLimit;
        memcpy(o->lightColor, r.lightColor, sizeof(r.lightColor));
        o->lightRadius = r.lightRadius;
        o->filename = r.filename;
        o->skin = r.skin;
        o->name = r.name;
        o->blockingPriority = r.blockingPriority;
        o->active = r.active;
        o->state = r.state;
        o->nextUpdate = r.nextUpdateIn > 0 ? s->time + r.nextUpdateIn : 0;
        o->anim = r.anim;
        o->animTime = r.animTime;
        o->animLoop = r.animLoop;
        o->animDone = r.animDone;
        o->hiddenNodes.assign(r.hiddenNodes.begin(), r.hiddenNodes.end());
        if (sameWorld && r.bsp >= 0) {
            o->bsp = r.bsp;
            o->basePos = r.basePos;
            o->baseRot = r.baseRot;
        }
        if (!r.sfx.empty()) {
            o->sfx = r.sfx;
            o->sfxSent = false;
        }
        byId[r.id] = o;
        made.push_back({&r, o});
    }

    // 2. Their data, with object references mapped to the new objects.
    setLoadObjectMap(&byId);
    for (auto &[r, o] : made) {
        if (o->removed) continue;
        Message state;
        state.data = r->gameData;
        state.emptyHStringIsNull = true;
        s->engineMessage(o, MID_LOADOBJECT, toReadHandle(&state), (float)param);
        state.magic = 0;
    }
    setLoadObjectMap(nullptr);

    // 3. Links and client references.
    for (auto &[r, o] : made) {
        for (uint32_t id : r->links) {
            auto it = byId.find(id);
            if (it != byId.end() && std::find(o->links.begin(), o->links.end(), it->second) == o->links.end())
                o->links.push_back(it->second);
        }
        if (r->client) {
            auto ref = std::make_unique<ClientRef>();
            ref->infoFlags = r->clientFlags2;
            ref->name = r->clientName;
            ref->object = o;
            g_clientRefs.push_back(std::move(ref));
        }
    }

    // Attachments (the characters' hand-held weapons).
    for (const AttachmentRecord &a : attachments) {
        auto p = byId.find(a.parent), c = byId.find(a.child);
        if (p != byId.end() && c != byId.end() && !p->second->removed && !c->second->removed)
            restoreAttachment(p->second, c->second, a);
    }

    // 4. Initial updates.
    for (auto &[r, o] : made)
        if (o->magic == EngineObject::kMagic && !o->removed) s->engineMessage(o, MID_INITIALUPDATE, nullptr, INITIALUPDATE_SAVEGAME);

    if (tracing())
        printf("[save] restored %zu of %zu object(s) from %s (%s world)\n", made.size(), records.size(), path.c_str(),
               sameWorld ? "same" : "another");
    return LT_OK;
}

ClientRef *R(HCLIENTREF h)
{
    auto *r = reinterpret_cast<ClientRef *>(h);
    for (const auto &p : g_clientRefs)
        if (p.get() == r) return r;
    return nullptr;
}
HCLIENTREF getNextClientRef(HCLIENTREF prev)
{
    if (!prev) return g_clientRefs.empty() ? nullptr : reinterpret_cast<HCLIENTREF>(g_clientRefs.front().get());
    for (size_t i = 0; i + 1 < g_clientRefs.size(); ++i)
        if (reinterpret_cast<HCLIENTREF>(g_clientRefs[i].get()) == prev)
            return reinterpret_cast<HCLIENTREF>(g_clientRefs[i + 1].get());
    return nullptr;
}
DDWORD getClientRefInfoFlags(HCLIENTREF h)
{
    ClientRef *r = R(h);
    return r ? r->infoFlags : 0;
}
DBOOL getClientRefName(HCLIENTREF h, char *name, int maxLen)
{
    ClientRef *r = R(h);
    if (!r || !name || maxLen <= 0) return DFALSE;
    snprintf(name, (size_t)maxLen, "%s", r->name.c_str());
    return DTRUE;
}
HOBJECT getClientRefObject(HCLIENTREF h)
{
    ClientRef *r = R(h);
    return r && r->object ? toHObject(r->object) : nullptr;
}

}  // namespace

std::string savePath(const char *name)
{
    std::string p = name;
    for (char &c : p)
        if (c == '\\') c = '/';
    for (size_t i = p.find('/'); i != std::string::npos; i = p.find('/', i + 1))
        if (i > 0) mkdir(p.substr(0, i).c_str(), 0755);
    return p;
}

void clearClientRefs() { g_clientRefs.clear(); }

void saveObjectRemoved(EngineObject *o)
{
    for (const auto &r : g_clientRefs)
        if (r->object == o) r->object = nullptr;
}

void installSaveAPI(ServerDE *d)
{
    d->SaveObjects = saveObjects;
    d->RestoreObjects = restoreObjects;
    d->GetNextClientRef = getNextClientRef;
    d->GetClientRefInfoFlags = getClientRefInfoFlags;
    d->GetClientRefName = getClientRefName;
    d->GetClientRefObject = getClientRefObject;
}

}  // namespace shogo
