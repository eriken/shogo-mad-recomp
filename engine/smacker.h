// Smacker movies (RAD Game Tools' SMK2/SMK4), used for Shogo's LOGO.SMK and
// INTRO.SMK. Written from the publicly documented file format.
//
// A file is a 104-byte header, per-frame sizes and types, four Huffman trees
// for the video, then the frames. Each frame may carry a palette update, one
// audio chunk per track, and the video: 4x4 blocks of palette indices coded
// as runs of mono (two colors and a bit mask), full (16 colors), skip (keep
// the previous frame) and solid fill blocks. Audio is raw PCM or Huffman-coded
// sample deltas (DPCM).
#ifndef SHOGO_ENGINE_SMACKER_H
#define SHOGO_ENGINE_SMACKER_H

#include <cstdint>
#include <string>
#include <vector>

namespace shogo {

class SmackerVideo {
public:
    // Takes the whole file; false (with a reason) if it isn't a Smacker movie.
    bool load(std::vector<uint8_t> data, std::string *err = nullptr);
    bool open(const std::string &path, std::string *err = nullptr);

    int width() const { return m_width; }
    int height() const { return m_height; }  // stored lines (see interlaced/doubled)
    int frameCount() const { return m_frames; }
    double frameDuration() const { return m_frameSeconds; }
    bool interlaced() const { return (m_flags & 2) != 0; }  // shown with a blank line after each line
    bool doubled() const { return (m_flags & 4) != 0; }     // shown with each line twice

    // Track 0's format (0 rate: no sound).
    int audioRate() const { return m_audio.rate; }
    int audioChannels() const { return m_audio.stereo ? 2 : 1; }

    // Decodes the next frame; false at the end or on bad data.
    bool nextFrame();
    int frameIndex() const { return m_current; }  // last decoded, -1 before the first

    const std::vector<uint8_t> &image() const { return m_image; }  // width*height palette indices
    const uint32_t *palette() const { return m_palette; }          // 0xRRGGBB
    // Track 0's sound from the last decoded frame: 16-bit, interleaved if stereo.
    const std::vector<int16_t> &audio() const { return m_samples; }

    // A big Huffman tree: 16-bit values, three of them "recently used" slots
    // that change as values are decoded.
    struct BigTree {
        std::vector<int32_t> table;  // nodes hold kNode | size of the 0 subtree
        int last[3] = {0, 0, 0};
        void reset();
    };

private:
    struct AudioTrack {
        int rate = 0;
        bool compressed = false, stereo = false, bits16 = false;
    };
    bool readTrees(std::string *err);
    bool decodePalette(const uint8_t *p, size_t n);
    void decodeVideo(const uint8_t *p, size_t n);
    void decodeAudio(const uint8_t *p, size_t n);

    std::vector<uint8_t> m_data;
    int m_width = 0, m_height = 0, m_frames = 0;
    uint32_t m_flags = 0;
    bool m_v4 = false;
    double m_frameSeconds = 0.1;
    AudioTrack m_audio;
    std::vector<uint32_t> m_frameSizes;
    std::vector<uint8_t> m_frameTypes;
    size_t m_nextOffset = 0;
    int m_current = -1;
    BigTree m_mmap, m_mclr, m_full, m_type;
    std::vector<uint8_t> m_image;
    uint32_t m_palette[256] = {};
    std::vector<int16_t> m_samples;
};

}  // namespace shogo

#endif
