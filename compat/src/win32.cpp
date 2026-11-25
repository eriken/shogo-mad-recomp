// Native implementations of the Win32 calls declared in shogo_win32.h.
#include "shogo_win32.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The player's own files, relative to the game folder: Save/... and autoexec.cfg.
static bool isPlayerPath(const char *p)
{
    if (p[0] == '.' && p[1] == '/') p += 2;
    if (strcasecmp(p, "autoexec.cfg") == 0) return true;
    return strncasecmp(p, "save", 4) == 0 && (p[4] == '\0' || p[4] == '/');
}

extern "C" void shogo_native_path(const char *in, char *out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!in) return;
    if (isalpha((unsigned char)in[0]) && in[1] == ':') in += 2;  // drop "c:"
    size_t n = 0;
    // The player's files are kept apart from the game data, in the folder
    // SHOGO_PLAYER_DIR names (set by the app; unset, they're in the game folder).
    char rel[1024];
    for (; *in && n + 1 < sizeof rel; ++in) rel[n++] = (*in == '\\') ? '/' : *in;
    rel[n] = '\0';
    const char *player = getenv("SHOGO_PLAYER_DIR");
    if (player && *player && rel[0] != '/' && isPlayerPath(rel))
        snprintf(out, outSize, "%s/%s", player, rel[0] == '.' && rel[1] == '/' ? rel + 2 : rel);
    else
        snprintf(out, outSize, "%s", rel);
}

namespace {

struct IniLine {
    std::string text;     // raw line, used for comments/blank lines and section headers
    std::string section;  // section this line belongs to
    std::string key;      // empty for non key=value lines
    std::string value;
};

std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool iequals(const std::string &a, const std::string &b)
{
    return strcasecmp(a.c_str(), b.c_str()) == 0;
}

std::vector<IniLine> readIni(const std::string &path)
{
    std::vector<IniLine> lines;
    std::ifstream f(path);
    std::string raw, section;
    while (std::getline(f, raw)) {
        IniLine l;
        l.text = raw;
        std::string t = trim(raw);
        if (!t.empty() && t[0] == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
            l.section = section;
        } else {
            l.section = section;
            size_t eq = t.find('=');
            if (eq != std::string::npos && t[0] != ';') {
                l.key = trim(t.substr(0, eq));
                l.value = trim(t.substr(eq + 1));
            }
        }
        lines.push_back(l);
    }
    return lines;
}

std::string nativePath(const char *p)
{
    char buf[1024];
    shogo_native_path(p, buf, sizeof(buf));
    return buf;
}

}  // namespace

extern "C" DWORD GetPrivateProfileString(const char *app, const char *key, const char *def,
                                         char *out, DWORD size, const char *file)
{
    if (!out || size == 0) return 0;
    std::string result = def ? def : "";
    if (app && key && file) {
        for (const IniLine &l : readIni(nativePath(file))) {
            if (!l.key.empty() && iequals(l.section, app) && iequals(l.key, key)) {
                result = l.value;
                break;
            }
        }
    }
    size_t n = result.size() < size - 1 ? result.size() : size - 1;
    memcpy(out, result.data(), n);
    out[n] = '\0';
    return (DWORD)n;
}

extern "C" BOOL WritePrivateProfileString(const char *app, const char *key, const char *value,
                                          const char *file)
{
    if (!app || !file) return FALSE;
    std::string path = nativePath(file);
    std::vector<IniLine> lines = readIni(path);

    bool sectionFound = false, done = false;
    size_t insertAt = lines.size();
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        IniLine &l = lines[i];
        if (!iequals(l.section, app)) continue;
        sectionFound = true;
        insertAt = i + 1;
        if (key && !l.key.empty() && iequals(l.key, key)) {
            if (value) {
                l.value = value;
                l.text = l.key + "=" + l.value;
            } else {
                lines.erase(lines.begin() + (long)i);
            }
            done = true;
        }
    }
    if (!done && key && value) {
        IniLine l;
        l.section = app;
        l.key = key;
        l.value = value;
        l.text = std::string(key) + "=" + value;
        if (!sectionFound) {
            IniLine header;
            header.section = app;
            header.text = std::string("[") + app + "]";
            lines.push_back(header);
            lines.push_back(l);
        } else {
            lines.insert(lines.begin() + (long)insertAt, l);
        }
    }

    std::ofstream f(path, std::ios::trunc);
    if (!f) return FALSE;
    for (const IniLine &l : lines) f << l.text << "\r\n";
    return TRUE;
}

extern "C" DWORD GetTickCount(void)
{
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return (DWORD)duration_cast<milliseconds>(steady_clock::now() - start).count();
}

extern "C" void OutputDebugString(const char *str)
{
    if (str) fputs(str, stderr);
}

extern "C" SHORT GetKeyState(int)
{
    return 0;
}
