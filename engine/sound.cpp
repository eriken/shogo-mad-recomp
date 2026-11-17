#include "engine/sound.h"

#include "engine/math3d.h"
#include "engine/objects.h"
#include "engine/rez_file.h"
#include "engine/stub_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace shogo {

// ---- WAV ---------------------------------------------------------------------------------------

std::shared_ptr<SoundData> parseWav(const std::vector<uint8_t> &d)
{
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) != 0 || memcmp(&d[8], "WAVE", 4) != 0) return nullptr;
    uint16_t tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = nullptr;
    size_t pcmSize = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t size;
        memcpy(&size, &d[p + 4], 4);
        size_t body = p + 8, avail = std::min<size_t>(size, d.size() - body);
        if (!memcmp(&d[p], "fmt ", 4) && avail >= 16) {
            memcpy(&tag, &d[body], 2);
            memcpy(&channels, &d[body + 2], 2);
            memcpy(&rate, &d[body + 4], 4);
            memcpy(&bits, &d[body + 14], 2);
        } else if (!memcmp(&d[p], "data", 4)) {
            pcm = &d[body];
            pcmSize = avail;
        }
        p = body + size + (size & 1);
    }
    if (tag != 1 || !pcm || channels < 1 || channels > 2 || (bits != 8 && bits != 16) || rate == 0) return nullptr;
    auto s = std::make_shared<SoundData>();
    s->rate = (int)rate;
    const size_t frameBytes = size_t(channels) * (bits / 8), n = pcmSize / frameBytes;
    s->samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
        int sum = 0;
        for (int c = 0; c < channels; ++c) {
            const uint8_t *q = pcm + i * frameBytes + size_t(c) * (bits / 8);
            if (bits == 8) {
                sum += (int(*q) - 128) << 8;
            } else {
                int16_t v;
                memcpy(&v, q, 2);
                sum += v;
            }
        }
        s->samples[i] = int16_t(sum / channels);
    }
    return s;
}

// ---- mixer -------------------------------------------------------------------------------------

Mixer &mixer()
{
    static Mixer m;
    return m;
}

const SoundData *Mixer::load(const std::string &name)
{
    std::string key = RezFile::key(name);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second.get();
    std::shared_ptr<SoundData> s;
    if (m_fs)
        if (auto d = m_fs->read(name)) s = parseWav(*d);
    if (!s && getenv("SHOGO_TRACE_SOUND")) fprintf(stderr, "[sound] can't load %s\n", name.c_str());
    return m_cache.emplace(key, s).first->second.get();
}

uint32_t Mixer::play(const PlaySoundInfo &info, EngineObject *attachTo, bool local)
{
    load(info.m_szSoundName);
    auto data = m_cache[RezFile::key(info.m_szSoundName)];
    if (!data || data->samples.empty()) return 0;

    // A voice limit like the hardware had: drop the oldest finished-or-lowest one.
    constexpr size_t kMaxVoices = 48;
    if (m_voices.size() >= kMaxVoices) {
        auto victim = std::min_element(m_voices.begin(), m_voices.end(), [](const Voice &a, const Voice &b) {
            if (a.keep != b.keep) return !a.keep;  // never steal a sound the game holds a handle to
            return a.priority < b.priority;
        });
        if (victim->keep || victim->priority > info.m_nPriority) return 0;
        m_voices.erase(victim);
    }

    Voice v;
    v.id = m_nextId++;
    v.data = data;
    v.flags = info.m_dwFlags;
    v.volume = (info.m_dwFlags & PLAYSOUND_CTRL_VOL) ? info.m_nVolume / 100.0f : 1.0f;
    v.inner = info.m_fInnerRadius;
    v.outer = info.m_fOuterRadius;
    v.where = info.m_vPosition;
    v.priority = info.m_nPriority;
    v.keep = (info.m_dwFlags & PLAYSOUND_GETHANDLE) != 0;
    v.local = local || !(info.m_dwFlags & (PLAYSOUND_3D | PLAYSOUND_AMBIENT));
    if (attachTo && (info.m_dwFlags & PLAYSOUND_ATTACHED)) {
        v.attached = attachTo->id;
        v.where = attachTo->pos;
    }
    m_voices.push_back(v);
    ++m_started;
    if (getenv("SHOGO_TRACE_SOUND"))
        fprintf(stderr, "[sound] #%u %s flags %x vol %.2f radius %.0f-%.0f %s%s\n", v.id, info.m_szSoundName,
                info.m_dwFlags, v.volume, v.inner, v.outer, v.local ? "local" : "positional",
                v.attached ? " attached" : "");
    return v.id;
}

Mixer::Voice *Mixer::find(uint32_t id)
{
    for (Voice &v : m_voices)
        if (v.id == id) return &v;
    return nullptr;
}
const Mixer::Voice *Mixer::find(uint32_t id) const
{
    for (const Voice &v : m_voices)
        if (v.id == id) return &v;
    return nullptr;
}

void Mixer::kill(uint32_t id)
{
    m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [id](const Voice &v) { return v.id == id; }),
                   m_voices.end());
}

bool Mixer::isDone(uint32_t id) const
{
    const Voice *v = find(id);
    return !v || v->done;
}

float Mixer::duration(uint32_t id) const
{
    const Voice *v = find(id);
    return v ? v->data->duration() : 0.0f;
}

bool Mixer::setPosition(uint32_t id, const DVector &pos)
{
    Voice *v = find(id);
    if (!v) return false;
    v->where = pos;
    return true;
}

bool Mixer::getPosition(uint32_t id, DVector *pos) const
{
    const Voice *v = find(id);
    if (!v) return false;
    if (pos) *pos = v->where;
    return true;
}

void Mixer::setListener(const Vec3 &pos, const Vec3 &right)
{
    m_listener = pos;
    m_right = right;
}

void Mixer::clear()
{
    m_voices.clear();
}

void Mixer::gains(const Voice &v, float &l, float &r) const
{
    l = r = v.volume;
    if (v.local) return;
    Vec3 d{v.where.x - m_listener.x, v.where.y - m_listener.y, v.where.z - m_listener.z};
    float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    float g = 1.0f;
    if (v.outer > 0 && dist >= v.outer) g = 0;
    else if (dist > v.inner && v.outer > v.inner) g = 1.0f - (dist - v.inner) / (v.outer - v.inner);
    l *= g;
    r *= g;
    if ((v.flags & PLAYSOUND_3D) && dist > 1.0f && g > 0) {
        float x = (d.x * m_right.x + d.y * m_right.y + d.z * m_right.z) / dist;  // -1 left .. 1 right
        l *= std::min(1.0f, 1.0f - x * 0.8f);
        r *= std::min(1.0f, 1.0f + x * 0.8f);
    }
}

void Mixer::mix(int16_t *out, int frames)
{
    m_acc.assign(size_t(frames) * 2, 0.0f);
    // Attached sounds follow their object; a removed object ends its sound.
    for (Voice &v : m_voices) {
        if (!v.attached || v.done) continue;
        EngineObject *o = objects().find(v.attached);
        if (!o || o->removed) v.done = true;
        else v.where = o->pos;
    }
    if (!m_paused) {
        for (Voice &v : m_voices) {
            if (v.done) continue;
            float gl, gr;
            gains(v, gl, gr);
            const std::vector<int16_t> &s = v.data->samples;
            const double step = double(v.data->rate) / kRate;
            const bool loop = (v.flags & PLAYSOUND_LOOP) != 0;
            const size_t n = s.size();
            for (int i = 0; i < frames; ++i) {
                if (v.pos >= double(n)) {
                    if (!loop) {
                        v.done = true;
                        break;
                    }
                    v.pos = std::fmod(v.pos, double(n));
                }
                size_t i0 = size_t(v.pos);
                size_t i1 = i0 + 1 < n ? i0 + 1 : (loop ? 0 : i0);
                float t = float(v.pos - double(i0));
                float smp = (s[i0] + (s[i1] - s[i0]) * t) * (1.0f / 32768.0f);
                if (gl > 0 || gr > 0) {
                    m_acc[size_t(i) * 2] += smp * gl;
                    m_acc[size_t(i) * 2 + 1] += smp * gr;
                }
                v.pos += step;
            }
        }
        // Finished fire-and-forget sounds go away; handle sounds wait for KillSound.
        m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [](const Voice &v) { return v.done && !v.keep; }),
                       m_voices.end());
    }
    const float master = m_volume / 100.0f;
    for (size_t i = 0; i < m_acc.size(); ++i) {
        // Soft knee above 0.75 so piles of gunfire squash instead of clipping.
        float x = m_acc[i] * master, a = std::fabs(x);
        if (a > 0.75f) x = std::copysign(0.75f + 0.25f * std::tanh((a - 0.75f) / 0.25f), x);
        out[i] = int16_t(x * 32767.0f);
    }
}

bool writeWav(const std::string &path, const std::vector<int16_t> &stereo, int rate)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(stereo.size() * 2);
    fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32(uint32_t(rate));
    u32(uint32_t(rate) * 4);
    u16(4);
    u16(16);
    fwrite("data", 1, 4, f);
    u32(bytes);
    fwrite(stereo.data(), 2, stereo.size(), f);
    return fclose(f) == 0;
}

// ---- API ---------------------------------------------------------------------------------------

namespace {

HSOUNDDE toHandle(uint32_t id) { return reinterpret_cast<HSOUNDDE>(uintptr_t(id)); }
uint32_t fromHandle(HSOUNDDE h) { return uint32_t(reinterpret_cast<uintptr_t>(h)); }

bool g_listenerInClient = true;
Vec3 g_cameraPos, g_cameraRight{1, 0, 0};
EngineObject *(*g_playerObject)() = nullptr;

// Client.
DRESULT cPlaySound(PlaySoundInfo *info)
{
    if (!info) return LT_ERROR;
    uint32_t id = mixer().play(*info, nullptr, false);
    if (info->m_dwFlags & PLAYSOUND_GETHANDLE) info->m_hSound = id ? toHandle(id) : nullptr;
    return id ? LT_OK : LT_ERROR;
}
DRESULT cSetSoundPosition(HSOUNDDE h, DVector *pos)
{
    if (!pos) return DE_INVALIDPARAMS;
    return mixer().setPosition(fromHandle(h), *pos) ? LT_OK : LT_ERROR;
}
DRESULT cGetSoundPosition(HSOUNDDE h, DVector *pos) { return mixer().getPosition(fromHandle(h), pos) ? LT_OK : LT_ERROR; }
void cPauseSounds() { mixer().pause(true); }
void cResumeSounds() { mixer().pause(false); }
DRESULT getSoundDuration(HSOUNDDE h, DFLOAT *d)
{
    if (d) *d = mixer().duration(fromHandle(h));
    return mixer().isDone(fromHandle(h)) && mixer().duration(fromHandle(h)) == 0 ? DE_INVALIDPARAMS : LT_OK;
}
DBOOL cIsDone(HSOUNDDE h) { return mixer().isDone(fromHandle(h)); }
void cKillSound(HSOUNDDE h) { mixer().kill(fromHandle(h)); }
void cSetListener(DBOOL inClient, DVector *pos, DRotation *rot)
{
    g_listenerInClient = inClient != 0;
    if (g_listenerInClient) {
        mixer().setListener(g_cameraPos, g_cameraRight);
    } else if (pos) {
        DVector u, r, f;
        quatVectors(rot ? *rot : quatIdentity(), &u, &r, &f);
        mixer().setListener({pos->x, pos->y, pos->z}, {r.x, r.y, r.z});
    }
}
unsigned short cGetSoundVolume() { return (unsigned short)mixer().volume(); }
void cSetSoundVolume(unsigned short v) { mixer().setVolume(v); }
DRESULT cInitSound(InitSoundInfo *info)
{
    if (info) {
        info->m_dwResults = 0;  // no reverb
        mixer().setVolume(info->m_nVolume ? info->m_nVolume : 100);
    }
    return LT_OK;
}
DRESULT cGetReverb(ReverbProperties *) { return LT_ERROR; }
DRESULT cSetReverb(ReverbProperties *) { return LT_OK; }
DRESULT cGetProviders(Sound3DProvider *&list, DBOOL)
{
    list = nullptr;
    return LT_OK;
}
void cReleaseProviders(Sound3DProvider *) {}

// Music (DirectMusic data in Music\): not played yet; say there's no music driver.
DBOOL mInitMusic(char *) { return DFALSE; }
DBOOL mFalse1(char *) { return DFALSE; }
void mVoid() {}
DBOOL mFalse0() { return DFALSE; }
DBOOL mPlayList(char *, char *, DBOOL, DDWORD) { return DFALSE; }
DBOOL mPlayBreak(char *, DDWORD) { return DFALSE; }
void mStop(DDWORD) {}
DBOOL mAddSong(char *, char *) { return DFALSE; }
void mDeletePlayList(char *) {}
DBOOL mPlayMotif(char *, DBOOL) { return DFALSE; }
void mStopMotif(char *) {}
short g_musicVolume = 100;
short mGetVolume() { return g_musicVolume; }
void mSetVolume(short v) { g_musicVolume = v; }

// Server: one local client, so server sounds go straight to the mixer.
DRESULT sPlaySound(PlaySoundInfo *info)
{
    if (!info) return LT_ERROR;
    EngineObject *obj = toObject(info->m_hObject);
    EngineObject *player = g_playerObject ? g_playerObject() : nullptr;
    // The player's own sounds (attached or client-local) play in the head.
    bool local = player && obj == player && (info->m_dwFlags & (PLAYSOUND_ATTACHED | PLAYSOUND_CLIENTLOCAL));
    uint32_t id = mixer().play(*info, obj, local);
    if (info->m_dwFlags & PLAYSOUND_GETHANDLE) info->m_hSound = id ? toHandle(id) : nullptr;
    return id ? LT_OK : LT_ERROR;
}
DRESULT sIsSoundDone(HSOUNDDE h, DBOOL *done)
{
    if (done) *done = mixer().isDone(fromHandle(h));
    return LT_OK;
}
DRESULT sKillSound(HSOUNDDE h)
{
    mixer().kill(fromHandle(h));
    return LT_OK;
}

}  // namespace

void soundCameraMoved(const Vec3 &pos, const Vec3 &right)
{
    g_cameraPos = pos;
    g_cameraRight = right;
    if (g_listenerInClient) mixer().setListener(pos, right);
}

void installSoundAPI(ClientDE *c)
{
    c->PlaySound = cPlaySound;
    c->SetSoundPosition = cSetSoundPosition;
    c->GetSoundPosition = cGetSoundPosition;
    c->PauseSounds = cPauseSounds;
    c->ResumeSounds = cResumeSounds;
    c->GetSoundDuration = getSoundDuration;
    c->IsDone = cIsDone;
    c->KillSound = cKillSound;
    c->SetListener = cSetListener;
    c->GetSoundVolume = cGetSoundVolume;
    c->SetSoundVolume = cSetSoundVolume;
    c->InitSound = cInitSound;
    c->GetReverbProperties = cGetReverb;
    c->SetReverbProperties = cSetReverb;
    c->GetSound3DProviderLists = cGetProviders;
    c->ReleaseSound3DProviderList = cReleaseProviders;

    c->InitMusic = mInitMusic;
    c->SetMusicDirectory = mFalse1;
    c->LoadSong = mFalse1;
    c->DestroyAllSongs = mVoid;
    c->PlayList = mPlayList;
    c->PlayBreak = mPlayBreak;
    c->StopMusic = mStop;
    c->PauseMusic = mFalse0;
    c->ResumeMusic = mFalse0;
    c->AddSongToPlayList = mAddSong;
    c->DeletePlayList = mDeletePlayList;
    c->PlayMotif = mPlayMotif;
    c->StopMotif = mStopMotif;
    c->GetMusicVolume = mGetVolume;
    c->SetMusicVolume = mSetVolume;
}

void installSoundAPI(ServerDE *s, EngineObject *(*playerObject)())
{
    g_playerObject = playerObject;
    s->PlaySound = sPlaySound;
    s->GetSoundDuration = getSoundDuration;
    s->IsSoundDone = sIsSoundDone;
    s->KillSound = sKillSound;
}

}  // namespace shogo
