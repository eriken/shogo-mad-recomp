// Shogo's music: Microsoft Interactive Music Architecture (IMA) data, played
// with our own sequencer and DLS synth. Formats worked out from the game's
// own files (notes in docs/music-format.md).
//
// Each soundtrack folder (Music\Inform, Music\RedRiot, ...) holds a DLS
// instrument bank, styles (.STY: bands and patterns of notes) and sections
// (.SEC: a style, a groove level, a band, a length in measures). A section
// plays the style's pattern for its groove level, looped over its length;
// each part picks one of up to 16 variations of its notes on every pass.
//
// The game builds playlists of sections per level (silence, ambient,
// cruising, hard driving) and moves between them through transition sections.
#ifndef SHOGO_ENGINE_MUSIC_H
#define SHOGO_ENGINE_MUSIC_H

#include "engine/api.h"
#include "engine/dls.h"

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace shogo {

struct ImaBand {
    uint8_t program[16] = {};  // DLS program per part (0: none)
    uint8_t volume[16] = {};
    uint8_t pan[16] = {};
};

struct ImaNote {
    int32_t time = 0;  // clocks from the pattern's start
    uint32_t duration = 0;
    uint16_t variations = 0;  // bit n: plays in variation n
    uint8_t part = 0, key = 0, velocity = 0;
};

struct ImaPattern {
    std::string name;
    uint32_t length = 0;      // clocks
    uint32_t grooves = 0;     // groove bits it plays at
    int clocksPerBeat = 192;
    int measures = 1;
    std::vector<ImaNote> notes;
    uint16_t partVariations[16] = {};  // variations each part has notes in
};

struct ImaStyle {
    std::string name;
    int tempo = 120;
    int beatsPerMeasure = 4;
    ImaBand band;
    std::vector<ImaPattern> patterns;
    // A pattern for a groove level (any if none matches).
    const ImaPattern *patternFor(uint32_t groove, std::mt19937 &rng) const;
};

struct ImaSection {
    std::string name, style;
    int tempo = 120;
    int measures = 1;
    uint32_t groove = 0;
    bool hasBand = false;
    ImaBand band;
};

std::shared_ptr<ImaStyle> parseImaStyle(const std::vector<uint8_t> &data, std::string *err = nullptr);
std::shared_ptr<ImaSection> parseImaSection(const std::vector<uint8_t> &data, std::string *err = nullptr);

// A DLS sample player: 16 channels, a program, volume and pan each.
class DlsSynth {
public:
    static constexpr int kRate = 44100;

    void setCollection(std::shared_ptr<const DlsCollection> c);
    void program(int ch, int program) { m_channels[ch & 15].program = program; }
    void volume(int ch, int v) { m_channels[ch & 15].volume = v; }
    void pan(int ch, int v) { m_channels[ch & 15].pan = v; }
    void noteOn(int ch, int key, int velocity);
    void noteOff(int ch, int key);
    void allNotesOff();  // release everything
    void silence();      // stop everything at once
    // Adds frames stereo samples (interleaved) to out.
    void render(float *out, int frames);
    size_t activeVoices() const;

private:
    struct Channel {
        int program = 0, volume = 100, pan = 64;
    };
    struct Voice {
        bool active = false, released = false;
        int channel = 0, key = 0;
        const DlsWave *wave = nullptr;
        const DlsRegion *region = nullptr;
        DlsArticulation art;
        double pos = 0, step = 0;
        float gainL = 0, gainR = 0;
        int stage = 0;  // 0 attack, 1 decay, 2 sustain, 3 release
        float env = 0, sustain = 1;
        double age = 0;  // seconds since note on
        uint64_t serial = 0;
    };
    Voice *allocate();

    std::shared_ptr<const DlsCollection> m_dls;
    Channel m_channels[16];
    std::vector<Voice> m_voices = std::vector<Voice>(64);
    uint64_t m_serial = 0;
};

// The client's music functions: playlists of sections, transitions, volume.
class MusicPlayer {
public:
    bool setDirectory(const char *dir);
    bool initInstruments(const char *dlsFile, const char *styleFile);
    bool loadSong(const char *name);
    void destroyAllSongs();
    bool addToPlayList(const char *list, const char *song);
    void deletePlayList(const char *list);
    bool playList(const char *list, const char *transition, bool loop, DDWORD boundary);
    void stop(DDWORD boundary);
    void pause(bool paused) { m_paused = paused; }
    void setVolume(int percent) { m_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
    int volume() const { return m_volume; }

    // Adds frames of music (44.1 kHz stereo, interleaved) to out.
    void render(float *out, int frames);

    const std::string &currentSection() const { return m_currentName; }
    uint64_t samplesPlayed() const { return m_now; }
    DlsSynth &synth() { return m_synth; }
    const DlsCollection *instruments() const { return m_dls.get(); }
    const ImaSection *findSection(const std::string &name) { return section(name); }
    const ImaStyle *findStyle(const std::string &name) { return style(name); }

private:
    struct Event {
        uint64_t at = 0;
        uint8_t type = 0;  // 0 note off, 1 note on, 2 band
        uint8_t channel = 0, key = 0, velocity = 0;
    };
    const ImaSection *section(const std::string &name);
    const ImaStyle *style(const std::string &name);
    std::vector<uint8_t> readFile(const std::string &name) const;
    void startNextSection(uint64_t at);
    void schedule(const ImaSection &sec, uint64_t at);
    void cutAt(uint64_t at);  // drops notes from `at` on, ends held notes there
    uint64_t nextMeasure() const;

    std::string m_dir;
    std::string m_dlsPath;
    std::shared_ptr<DlsCollection> m_dls;
    std::map<std::string, std::shared_ptr<ImaSection>> m_sections;  // by lower-case file name
    std::map<std::string, std::shared_ptr<ImaStyle>> m_styles;      // by lower-case style name
    bool m_stylesScanned = false;
    std::map<std::string, std::vector<std::string>> m_lists;

    // Playback.
    DlsSynth m_synth;
    std::vector<Event> m_events;  // sorted by time
    uint64_t m_now = 0;           // samples
    bool m_playing = false;
    uint64_t m_sectionStart = 0, m_sectionEnd = 0;
    double m_samplesPerMeasure = 0;
    std::string m_currentName;
    std::string m_transition;            // played before the list
    std::vector<std::string> m_playlist;  // then these, in order
    size_t m_listPos = 0;
    bool m_loop = false;
    bool m_stopAtEnd = false;
    bool m_paused = false;
    int m_volume = 100;
    std::mt19937 m_rng{1998};
};

MusicPlayer &music();

// Installs the music functions into the client's function table.
void installMusicAPI(ClientDE *client);

}  // namespace shogo

#endif
