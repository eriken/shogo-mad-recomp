# Music formats (Microsoft Interactive Music Architecture, as used by Shogo)

Shogo's music is IMA data, the 1997 predecessor of DirectMusic, played by
`IMUSIC25.DLL` and friends on Windows. The engine reads it with its own code:
`engine/dls.cpp` (instruments), `engine/music.cpp` (styles, sections, the
sequencer and synth). The layouts below were worked out from the game's own
files (`Music/*/`: 6 soundtracks, 52 styles, 221 sections, 6 DLS banks) and
from how the game's `Music.cpp` drives the engine. Everything is RIFF, little
endian; names are UTF-16, 16 characters, zero padded. `shogo-music --info`
prints what the engine reads.

Fields marked `?` are present but not understood; the engine ignores them.

## How the game uses it

Each level's WorldProperties object sets server console variables:

```
MusicDirectory   music\RedRiot
InstrumentFiles  redriot.dls redinit.sty
AmbientList      a1.sec a2.sec a2.sec a3.sec ...
CruisingList     c1.sec c2.sec ...
HarddrivingList  ...
```

The client (`CMusic`) loads the DLS bank, builds playlists from the lists
(plus `SilenceList` = `s.sec`) and loads the transition sections
(`sta.sec` silence→ambient, `atc.sec` ambient→cruising, and so on). The
server sends a music level (silence, ambient, cruising, hard driving) as the
action changes; the client plays the new list with the matching transition
first: `PlayList(list, transition, loop, boundary)`, at once from silence,
otherwise when the current section ends. Motifs and breaks are never used.

A section is one musical phrase: it names a style and a groove level, and
plays the style's pattern for that groove, looped over its length. The
composers set it up so each section selects exactly one pattern (section
`a2.sec` asks for groove B; style `akuma1`'s groove-B pattern is named `a2`).

## Section (`.SEC`): RIFF `AASE`

```
secn  u32   position in the composer's song (clocks; unused for playback)
      name  section name
      u16   tempo (beats per minute)
      u16   ?  0
      u16   length in measures
      u16   clocks per measure (768)
      u16   clocks per beat (192)
      ...   ?
      GUID  the style's
sref  name  style name (matches the style's styl name, not its file name)
prnm  name  personality ("Silence" everywhere)
RIFF AABN   band (below)
chrd  ...   chords: every section has C major on a C major scale
            (chord bits 0x91, scale bits 0xAB5AB5, root 12)
cmnd  u16   record size (8), then records:
      u32   time
      u32   groove bits: 0x80 groove A, 0x100 B, 0x200 C, 0x400 D
            (every section has one command, at time 0)
mute  u16   record size (8), then per beat: u32 time, u32 mute bits (all 0)
```

## Style (`.STY`): RIFF `AASY`

```
styl  name, u16 beats per measure, u16 ?, u16 clocks per grid step,
      u16 clocks per beat, u16 clocks per measure, u16 tempo, u16 ?, GUID
pref  personality reference
RIFF AABN   the default band
RIFF AAPT   one per pattern
```

### Pattern: RIFF `AAPT`

```
patt  u32   ?
      u32   length (clocks)
      u32   groove bits it plays at (as in cmnd), with 0x1C set
      u16   clocks per grid step (48, or 24 in some KumoB styles)
      u16   grid steps per beat
      u16   clocks per beat (192)
      u16   length in measures
      name
      u32   reference scale bits (0xAB5AB5, C major)
      u32   reference chord bits (0x891, Cmaj7)
      u8[16] highest note per part
      u8[16] lowest note per part
      ...   ? (per-part and per-variation settings, mostly 0xFF)
LIST AACL   one per grid step that has notes
  clik  u16   grid step
  note  u16   record size (18), then records:
        u8    9 (note)
        u8    part (0-15)
        i16   time offset from the grid step (clocks)
        u16   variation bits: the note plays in variation n if bit n is set
        u16   ?
        u8    MIDI key
        u8    velocity
        u16   ?
        u16   duration (clocks)
        u8[4] ? (04 00 07 00/87)
```

Keys are plain MIDI notes. Since every chord is the pattern's own key (C), no
note needs moving to fit a chord. Each part picks one of the variations it has
notes in, at random, every time the pattern starts.

## Band: RIFF `AABN` / `band`

```
name
...      ?
u8[16]   program per part (a DLS instrument in the soundtrack's bank, 0 = none)  at 40
u8[16]   volume per part (MIDI 0-127)                                            at 56
u8[16]   pan per part (0-127, 64 centre)                                         at 72
...      ?, the DLS collection's name ("InForm")
```

Part n plays on channel n.

## Instruments (`.DLS`)

Standard DLS level 1. Shogo's banks use 16-bit mono 22 kHz samples, forward
loops, melodic instruments only (the drum kits are ordinary instruments with
a region per key), and these articulation connections: EG1 attack, decay,
sustain and release; pan; LFO frequency and delay, LFO to pitch and to
volume. Destination 2 (reserved in DLS 1) repeats the pan in other units and
is ignored. Sample attenuation is in 1/655360 dB.
