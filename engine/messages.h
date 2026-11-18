// HMESSAGEWRITE / HMESSAGEREAD: the typed byte-stream messages the game sends
// between client, server and objects. Client and server run in one process, so
// messages are never serialized for a network; they only need to read back
// what was written. Object references travel as object ids.
#ifndef SHOGO_ENGINE_MESSAGES_H
#define SHOGO_ENGINE_MESSAGES_H

#include "engine/api.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace shogo {

class EngineStrings;
struct EngineObject;

struct Message {
    static constexpr uint32_t kMagic = 0x4D534731;  // "MSG1"
    uint32_t magic = kMagic;
    std::vector<uint8_t> data;
    size_t pos = 0;
    // Routing for messages started with StartMessage* (filled by whoever starts it).
    int kind = 0;
    uint32_t id = 0;
    void *target = nullptr;
    std::vector<std::string> strings;  // storage for ReadFromMessageString results

    void put(const void *p, size_t n) { data.insert(data.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
    bool get(void *p, size_t n)
    {
        if (pos + n > data.size()) {
            memset(p, 0, n);
            pos = data.size();
            return false;
        }
        memcpy(p, &data[pos], n);
        pos += n;
        return true;
    }
    template <class T> void put(const T &v) { put(&v, sizeof(T)); }
    template <class T> T get()
    {
        T v;
        get(&v, sizeof(T));
        return v;
    }
};

inline Message *toMessage(HMESSAGEWRITE h)
{
    auto *m = reinterpret_cast<Message *>(h);
    return (m && m->magic == Message::kMagic) ? m : nullptr;
}
inline HMESSAGEWRITE toWriteHandle(Message *m) { return reinterpret_cast<HMESSAGEWRITE>(m); }
inline HMESSAGEREAD toReadHandle(Message *m) { return reinterpret_cast<HMESSAGEREAD>(m); }

// Installs the CSBase message methods for both engine sides. The string tables
// are used for HSTRING reads on the matching side.
void installMessageAPI(void *clientEngine, EngineStrings *clientStrings, void *serverEngine,
                       EngineStrings *serverStrings);

// Object ids <-> objects, provided by the object system.
uint32_t objectId(EngineObject *obj);
EngineObject *objectFromId(uint32_t id);
// While set (RestoreObjects), ReadFromLoadSaveMessageObject maps saved ids through it.
void setLoadObjectMap(const std::unordered_map<uint32_t, EngineObject *> *map);

}  // namespace shogo

#endif
