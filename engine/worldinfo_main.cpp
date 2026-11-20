// shogo-worldinfo: parses .DAT worlds and DTX textures from the game data and reports on them.
// Usage (from the game folder): shogo-worldinfo [WORLDS/01_AMBUSH.DAT ...]   (no args = every world)
#include "engine/dtx.h"
#include "engine/rez_file.h"
#include "engine/world_dat.h"

#include <cstdio>
#include <cstdlib>
#include <map>

using namespace shogo;

int main(int argc, char **argv)
{
    ResourceFS fs;
    std::string err;
    if (!fs.addArchive("SHOGO.REZ", &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::vector<std::string> names;
    for (int i = 1; i < argc; ++i) names.push_back(argv[i]);
    if (names.empty()) names = fs.list("WORLDS", ".DAT");
    int failures = 0;
    for (const auto &name : names) {
        auto data = fs.read(name);
        if (!data) { printf("%-40s not found\n", name.c_str()); ++failures; continue; }
        auto w = WorldData::parse(*data, &err);
        if (!w) { printf("%-40s FAILED: %s\n", name.c_str(), err.c_str()); ++failures; continue; }
        size_t polys = 0, tex = 0, badTex = 0;
        for (const auto &m : w->models) {
            polys += m.polys.size();
            for (const auto &t : m.textures) {
                ++tex;
                auto td = fs.read(t);
                std::string why = "missing";
                if (!td || !decodeDTX(*td, &why)) {
                    ++badTex;
                    if (getenv("SHOGO_VERBOSE")) printf("   unreadable %s: %s\n", t.c_str(), why.c_str());
                }
            }
        }
        printf("%-40s %4zu objects %3zu models %6zu polys (main %5zu)  %4zu texture refs, %zu unreadable\n",
               name.c_str(), w->objects.size(), w->models.size(), polys, w->models[0].polys.size(), tex, badTex);
        if (argc > 1 && getenv("SHOGO_WORLDINFO_OBJECTS")) {
            // Every object: class, then its string, vector and number properties.
            for (const auto &o : w->objects) {
                printf("   object %s", o.className.c_str());
                for (const auto &p : o.props) {
                    if (p.type == WorldProp::String && !p.string.empty())
                        printf(" %s=\"%s\"", p.name.c_str(), p.string.c_str());
                    else if (p.type == WorldProp::Vector || p.type == WorldProp::Color)
                        printf(" %s=(%g %g %g)", p.name.c_str(), p.vec[0], p.vec[1], p.vec[2]);
                    else if (p.type == WorldProp::Real || p.type == WorldProp::LongInt)
                        printf(" %s=%g", p.name.c_str(), p.vec[0]);
                }
                printf("\n");
            }
        }
        if (argc > 1) {
            std::map<uint32_t, int> flags;
            for (const auto &s : w->models[0].surfaces) flags[s.flags]++;
            for (auto &[f, n] : flags) printf("   surface flags %08x x%d\n", f, n);
            for (const auto &m : w->models) {
                printf("   model %-24s flags %08x %5zu polys\n", m.name.c_str(), m.flags, m.polys.size());
                const bool all = getenv("SHOGO_WORLDINFO_ALL") != nullptr;  // the main model's surfaces too
                if (&m == &w->models[0] && !all) continue;
                // Each surface of the smaller models: flags, texture, effect.
                std::map<std::string, int> seen;
                for (const auto &s : m.surfaces) {
                    char key[512];
                    snprintf(key, sizeof key, "%08x %s%s%s %08x", s.flags,
                             s.texture < m.textures.size() ? m.textures[s.texture].c_str() : "?",
                             s.effect.empty() ? "" : " effect ", s.effect.c_str(), s.unknown2);
                    seen[key]++;
                }
                for (auto &[k, n] : seen) printf("      surface %s x%d\n", k.c_str(), n);
            }
        }
    }
    return failures ? 1 : 0;
}
