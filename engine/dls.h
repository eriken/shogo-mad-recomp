// DLS level 1 instrument collections (Downloadable Sounds, the MIDI
// Manufacturers Association's published format), as used by Shogo's music:
// each soundtrack folder has one .DLS holding its instruments and samples.
//
// A collection is a RIFF 'DLS ' form: instruments ('ins ': bank and program,
// key/velocity regions, articulation), a pool of PCM waves ('wvpl') and a
// table of offsets into the pool ('ptbl'). Articulations are DLS level 1
// connection blocks; the ones Shogo's banks use are the volume envelope,
// pan and the LFO's rate, delay and depth on pitch and volume.
#ifndef SHOGO_ENGINE_DLS_H
#define SHOGO_ENGINE_DLS_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shogo {

struct DlsWave {
    std::vector<int16_t> samples;  // mono
    int rate = 22050;
};

struct DlsArticulation {
    // Volume envelope, seconds; sustain as a fraction of full level.
    float attack = 0, decay = 0, sustain = 1, release = 0;
    float pan = 0;                       // -1 left .. 1 right
    float lfoHz = 5, lfoDelay = 0;       // LFO
    float lfoPitchCents = 0, lfoGainDb = 0;
};

struct DlsRegion {
    int keyLo = 0, keyHi = 127, velLo = 0, velHi = 127;
    int wave = -1;  // index into DlsCollection::waves
    int unityNote = 60;
    int fineTune = 0;  // cents
    float gain = 1;    // from the sample's attenuation
    bool looped = false;
    uint32_t loopStart = 0, loopLength = 0;
    int articulation = -1;  // index into DlsCollection::articulations (-1: the instrument's)
};

struct DlsInstrument {
    uint32_t bank = 0;  // high bit: drum kit
    uint32_t program = 0;
    std::string name;
    std::vector<DlsRegion> regions;
    int articulation = -1;  // -1: defaults
};

struct DlsCollection {
    std::vector<DlsInstrument> instruments;
    std::vector<DlsWave> waves;
    std::vector<DlsArticulation> articulations;

    const DlsInstrument *find(uint32_t bank, uint32_t program) const;
    const DlsArticulation &articulationFor(const DlsInstrument &ins, const DlsRegion &r) const;
};

// nullptr (with a reason) if the data isn't a DLS collection.
std::shared_ptr<DlsCollection> parseDls(const std::vector<uint8_t> &data, std::string *err = nullptr);

}  // namespace shogo

#endif
