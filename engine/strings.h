// The engine's HSTRING objects and string resources (ClientDE/ServerDE string API).
//
// From the API docs: strings are reference counted and immutable. FormatString and
// CreateString return a string with one reference, CopyString adds one, FreeString
// drops one and deletes the string at zero. FormatString takes a resource string ID
// whose text uses FormatMessage inserts: %1!s!, %2!d!, %3!f!, ...
#ifndef SHOGO_ENGINE_STRINGS_H
#define SHOGO_ENGINE_STRINGS_H

#include "engine/api.h"
#include "engine/pe_resources.h"

#include <cstdarg>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace shogo {

// Expands FormatMessage-style inserts in fmt, pulling arguments from args in
// insert-number order. Supports %1..%99 with an optional !printf-spec! (default !s!),
// and the escapes %% %n %t %0 %. %! and "% ".
std::string formatMessage(const std::string &fmt, va_list args);

class EngineStrings {
public:
    // resources may be null (then FormatString always fails). side is "client"/"server" for messages.
    EngineStrings(std::unique_ptr<PEResources> resources, std::string side);
    ~EngineStrings();

    HSTRING create(const char *text);
    HSTRING format(int messageCode, va_list args);
    HSTRING copy(HSTRING s);
    void free(HSTRING s);
    char *data(HSTRING s) const;
    bool equal(HSTRING a, HSTRING b, bool ignoreCase) const;

    // A NUL-terminated copy of a named "TEXT" resource (credits etc.), owned by this object.
    const char *textResource(const std::string &name);

    size_t liveCount() const { return m_live; }
    const PEResources *resources() const { return m_resources.get(); }

private:
    struct Str;
    std::unique_ptr<PEResources> m_resources;
    std::string m_side;
    size_t m_live = 0;
    std::map<std::string, std::vector<char>> m_texts;
};

// Point a ClientDE / ServerDE string API at an EngineStrings instance.
// (The function tables are plain C function pointers, so one instance per side.)
void installClientStrings(ClientDE *client, EngineStrings *strings);
void installServerStrings(ServerDE *server, EngineStrings *strings);

}  // namespace shogo

#endif
