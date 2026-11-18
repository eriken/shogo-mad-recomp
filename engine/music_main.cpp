// shogo-music: renders a soundtrack's sections to a WAV file, as the game
// would play them in a list (run from the game folder).
//   shogo-music Music\RedRiot redriot.dls sta.sec a1.sec a2.sec --out /tmp/a.wav
#include "engine/music.h"
#include "engine/sound.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    std::vector<const char *> args;
    const char *out = "music.wav";
    double extra = 2.0;  // seconds after the last section, for the notes to ring out
    bool info = false;    // list the instruments and each section's style and pattern
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--tail") && i + 1 < argc) extra = atof(argv[++i]);
        else if (!strcmp(argv[i], "--info")) info = true;
        else args.push_back(argv[i]);
    }
    if (args.size() < 3) {
        fprintf(stderr, "usage: %s <music dir> <instruments.dls> <section.sec ...> [--out file.wav] [--tail seconds]\n", argv[0]);
        return 2;
    }
    shogo::MusicPlayer &m = shogo::music();
    if (!m.setDirectory(args[0])) {
        fprintf(stderr, "no music directory %s\n", args[0]);
        return 1;
    }
    if (!m.initInstruments(args[1], nullptr)) return 1;
    if (info) {
        for (const shogo::DlsInstrument &ins : m.instruments()->instruments) {
            printf("program %u %-20s", ins.program, ins.name.c_str());
            for (const shogo::DlsRegion &r : ins.regions)
                printf(" [%d-%d wave %d unity %d%s]", r.keyLo, r.keyHi, r.wave, r.unityNote, r.looped ? " loop" : "");
            printf("\n");
        }
        for (size_t i = 2; i < args.size(); ++i)
            if (const shogo::ImaSection *sec = m.findSection(args[i])) {
                printf("%s: style %s, groove %x, %d measures, %d bpm\n", args[i], sec->style.c_str(), sec->groove,
                       sec->measures, sec->tempo);
                if (const shogo::ImaStyle *sty = m.findStyle(sec->style))
                    for (const shogo::ImaPattern &p : sty->patterns) {
                        printf("  pattern %-12s grooves %4x, %u clocks, %zu notes, variations", p.name.c_str(), p.grooves,
                               p.length, p.notes.size());
                        for (uint16_t v : p.partVariations) printf(" %x", v);
                        printf("\n");
                    }
            }
    }
    for (size_t i = 2; i < args.size(); ++i)
        if (!m.addToPlayList("list", args[i])) fprintf(stderr, "can't load %s\n", args[i]);
    if (!m.playList("list", nullptr, false, MUSIC_IMMEDIATE)) return 1;

    std::vector<int16_t> pcm;
    const int block = 1024;
    std::vector<float> buf(block * 2);
    int quietBlocks = 0;
    const int tailBlocks = int(extra * shogo::DlsSynth::kRate / block);
    std::string last;
    while (quietBlocks < tailBlocks) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        m.render(buf.data(), block);
        if (m.currentSection() != last && !m.currentSection().empty()) {
            last = m.currentSection();
            printf("%7.2fs %s\n", double(pcm.size() / 2) / shogo::DlsSynth::kRate, last.c_str());
        }
        if (m.currentSection().empty()) ++quietBlocks;
        for (float f : buf) {
            float x = f > 1 ? 1 : f < -1 ? -1 : f;
            pcm.push_back(int16_t(x * 32767));
        }
    }
    printf("%.1f s, %zu voices still sounding\n", double(pcm.size() / 2) / shogo::DlsSynth::kRate, m.synth().activeVoices());
    return shogo::writeWav(out, pcm, shogo::DlsSynth::kRate) ? 0 : 1;
}
