#include "engine/messages.h"

#include "engine/strings.h"
#include "engine/stub_support.h"

#include <cstdarg>

namespace shogo {

namespace {
void *g_clientEngine = nullptr, *g_serverEngine = nullptr;
EngineStrings *g_clientStrings = nullptr, *g_serverStrings = nullptr;

EngineStrings *stringsFor(void *self) { return self == g_serverEngine ? g_serverStrings : g_clientStrings; }

Message *W(HMESSAGEWRITE h) { return toMessage(h); }
Message *R(HMESSAGEREAD h) { return toMessage(h); }

template <class T> DRESULT writeVal(HMESSAGEWRITE h, T v)
{
    Message *m = W(h);
    if (!m) return LT_ERROR;
    m->put(v);
    return LT_OK;
}

HMESSAGEWRITE startHMessageWrite(void *) { return toWriteHandle(new Message); }
DRESULT writeFloat(void *, HMESSAGEWRITE h, float v) { return writeVal(h, v); }
DRESULT writeByte(void *, HMESSAGEWRITE h, DBYTE v) { return writeVal(h, v); }
DRESULT writeWord(void *, HMESSAGEWRITE h, D_WORD v) { return writeVal(h, v); }
DRESULT writeDWord(void *, HMESSAGEWRITE h, DDWORD v) { return writeVal(h, v); }
DRESULT writeString(void *, HMESSAGEWRITE h, char *s)
{
    Message *m = W(h);
    if (!m) return LT_ERROR;
    uint32_t n = s ? (uint32_t)strlen(s) : 0;
    m->put(n);
    if (n) m->put(s, n);
    return LT_OK;
}
DRESULT writeVector(void *, HMESSAGEWRITE h, DVector *v) { return writeVal(h, v ? *v : DVector()); }
DRESULT writeRotation(void *, HMESSAGEWRITE h, DRotation *r) { return writeVal(h, r ? *r : DRotation(0, 0, 0, 1)); }
DRESULT writeHString(void *self, HMESSAGEWRITE h, HSTRING s)
{
    EngineStrings *st = stringsFor(self);
    char *text = (st && s) ? st->data(s) : nullptr;
    return writeString(self, h, text ? text : (char *)"");
}
DRESULT writeHMessageWrite(void *, HMESSAGEWRITE h, HMESSAGEWRITE inner)
{
    Message *m = W(h), *in = W(inner);
    if (!m) return LT_ERROR;
    uint32_t n = in ? (uint32_t)in->data.size() : 0;
    m->put(n);
    if (n) m->put(in->data.data(), n);
    return LT_OK;
}
DRESULT writeHMessageRead(void *, HMESSAGEWRITE h, HMESSAGEREAD inner)
{
    Message *m = W(h), *in = R(inner);
    if (!m) return LT_ERROR;
    // The unread remainder of the read message.
    uint32_t n = in ? (uint32_t)(in->data.size() - std::min(in->pos, in->data.size())) : 0;
    m->put(n);
    if (n) m->put(in->data.data() + in->pos, n);
    return LT_OK;
}
DRESULT writeObject(void *, HMESSAGEWRITE h, HOBJECT o) { return writeVal(h, objectId(reinterpret_cast<EngineObject *>(o))); }

float readFloat(void *, HMESSAGEREAD h) { Message *m = R(h); return m ? m->get<float>() : 0.0f; }
DBYTE readByte(void *, HMESSAGEREAD h) { Message *m = R(h); return m ? m->get<DBYTE>() : 0; }
D_WORD readWord(void *, HMESSAGEREAD h) { Message *m = R(h); return m ? m->get<D_WORD>() : 0; }
DDWORD readDWord(void *, HMESSAGEREAD h) { Message *m = R(h); return m ? m->get<DDWORD>() : 0; }
char *readString(void *, HMESSAGEREAD h)
{
    static char empty[1] = {0};
    Message *m = R(h);
    if (!m) return empty;
    if (m->rereadable && m->pos >= m->data.size()) m->pos = 0;
    uint32_t n = m->get<uint32_t>();
    std::string s(std::min<size_t>(n, m->data.size() - m->pos), '\0');
    m->get(s.data(), s.size());
    m->strings.push_back(std::move(s));
    return m->strings.back().data();
}
void readVector(void *, HMESSAGEREAD h, DVector *v)
{
    Message *m = R(h);
    DVector r = m ? m->get<DVector>() : DVector();
    if (v) *v = r;
}
void readRotation(void *, HMESSAGEREAD h, DRotation *v)
{
    Message *m = R(h);
    DRotation r = m ? m->get<DRotation>() : DRotation(0, 0, 0, 1);
    if (v) *v = r;
}
HOBJECT readObject(void *, HMESSAGEREAD h)
{
    Message *m = R(h);
    return m ? reinterpret_cast<HOBJECT>(objectFromId(m->get<uint32_t>())) : nullptr;
}
HSTRING readHString(void *self, HMESSAGEREAD h)
{
    char *s = readString(self, h);
    if (Message *m = R(h); m && m->emptyHStringIsNull && !s[0]) return nullptr;
    EngineStrings *st = stringsFor(self);
    return st ? st->create(s) : nullptr;
}
HMESSAGEREAD readHMessageRead(void *, HMESSAGEREAD h)
{
    Message *m = R(h);
    if (!m) return nullptr;
    uint32_t n = m->get<uint32_t>();
    auto *inner = new Message;
    inner->emptyHStringIsNull = m->emptyHStringIsNull;
    n = (uint32_t)std::min<size_t>(n, m->data.size() - m->pos);
    inner->data.assign(m->data.begin() + m->pos, m->data.begin() + m->pos + n);
    m->pos += n;
    return toReadHandle(inner);
}
void endHMessageRead(void *, HMESSAGEREAD h)
{
    if (Message *m = R(h)) {
        m->magic = 0;
        delete m;
    }
}
void endHMessageWrite(void *, HMESSAGEWRITE h)
{
    if (Message *m = W(h)) {
        m->magic = 0;
        delete m;
    }
}
void resetRead(void *, HMESSAGEREAD h)
{
    if (Message *m = R(h)) m->pos = 0;
}
// Save-game object references are plain object references here.
DRESULT writeLoadSaveObject(void *s, HMESSAGEWRITE h, HOBJECT o) { return writeObject(s, h, o); }
const std::unordered_map<uint32_t, EngineObject *> *g_loadMap = nullptr;
DRESULT readLoadSaveObject(void *s, HMESSAGEREAD h, HOBJECT *o)
{
    HOBJECT r = nullptr;
    if (g_loadMap) {
        Message *m = R(h);
        uint32_t id = m ? m->get<uint32_t>() : 0;
        auto it = g_loadMap->find(id);
        r = it != g_loadMap->end() ? reinterpret_cast<HOBJECT>(it->second) : nullptr;
    } else {
        r = readObject(s, h);
    }
    if (o) *o = r;
    return LT_OK;
}

#define HOOK(name, fn) shogo_set_virtual_hook("CSBase::" name, (void *)&fn)
}  // namespace

void setLoadObjectMap(const std::unordered_map<uint32_t, EngineObject *> *map) { g_loadMap = map; }

void installMessageAPI(void *clientEngine, EngineStrings *clientStrings, void *serverEngine,
                       EngineStrings *serverStrings)
{
    g_clientEngine = clientEngine;
    g_serverEngine = serverEngine;
    g_clientStrings = clientStrings;
    g_serverStrings = serverStrings;
    HOOK("StartHMessageWrite", startHMessageWrite);
    HOOK("WriteToMessageFloat", writeFloat);
    HOOK("WriteToMessageByte", writeByte);
    HOOK("WriteToMessageWord", writeWord);
    HOOK("WriteToMessageDWord", writeDWord);
    HOOK("WriteToMessageString", writeString);
    HOOK("WriteToMessageVector", writeVector);
    HOOK("WriteToMessageCompVector", writeVector);
    HOOK("WriteToMessageCompPosition", writeVector);
    HOOK("WriteToMessageRotation", writeRotation);
    HOOK("WriteToMessageHString", writeHString);
    HOOK("WriteToMessageHMessageWrite", writeHMessageWrite);
    HOOK("WriteToMessageHMessageRead", writeHMessageRead);
    HOOK("WriteToMessageObject", writeObject);
    HOOK("WriteToLoadSaveMessageObject", writeLoadSaveObject);
    HOOK("ReadFromMessageFloat", readFloat);
    HOOK("ReadFromMessageByte", readByte);
    HOOK("ReadFromMessageWord", readWord);
    HOOK("ReadFromMessageDWord", readDWord);
    HOOK("ReadFromMessageString", readString);
    HOOK("ReadFromMessageVector", readVector);
    HOOK("ReadFromMessageCompVector", readVector);
    HOOK("ReadFromMessageCompPosition", readVector);
    HOOK("ReadFromMessageRotation", readRotation);
    HOOK("ReadFromMessageObject", readObject);
    HOOK("ReadFromMessageHString", readHString);
    HOOK("ReadFromLoadSaveMessageObject", readLoadSaveObject);
    HOOK("ReadFromMessageHMessageRead", readHMessageRead);
    HOOK("EndHMessageRead", endHMessageRead);
    HOOK("EndHMessageWrite", endHMessageWrite);
    HOOK("ResetRead", resetRead);
}

}  // namespace shogo
