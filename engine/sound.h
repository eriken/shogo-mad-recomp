// Sound effects: WAV loading, a software mixer and the client/server sound API.
//
// Every sound in Shogo's SOUND.REZ is PCM mono (8 or 16 bit, 11 or 22 kHz).
// The mixer resamples to 44.1 kHz stereo. Sounds are local (in the player's
// head), ambient (distance falloff) or 3D (falloff plus left/right pan); full
// volume inside the inner radius, fading linearly to nothing at the outer one.
//
// The mixer runs on the caller's thread: mix() fills a buffer, the window
// queues it to the audio device, the probe can write it to a WAV file.
#ifndef SHOGO_ENGINE_SOUND_H
#define SHOGO_ENGINE_SOUND_H

#include "engine/api.h"
#include "engine/world_dat.h"  // Vec3

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace shogo {

class ResourceFS;
struct EngineObject;

struct SoundData {
    std::vector<int16_t> samples;  // mono
    int rate = 22050;
    float duration() const { return rate > 0 ? float(samples.size()) / float(rate) : 0.0f; }
};

// Decodes a PCM WAV (8/16 bit, mono or stereo, which is mixed down); nullptr otherwise.
std::shared_ptr<SoundData> parseWav(const std::vector<uint8_t> &data);

class Mixer {
public:
    static constexpr int kRate = 44100;

    void setResources(const ResourceFS *fs) { m_fs = fs; }

    // Starts a sound; returns its id (0 if it couldn't play). With
    // PLAYSOUND_GETHANDLE the sound stays until kill(), otherwise it is
    // dropped once finished. localObject: sounds attached to this object
    // play in the head (the player's own sounds).
    uint32_t play(const PlaySoundInfo &info, EngineObject *attachTo, bool local);
    void kill(uint32_t id);
    bool isDone(uint32_t id) const;
    float duration(uint32_t id) const;
    bool setPosition(uint32_t id, const DVector &pos);
    bool getPosition(uint32_t id, DVector *pos) const;
    void pause(bool paused) { m_paused = paused; }
    void setVolume(int percent) { m_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
    int volume() const { return m_volume; }
    void setListener(const Vec3 &pos, const Vec3 &right);
    // Drops everything (new world, shutdown).
    void clear();

    // Fills frames stereo samples (interleaved L R).
    void mix(int16_t *out, int frames);

    size_t voiceCount() const { return m_voices.size(); }
    size_t started() const { return m_started; }
    const SoundData *load(const std::string &name);

private:
    struct Voice {
        uint32_t id = 0;
        std::shared_ptr<SoundData> data;
        double pos = 0;  // in source samples
        DDWORD flags = 0;
        float volume = 1;
        float inner = 0, outer = 0;
        DVector where{};
        uint32_t attached = 0;  // object id
        bool local = false;
        bool done = false;
        bool keep = false;  // GETHANDLE: stays until killed
        uint8_t priority = 0;
    };
    Voice *find(uint32_t id);
    const Voice *find(uint32_t id) const;
    void gains(const Voice &v, float &l, float &r) const;

    const ResourceFS *m_fs = nullptr;
    std::unordered_map<std::string, std::shared_ptr<SoundData>> m_cache;
    std::vector<Voice> m_voices;
    std::vector<float> m_acc;
    uint32_t m_nextId = 1;
    size_t m_started = 0;
    int m_volume = 100;
    bool m_paused = false;
    Vec3 m_listener, m_right{1, 0, 0};
};

Mixer &mixer();

// Writes 16-bit stereo samples (interleaved) as a WAV file.
bool writeWav(const std::string &path, const std::vector<int16_t> &stereo, int rate);

// The sound functions of each interface (music calls are accepted and ignored).
void installSoundAPI(ClientDE *client);
void installSoundAPI(ServerDE *server, EngineObject *(*playerObject)());

// Listener for SetListener(TRUE): the camera last rendered.
void soundCameraMoved(const Vec3 &pos, const Vec3 &right);

}  // namespace shogo

#endif
