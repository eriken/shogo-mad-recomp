// Reads the resource section of a Windows PE file (the game's CRES.DLL / SRES.DLL)
// as plain data. Nothing in the DLL is executed.
#ifndef SHOGO_ENGINE_PE_RESOURCES_H
#define SHOGO_ENGINE_PE_RESOURCES_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shogo {

class PEResources {
public:
    // Parses the DLL image. Returns nullptr (with a message in *error) on failure.
    static std::unique_ptr<PEResources> parse(std::vector<uint8_t> image, std::string *error = nullptr);

    // String-table (RT_STRING) entry, converted from UTF-16 to Windows-1252 bytes,
    // which is what the 1998 game code expects. nullopt if the ID doesn't exist.
    std::optional<std::string> string(uint32_t id) const;
    size_t stringCount() const { return m_strings.size(); }

    // Raw bytes of a resource with a named type and name, e.g. ("TEXT", "CREDITS").
    std::optional<std::vector<uint8_t>> named(const std::string &type, const std::string &name) const;

private:
    PEResources() = default;
    std::map<uint32_t, std::string> m_strings;
    std::map<std::string, std::vector<uint8_t>> m_named;  // key "TYPE/NAME", upper case
};

// UTF-16 code unit to a Windows-1252 byte ('?' if not representable).
char utf16ToCp1252(uint16_t c);

}  // namespace shogo

#endif
