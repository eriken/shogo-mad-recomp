#include "engine/dls.h"

#include "engine/riff.h"

#include <algorithm>
#include <cmath>

namespace shogo {

using namespace riff;

namespace {

// DLS level 1 connection sources and destinations.
constexpr uint16_t kSrcNone = 0, kSrcLfo = 1;
constexpr uint16_t kDstAttenuation = 0x0001, kDstPitch = 0x0003, kDstPan = 0x0004;
constexpr uint16_t kDstLfoFrequency = 0x0104, kDstLfoDelay = 0x0105;
constexpr uint16_t kDstEg1Attack = 0x0206, kDstEg1Decay = 0x0207, kDstEg1Release = 0x0209, kDstEg1Sustain = 0x020A;

// Time cents to seconds; the smallest value means no time at all.
float timecents(int32_t scale)
{
    if (scale == int32_t(0x80000000)) return 0;
    return float(std::exp2(double(scale) / 65536.0 / 1200.0));
}

DlsArticulation parseArt1(const Chunk &c)
{
    DlsArticulation a;
    if (c.size < 8) return a;
    uint32_t cb = u32(c.data), n = u32(c.data + 4);
    for (uint32_t i = 0; i < n && cb + (i + 1) * 12 <= c.size; ++i) {
        const uint8_t *b = c.data + cb + i * 12;
        uint16_t src = u16(b), ctl = u16(b + 2), dst = u16(b + 4);
        int32_t scale = int32_t(u32(b + 8));
        if (ctl != kSrcNone) continue;
        const double v = double(scale) / 65536.0;
        if (src == kSrcNone) {
            switch (dst) {
            case kDstEg1Attack: a.attack = timecents(scale); break;
            case kDstEg1Decay: a.decay = timecents(scale); break;
            case kDstEg1Release: a.release = timecents(scale); break;
            case kDstEg1Sustain: a.sustain = float(std::clamp(v / 1000.0, 0.0, 1.0)); break;
            case kDstPan: a.pan = float(std::clamp(v / 500.0, -1.0, 1.0)); break;  // tenths of a percent
            case kDstLfoFrequency: a.lfoHz = float(8.176 * std::exp2(v / 1200.0)); break;  // absolute pitch cents
            case kDstLfoDelay: a.lfoDelay = timecents(scale); break;
            default: break;
            }
        } else if (src == kSrcLfo) {
            if (dst == kDstPitch) a.lfoPitchCents = float(v);
            else if (dst == kDstAttenuation) a.lfoGainDb = float(v / 10.0);  // tenths of a dB
        }
    }
    return a;
}

// wsmp: unity note, fine tune, attenuation, then loops.
void parseWsmp(const Chunk &c, DlsRegion &r)
{
    if (c.size < 20) return;
    uint32_t cb = u32(c.data);
    r.unityNote = u16(c.data + 4);
    r.fineTune = int16_t(u16(c.data + 6));
    int32_t att = int32_t(u32(c.data + 8));
    r.gain = float(std::pow(10.0, double(att) / 655360.0 / 20.0));  // 1/655360 dB units
    uint32_t loops = u32(c.data + 16);
    r.looped = false;
    if (loops && cb + 16 <= c.size) {
        const uint8_t *l = c.data + cb;
        r.looped = true;
        r.loopStart = u32(l + 8);
        r.loopLength = u32(l + 12);
    }
}

int addArticulation(DlsCollection &col, const std::vector<Chunk> &kids)
{
    const Chunk *lart = nullptr;
    for (const Chunk &k : kids)
        if (k.isList("lart")) lart = &k;
    if (!lart) return -1;
    const auto parts = children(*lart);
    if (const Chunk *a = find(parts, "art1")) {
        col.articulations.push_back(parseArt1(*a));
        return int(col.articulations.size() - 1);
    }
    return -1;
}

}  // namespace

const DlsInstrument *DlsCollection::find(uint32_t bank, uint32_t program) const
{
    for (const DlsInstrument &i : instruments)
        if (i.bank == bank && i.program == program) return &i;
    return nullptr;
}

const DlsArticulation &DlsCollection::articulationFor(const DlsInstrument &ins, const DlsRegion &r) const
{
    static const DlsArticulation kDefault;
    int a = r.articulation >= 0 ? r.articulation : ins.articulation;
    return a >= 0 && size_t(a) < articulations.size() ? articulations[size_t(a)] : kDefault;
}

std::shared_ptr<DlsCollection> parseDls(const std::vector<uint8_t> &data, std::string *err)
{
    auto fail = [&](const char *why) -> std::shared_ptr<DlsCollection> {
        if (err) *err = why;
        return nullptr;
    };
    auto top = children(data.data(), data.size());
    if (top.empty() || !top[0].isList("DLS ")) return fail("not a DLS collection");
    auto col = std::make_shared<DlsCollection>();
    auto body = children(top[0]);

    // Waves: the pool, then the offset table that wave links index.
    std::vector<int> waveAtOffset;  // ptbl index -> wave index
    const Chunk *wvpl = nullptr;
    for (const Chunk &c : body)
        if (c.isList("wvpl")) wvpl = &c;
    if (!wvpl) return fail("no wave pool");
    std::vector<std::pair<size_t, int>> offsets;  // offset of each wave's LIST in the pool
    std::vector<const Chunk *> waveWsmp;          // each wave's own sample settings
    std::vector<std::vector<Chunk>> waveParts;
    {
        auto pool = children(*wvpl);
        waveParts.reserve(pool.size());
        for (const Chunk &w : pool) {
            if (!w.isList("wave")) continue;
            const size_t start = size_t(w.data - wvpl->data) - 12;  // the LIST header, from the pool's start
            waveParts.push_back(children(w));
            const std::vector<Chunk> &parts = waveParts.back();
            waveWsmp.push_back(find(parts, "wsmp"));
            const Chunk *fmt = find(parts, "fmt ");
            const Chunk *pcm = find(parts, "data");
            DlsWave wave;
            if (fmt && pcm && fmt->size >= 16) {
                uint16_t tag = u16(fmt->data), channels = u16(fmt->data + 2), bits = u16(fmt->data + 14);
                wave.rate = int(u32(fmt->data + 4));
                if (tag == 1 && channels >= 1) {
                    const size_t frame = size_t(channels) * (bits / 8);
                    for (size_t i = 0; frame && i + frame <= pcm->size; i += frame)
                        wave.samples.push_back(bits == 16 ? int16_t(u16(pcm->data + i))
                                                          : int16_t((int(pcm->data[i]) - 128) << 8));
                }
            }
            offsets.push_back({start, int(col->waves.size())});
            col->waves.push_back(std::move(wave));
        }
    }
    if (const Chunk *ptbl = find(body, "ptbl"); ptbl && ptbl->size >= 8) {
        uint32_t cb = u32(ptbl->data), n = u32(ptbl->data + 4);
        for (uint32_t i = 0; i < n && cb + (i + 1) * 4 <= ptbl->size; ++i) {
            uint32_t o = u32(ptbl->data + cb + i * 4);
            int idx = -1;
            for (auto &p : offsets)
                if (p.first == o) idx = p.second;
            waveAtOffset.push_back(idx);
        }
    } else {
        for (auto &p : offsets) waveAtOffset.push_back(p.second);
    }

    for (const Chunk &c : body) {
        if (!c.isList("lins")) continue;
        for (const Chunk &insChunk : children(c)) {
            if (!insChunk.isList("ins ")) continue;
            auto kids = children(insChunk);
            DlsInstrument ins;
            if (const Chunk *h = find(kids, "insh"); h && h->size >= 12) {
                ins.bank = u32(h->data + 4);
                ins.program = u32(h->data + 8);
            }
            for (const Chunk &k : kids)
                if (k.isList("INFO"))
                    if (const auto info = children(k); const Chunk *nm = find(info, "INAM"))
                        ins.name.assign(reinterpret_cast<const char *>(nm->data), strnlen(reinterpret_cast<const char *>(nm->data), nm->size));
            ins.articulation = addArticulation(*col, kids);
            for (const Chunk &k : kids) {
                if (!k.isList("lrgn")) continue;
                for (const Chunk &rc : children(k)) {
                    if (!rc.isList("rgn ")) continue;
                    auto rk = children(rc);
                    DlsRegion r;
                    if (const Chunk *h = find(rk, "rgnh"); h && h->size >= 8) {
                        r.keyLo = u16(h->data);
                        r.keyHi = u16(h->data + 2);
                        r.velLo = u16(h->data + 4);
                        r.velHi = u16(h->data + 6);
                        if (r.velHi == 0) r.velHi = 127;  // some tools leave velocity unset
                    }
                    uint32_t table = 0;
                    if (const Chunk *l = find(rk, "wlnk"); l && l->size >= 12) table = u32(l->data + 8);
                    r.wave = table < waveAtOffset.size() ? waveAtOffset[table] : -1;
                    // The region's own sample settings, else the wave's.
                    const Chunk *ws = find(rk, "wsmp");
                    if (!ws && r.wave >= 0) ws = waveWsmp[size_t(r.wave)];
                    if (ws) parseWsmp(*ws, r);
                    r.articulation = addArticulation(*col, rk);
                    if (r.wave >= 0) ins.regions.push_back(r);
                }
            }
            col->instruments.push_back(std::move(ins));
        }
    }
    if (col->instruments.empty()) return fail("no instruments");
    return col;
}

}  // namespace shogo
