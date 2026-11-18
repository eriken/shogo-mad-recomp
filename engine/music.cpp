#include "engine/music.h"

#include "engine/movie.h"  // resolveGamePath
#include "engine/riff.h"
#include "engine/sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

namespace shogo {

using namespace riff;

namespace {

std::string lower(std::string s)
{
    for (char &c : s) c = char(tolower((unsigned char)c));
    return s;
}

bool traceMusic() { return getenv("SHOGO_TRACE_MUSIC") != nullptr; }

// Music level against sound effects at full volume.
constexpr float kMusicGain = 0.35f;

// band: name (16 wide chars), ..., programs at 40, volumes at 56, pans at 72 (one per part).
bool parseBand(const Chunk &form, ImaBand &band)
{
    const auto kids = children(form);
    const Chunk *b = find(kids, "band");
    if (!b || b->size < 88) return false;
    for (int p = 0; p < 16; ++p) {
        band.program[p] = b->data[40 + p];
        band.volume[p] = b->data[56 + p];
        band.pan[p] = b->data[72 + p];
    }
    return true;
}

// patt: ?, length (clocks), groove bits, clocks per grid step, steps per beat,
// clocks per beat, length in measures, name (16 wide chars), reference scale
// and chord, per-part note ranges and settings. Then one AACL list per grid
// step: its index ('clik') and its notes ('note': a record size, then
// records of type, part, time offset, variation bits, key, velocity, duration).
bool parsePattern(const Chunk &form, ImaPattern &pat)
{
    auto kids = children(form);
    const Chunk *h = find(kids, "patt");
    if (!h || h->size < 52) return false;
    pat.length = u32(h->data + 4);
    pat.grooves = u32(h->data + 8);
    const int clocksPerStep = u16(h->data + 12);
    pat.clocksPerBeat = u16(h->data + 16) ? u16(h->data + 16) : 192;
    pat.measures = u16(h->data + 18) ? u16(h->data + 18) : 1;
    pat.name = wideName(h->data + 20, 16);
    for (const Chunk &step : kids) {
        if (!step.isList("AACL")) continue;
        auto parts = children(step);
        const Chunk *clik = find(parts, "clik");
        if (!clik || clik->size < 2) continue;
        const int32_t base = int32_t(u16(clik->data)) * clocksPerStep;
        for (const Chunk &c : parts) {
            if (!c.is("note") || c.size < 2) continue;
            const size_t rec = u16(c.data);
            if (rec < 14) continue;
            for (size_t i = 2; i + rec <= c.size; i += rec) {
                const uint8_t *n = c.data + i;
                if (n[0] != 9) continue;  // note records only
                ImaNote note;
                note.part = n[1] & 15;
                note.time = base + int16_t(u16(n + 2));
                note.variations = u16(n + 4);
                note.key = n[8] & 127;
                note.velocity = n[9] & 127;
                note.duration = u16(n + 12);
                pat.partVariations[note.part] |= note.variations;
                pat.notes.push_back(note);
            }
        }
    }
    return true;
}

// Seconds per DLS envelope step at the synth's rate: a factor taking a level
// down by 96 dB over `seconds`.
float dbSlope(float seconds)
{
    if (seconds < 0.001f) seconds = 0.001f;
    return float(std::pow(10.0, -96.0 / 20.0 / (double(seconds) * DlsSynth::kRate)));
}

}  // namespace

// ---- formats ----------------------------------------------------------------

const ImaPattern *ImaStyle::patternFor(uint32_t groove, std::mt19937 &rng) const
{
    std::vector<const ImaPattern *> match;
    for (const ImaPattern &p : patterns)
        if (p.grooves & groove & 0xFF80) match.push_back(&p);
    if (match.empty())
        for (const ImaPattern &p : patterns) match.push_back(&p);
    if (match.empty()) return nullptr;
    return match[std::uniform_int_distribution<size_t>(0, match.size() - 1)(rng)];
}

// RIFF 'AASY': styl (name, time signature, grid, tempo), a band, patterns.
std::shared_ptr<ImaStyle> parseImaStyle(const std::vector<uint8_t> &data, std::string *err)
{
    auto top = children(data.data(), data.size());
    if (top.empty() || !top[0].isList("AASY")) {
        if (err) *err = "not an IMA style";
        return nullptr;
    }
    auto style = std::make_shared<ImaStyle>();
    for (const Chunk &c : children(top[0])) {
        if (c.is("styl") && c.size >= 44) {
            style->name = wideName(c.data, 16);
            style->beatsPerMeasure = u16(c.data + 32) ? u16(c.data + 32) : 4;
            style->tempo = u16(c.data + 42);
        } else if (c.isList("AABN")) {
            parseBand(c, style->band);
        } else if (c.isList("AAPT")) {
            ImaPattern p;
            if (parsePattern(c, p)) style->patterns.push_back(std::move(p));
        }
    }
    return style;
}

// RIFF 'AASE': secn (position in the song, name, tempo, measures), the style
// and personality by name, a band, chords, commands (the groove level) and mutes.
std::shared_ptr<ImaSection> parseImaSection(const std::vector<uint8_t> &data, std::string *err)
{
    auto top = children(data.data(), data.size());
    if (top.empty() || !top[0].isList("AASE")) {
        if (err) *err = "not an IMA section";
        return nullptr;
    }
    auto sec = std::make_shared<ImaSection>();
    for (const Chunk &c : children(top[0])) {
        if (c.is("secn") && c.size >= 42) {
            sec->name = wideName(c.data + 4, 16);
            sec->tempo = u16(c.data + 36);
            sec->measures = u16(c.data + 40);
        } else if (c.is("sref")) {
            sec->style = wideName(c.data, c.size / 2);
        } else if (c.is("cmnd") && c.size >= 10) {
            const size_t rec = u16(c.data);
            if (rec >= 8) sec->groove = u32(c.data + 2 + 4);  // the first command
        } else if (c.isList("AABN")) {
            sec->hasBand = parseBand(c, sec->band);
        }
    }
    if (sec->tempo <= 0) sec->tempo = 120;
    if (sec->measures <= 0) sec->measures = 1;
    return sec;
}

// ---- synth ------------------------------------------------------------------

void DlsSynth::setCollection(std::shared_ptr<const DlsCollection> c)
{
    if (c == m_dls) return;
    silence();
    m_dls = std::move(c);
}

DlsSynth::Voice *DlsSynth::allocate()
{
    Voice *best = nullptr;
    for (Voice &v : m_voices) {
        if (!v.active) return &v;
        // Steal the quietest released voice, else the oldest.
        if (!best) best = &v;
        else if (v.released != best->released) best = v.released ? &v : best;
        else if (v.released ? v.env < best->env : v.serial < best->serial) best = &v;
    }
    return best;
}

void DlsSynth::noteOn(int ch, int key, int velocity)
{
    ch &= 15;
    if (velocity <= 0) return noteOff(ch, key);
    noteOff(ch, key);
    if (!m_dls) return;
    const Channel &c = m_channels[ch];
    const DlsInstrument *ins = m_dls->find(0, uint32_t(c.program));
    if (!ins) return;
    for (const DlsRegion &r : ins->regions) {
        if (key < r.keyLo || key > r.keyHi || velocity < r.velLo || velocity > r.velHi) continue;
        if (r.wave < 0 || size_t(r.wave) >= m_dls->waves.size() || m_dls->waves[size_t(r.wave)].samples.empty()) continue;
        Voice *v = allocate();
        *v = Voice{};
        v->active = true;
        v->channel = ch;
        v->key = key;
        v->wave = &m_dls->waves[size_t(r.wave)];
        v->region = &r;
        v->art = m_dls->articulationFor(*ins, r);
        v->serial = ++m_serial;
        const double cents = (key - r.unityNote) * 100.0 + r.fineTune;
        v->step = double(v->wave->rate) / kRate * std::exp2(cents / 1200.0);
        const float vel = float(velocity) / 127.0f, vol = float(c.volume) / 127.0f;
        const float gain = r.gain * vel * vel * vol * vol;
        float p = std::clamp(float(c.pan) / 127.0f + v->art.pan * 0.5f, 0.0f, 1.0f);
        v->gainL = gain * std::cos(p * float(M_PI) / 2);
        v->gainR = gain * std::sin(p * float(M_PI) / 2);
        v->sustain = float(std::pow(10.0, -96.0 * (1.0 - v->art.sustain) / 20.0));
        v->stage = 0;
        v->env = 0;
        break;  // one region per note
    }
}

void DlsSynth::noteOff(int ch, int key)
{
    for (Voice &v : m_voices)
        if (v.active && !v.released && v.channel == (ch & 15) && v.key == key) {
            v.released = true;
            v.stage = 3;
        }
}

void DlsSynth::allNotesOff()
{
    for (Voice &v : m_voices)
        if (v.active) {
            v.released = true;
            v.stage = 3;
        }
}

void DlsSynth::silence()
{
    for (Voice &v : m_voices) v.active = false;
}

size_t DlsSynth::activeVoices() const
{
    size_t n = 0;
    for (const Voice &v : m_voices) n += v.active;
    return n;
}

void DlsSynth::render(float *out, int frames)
{
    const float dt = 1.0f / kRate;
    for (Voice &v : m_voices) {
        if (!v.active) continue;
        const DlsArticulation &a = v.art;
        // The LFO changes slowly: once per block.
        double step = v.step;
        float lfoGain = 1;
        if ((a.lfoPitchCents != 0 || a.lfoGainDb != 0) && v.age > a.lfoDelay) {
            const double l = std::sin(2 * M_PI * a.lfoHz * (v.age - a.lfoDelay));
            step *= std::exp2(l * a.lfoPitchCents / 1200.0);
            lfoGain = float(std::pow(10.0, l * a.lfoGainDb / 20.0));
        }
        const std::vector<int16_t> &s = v.wave->samples;
        const double n = double(s.size());
        const bool looped = v.region->looped && v.region->loopLength > 0 &&
                            v.region->loopStart + v.region->loopLength <= s.size();
        const double loopEnd = looped ? double(v.region->loopStart + v.region->loopLength) : n;
        const double loopLen = double(v.region->loopLength);
        const float attackStep = a.attack > dt ? dt / a.attack : 1.0f;
        const float decayK = dbSlope(a.decay), releaseK = dbSlope(a.release > 0.005f ? a.release : 0.005f);
        const float gl = v.gainL * lfoGain, gr = v.gainR * lfoGain;
        for (int i = 0; i < frames; ++i) {
            switch (v.stage) {
            case 0:
                v.env += attackStep;
                if (v.env >= 1) v.env = 1, v.stage = 1;
                break;
            case 1:
                v.env *= decayK;
                if (v.env <= v.sustain) v.env = v.sustain, v.stage = 2;
                break;
            case 2: break;
            default:
                v.env *= releaseK;
                if (v.env < 1e-4f) v.active = false;
                break;
            }
            if (!v.active) break;
            if (v.pos >= loopEnd) {
                if (!looped) {
                    v.active = false;
                    break;
                }
                v.pos -= loopLen * std::floor((v.pos - loopEnd) / loopLen + 1);
            }
            const size_t i0 = size_t(v.pos);
            size_t i1 = i0 + 1;
            if (looped && double(i1) >= loopEnd) i1 = v.region->loopStart;
            if (i1 >= s.size()) i1 = i0;
            const float t = float(v.pos - double(i0));
            const float smp = (s[i0] + (s[i1] - s[i0]) * t) * (1.0f / 32768.0f) * v.env;
            out[i * 2] += smp * gl;
            out[i * 2 + 1] += smp * gr;
            v.pos += step;
        }
        v.age += double(frames) * dt;
    }
}

// ---- sequencer --------------------------------------------------------------

std::vector<uint8_t> MusicPlayer::readFile(const std::string &name) const
{
    std::vector<uint8_t> data;
    std::string path = resolveGamePath((m_dir + "\\" + name).c_str());
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        data.resize(size_t(n));
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    fclose(f);
    return data;
}

bool MusicPlayer::setDirectory(const char *dir)
{
    if (!dir) return false;
    std::string path = resolveGamePath(dir);
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (traceMusic()) printf("[music] no music directory %s\n", dir);
        return false;
    }
    if (strcasecmp(m_dir.c_str(), dir) != 0) {
        m_dir = dir;
        m_sections.clear();
        m_styles.clear();
        m_stylesScanned = false;
    }
    return true;
}

bool MusicPlayer::initInstruments(const char *dlsFile, const char *)
{
    if (!dlsFile) return false;
    std::string path = lower(resolveGamePath((m_dir + "\\" + dlsFile).c_str()));
    if (path == m_dlsPath && m_dls) return true;
    std::string err;
    auto dls = parseDls(readFile(dlsFile), &err);
    if (!dls) {
        printf("[music] can't load instruments %s: %s\n", dlsFile, err.c_str());
        return false;
    }
    // New instruments: whatever is playing stops.
    cutAt(m_now);
    m_synth.setCollection(dls);
    m_dls = dls;
    m_dlsPath = path;
    if (traceMusic())
        printf("[music] %s: %zu instruments, %zu waves\n", dlsFile, dls->instruments.size(), dls->waves.size());
    return true;
}

const ImaStyle *MusicPlayer::style(const std::string &name)
{
    if (!m_stylesScanned) {
        m_stylesScanned = true;
        std::string dir = resolveGamePath(m_dir.c_str());
        if (DIR *d = opendir(dir.c_str())) {
            while (dirent *e = readdir(d)) {
                std::string f = e->d_name;
                if (f.size() < 4 || strcasecmp(f.c_str() + f.size() - 4, ".sty") != 0) continue;
                if (auto s = parseImaStyle(readFile(f))) m_styles[lower(s->name)] = s;
            }
            closedir(d);
        }
    }
    auto it = m_styles.find(lower(name));
    return it == m_styles.end() ? nullptr : it->second.get();
}

const ImaSection *MusicPlayer::section(const std::string &name)
{
    std::string key = lower(name);
    auto it = m_sections.find(key);
    if (it != m_sections.end()) return it->second.get();
    auto sec = parseImaSection(readFile(name));
    if (!sec) {
        printf("[music] can't load section %s\n", name.c_str());
        return nullptr;
    }
    m_sections[key] = sec;
    return sec.get();
}

bool MusicPlayer::loadSong(const char *name) { return name && section(name) != nullptr; }

void MusicPlayer::destroyAllSongs()
{
    // What is playing was already scheduled and plays on to its end.
    m_sections.clear();
    m_lists.clear();
}

bool MusicPlayer::addToPlayList(const char *list, const char *song)
{
    if (!list || !song || !loadSong(song)) return false;
    m_lists[lower(list)].push_back(song);
    return true;
}

void MusicPlayer::deletePlayList(const char *list)
{
    if (list) m_lists.erase(lower(list));
}

uint64_t MusicPlayer::nextMeasure() const
{
    if (!m_playing || m_samplesPerMeasure <= 0) return m_now;
    double elapsed = double(m_now - m_sectionStart);
    uint64_t t = m_sectionStart + uint64_t(std::ceil(elapsed / m_samplesPerMeasure) * m_samplesPerMeasure);
    return std::min(t, m_sectionEnd);
}

void MusicPlayer::cutAt(uint64_t at)
{
    m_events.erase(std::remove_if(m_events.begin(), m_events.end(),
                                  [&](const Event &e) { return e.type != 0 && e.at >= at; }),
                   m_events.end());
    for (Event &e : m_events)
        if (e.type == 0 && e.at > at) e.at = at;
    std::stable_sort(m_events.begin(), m_events.end(), [](const Event &a, const Event &b) { return a.at < b.at; });
}

bool MusicPlayer::playList(const char *list, const char *transition, bool loop, DDWORD boundary)
{
    if (!list) return false;
    auto it = m_lists.find(lower(list));
    if (it == m_lists.end() || it->second.empty()) return false;
    m_playlist = it->second;
    m_listPos = 0;
    m_loop = loop;
    m_transition = transition ? transition : "";
    m_stopAtEnd = false;
    if (traceMusic())
        printf("[music] play list %s (%zu sections) after %s, %s\n", list, m_playlist.size(),
               m_transition.empty() ? "nothing" : m_transition.c_str(),
               boundary == MUSIC_IMMEDIATE ? "now" : boundary == MUSIC_NEXTMEASURE ? "next measure" : "next section");
    if (!m_playing || boundary == MUSIC_IMMEDIATE) {
        cutAt(m_now);
        m_synth.allNotesOff();
        startNextSection(m_now);
    } else if (boundary == MUSIC_NEXTMEASURE) {
        uint64_t t = nextMeasure();
        cutAt(t);
        m_sectionEnd = t;
    }
    // Next song / queue: the new list follows the section that is playing.
    return true;
}

void MusicPlayer::stop(DDWORD boundary)
{
    if (!m_playing) return;
    if (boundary == MUSIC_IMMEDIATE) {
        cutAt(m_now);
        m_synth.allNotesOff();
        m_playing = false;
        m_currentName.clear();
        return;
    }
    if (boundary == MUSIC_NEXTMEASURE) {
        uint64_t t = nextMeasure();
        cutAt(t);
        m_sectionEnd = t;
    }
    m_stopAtEnd = true;
}

void MusicPlayer::startNextSection(uint64_t at)
{
    m_playing = false;
    m_currentName.clear();
    if (m_stopAtEnd) return;
    // Sections that don't load are skipped (at most one pass over the list).
    for (size_t tries = 0; tries <= m_playlist.size() + 1; ++tries) {
        std::string name;
        if (!m_transition.empty()) {
            name = m_transition;
            m_transition.clear();
        } else if (m_listPos < m_playlist.size()) {
            name = m_playlist[m_listPos++];
        } else if (m_loop && !m_playlist.empty()) {
            m_listPos = 0;
            name = m_playlist[m_listPos++];
        } else {
            return;
        }
        if (const ImaSection *sec = section(name)) {
            m_currentName = name;
            schedule(*sec, at);
            m_playing = true;
            return;
        }
    }
}

void MusicPlayer::schedule(const ImaSection &sec, uint64_t at)
{
    const ImaStyle *sty = style(sec.style);
    const ImaPattern *pat = sty ? sty->patternFor(sec.groove, m_rng) : nullptr;
    const int clocksPerBeat = pat ? pat->clocksPerBeat : 192;
    const int beats = sty ? sty->beatsPerMeasure : 4;
    const double samplesPerClock = double(DlsSynth::kRate) * 60.0 / (double(sec.tempo) * clocksPerBeat);
    const int64_t sectionClocks = int64_t(sec.measures) * beats * clocksPerBeat;
    m_samplesPerMeasure = samplesPerClock * beats * clocksPerBeat;
    m_sectionStart = at;
    m_sectionEnd = at + uint64_t(double(sectionClocks) * samplesPerClock);

    const ImaBand *band = sec.hasBand ? &sec.band : sty ? &sty->band : nullptr;
    if (band)
        for (int p = 0; p < 16; ++p) {
            m_synth.program(p, band->program[p]);
            m_synth.volume(p, band->volume[p]);
            m_synth.pan(p, band->pan[p]);
        }
    if (traceMusic())
        printf("[music] section %s: style %s, pattern %s, %d measures at %d bpm\n", sec.name.c_str(),
               sec.style.c_str(), pat ? pat->name.c_str() : "(none)", sec.measures, sec.tempo);
    if (!pat || pat->length == 0) return;

    for (int64_t start = 0; start < sectionClocks; start += pat->length) {
        // Each part plays one of the variations it has notes in, chosen anew on every pass.
        int variation[16];
        for (int p = 0; p < 16; ++p) {
            std::vector<int> bits;
            for (int b = 0; b < 16; ++b)
                if (pat->partVariations[p] & (1u << b)) bits.push_back(b);
            variation[p] = bits.empty() ? 0 : bits[std::uniform_int_distribution<size_t>(0, bits.size() - 1)(m_rng)];
        }
        for (const ImaNote &n : pat->notes) {
            if (!(n.variations & (1u << variation[n.part]))) continue;
            int64_t t = start + n.time;
            if (t < 0) t = 0;
            if (t >= sectionClocks) continue;
            Event on;
            on.at = at + uint64_t(double(t) * samplesPerClock);
            on.type = 1;
            on.channel = n.part;
            on.key = n.key;
            on.velocity = n.velocity;
            Event off = on;
            off.type = 0;
            off.at = on.at + std::max<uint64_t>(1, uint64_t(double(n.duration) * samplesPerClock));
            m_events.push_back(on);
            m_events.push_back(off);
        }
    }
    // Offs before ons at the same time, so a repeated note restarts.
    std::stable_sort(m_events.begin(), m_events.end(),
                     [](const Event &a, const Event &b) { return a.at != b.at ? a.at < b.at : a.type < b.type; });
}

void MusicPlayer::render(float *out, int frames)
{
    if (m_paused) return;
    if (!m_playing && m_events.empty() && m_synth.activeVoices() == 0) {
        m_now += uint64_t(frames);
        return;
    }
    std::vector<float> buf(size_t(frames) * 2, 0.0f);
    int done = 0;
    size_t next = 0;
    while (done < frames) {
        if (m_playing && m_now >= m_sectionEnd) {
            // Drop what has been played before scheduling more.
            m_events.erase(m_events.begin(), m_events.begin() + ptrdiff_t(next));
            next = 0;
            startNextSection(m_sectionEnd);
        }
        for (; next < m_events.size() && m_events[next].at <= m_now; ++next) {
            const Event &e = m_events[next];
            if (e.type == 1) m_synth.noteOn(e.channel, e.key, e.velocity);
            else m_synth.noteOff(e.channel, e.key);
        }
        uint64_t until = m_now + 64;
        if (next < m_events.size()) until = std::min(until, m_events[next].at);
        if (m_playing) until = std::min(until, std::max(m_sectionEnd, m_now + 1));
        int n = int(std::min<uint64_t>(uint64_t(frames - done), std::max<uint64_t>(1, until - m_now)));
        m_synth.render(buf.data() + size_t(done) * 2, n);
        m_now += uint64_t(n);
        done += n;
    }
    m_events.erase(m_events.begin(), m_events.begin() + ptrdiff_t(next));
    const float gain = kMusicGain * float(m_volume) / 100.0f;
    for (size_t i = 0; i < buf.size(); ++i) out[i] += buf[i] * gain;
}

MusicPlayer &music()
{
    static MusicPlayer m;
    return m;
}

// ---- the client API -------------------------------------------------------------

namespace {

DBOOL initMusic(char *driver)
{
    // "ima.dll" is the interactive music; "cdaudio.dll" would be CD tracks, which there are none of.
    bool ima = driver && strcasestr(driver, "ima") != nullptr;
    if (ima) mixer().setMusicSource([](float *out, int frames) { music().render(out, frames); });
    return ima ? DTRUE : DFALSE;
}
DBOOL setMusicDirectory(char *dir) { return music().setDirectory(dir) ? DTRUE : DFALSE; }
DBOOL initInstruments(char *dls, char *style) { return music().initInstruments(dls, style) ? DTRUE : DFALSE; }
DBOOL loadSong(char *name) { return music().loadSong(name) ? DTRUE : DFALSE; }
void destroyAllSongs() { music().destroyAllSongs(); }
DBOOL playList(char *list, char *transition, DBOOL loop, DDWORD boundary)
{
    return music().playList(list, transition, loop != 0, boundary) ? DTRUE : DFALSE;
}
DBOOL playBreak(char *, DDWORD) { return DFALSE; }  // the game never asks for one
void stopMusic(DDWORD boundary) { music().stop(boundary); }
DBOOL pauseMusic()
{
    music().pause(true);
    return DTRUE;
}
DBOOL resumeMusic()
{
    music().pause(false);
    return DTRUE;
}
DBOOL addSongToPlayList(char *list, char *song) { return music().addToPlayList(list, song) ? DTRUE : DFALSE; }
void deletePlayList(char *list) { music().deletePlayList(list); }
DBOOL playMotif(char *, DBOOL) { return DFALSE; }  // nor for motifs
void stopMotif(char *) {}
short getMusicVolume() { return short(music().volume()); }
void setMusicVolume(short v) { music().setVolume(v); }

}  // namespace

void installMusicAPI(ClientDE *c)
{
    c->InitMusic = initMusic;
    c->SetMusicDirectory = setMusicDirectory;
    c->InitInstruments = initInstruments;
    c->LoadSong = loadSong;
    c->DestroyAllSongs = destroyAllSongs;
    c->PlayList = playList;
    c->PlayBreak = playBreak;
    c->StopMusic = stopMusic;
    c->PauseMusic = pauseMusic;
    c->ResumeMusic = resumeMusic;
    c->AddSongToPlayList = addSongToPlayList;
    c->DeletePlayList = deletePlayList;
    c->PlayMotif = playMotif;
    c->StopMotif = stopMotif;
    c->GetMusicVolume = getMusicVolume;
    c->SetMusicVolume = setMusicVolume;
}

}  // namespace shogo
