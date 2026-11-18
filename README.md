# Shogo native (macOS)

A native macOS / Apple Silicon port of *Shogo: Mobile Armor Division* (Monolith, 1998):
a new LithTech 1.0–compatible engine, running Monolith's own released game code.

This repository contains **only our own code**. You supply two things yourself:

1. **The game data** from your own copy of the game (the GOG installer works).
2. **Monolith's Shogo v2.2 game source** (`shogsrc22.zip`). Its EULA allows owners of
   Shogo to use and modify it, but forbids redistributing it, so it is never committed here
   (see `.gitignore`).

## Status

| Piece | State |
|---|---|
| Server game code (`OBJECT.LTO`, 129 files) | Builds natively for arm64; registers all 343 object classes |
| Client game code (`CSHELL.DLL`, 89 files) | Builds natively for arm64; front-end menus work, and **New Game** loads the first level |
| Engine: resources | `.REZ` archives (with patch-archive layering) and the string/`TEXT` resources in `CRES.DLL`/`SRES.DLL`, read as data |
| Engine: strings | Real `HSTRING` API on client and server: ref counting, `FormatString` with `%1!s!` inserts |
| Engine: 2D | Software 32-bit surfaces: PCX loading, copy/color-key/solid-color/scaled/warped/rotated/masked blits, fills, pixels, border detection (keys compared at RGB565 precision) |
| Engine: fonts | System fonts via Core Text, rasterized without anti-aliasing like GDI |
| Engine: display | SDL3 window, letterboxed nearest-neighbour scaling of the 640x480 screen; keyboard mapped to Windows VK codes |
| Engine: worlds | All 61 levels load: BSP geometry, world models, level objects, DTX/SPR textures, sky boxes ([format notes](docs/world-format.md)) |
| Engine: 3D | Software renderer: perspective-correct textured polygons, baked lightmaps (bilinear, 20-unit samples) on lightmapped surfaces and baked vertex light on the rest, z-buffer, sky box; world models (doors, lifts, fans, glass) drawn where their objects are, hidden ones left out; `shogo-worldview` flies through any level |
| Engine: objects | Server object system: the game's classes create the level objects from the world file (properties, `MID_PRECREATE`/`INITIALUPDATE`/`UPDATE`, object messages, links, removal); spawn strings via `CreateObjectProps` |
| Engine: game flow | `StartGame` with a local server, client/server and object messages, special-effect messages, console variables (`+runworld`), client objects and cameras, `RenderCamera` from the player's camera, `Physics()`/`Common()` interfaces |
| Engine: input | The game's key bindings (`autoexec.cfg`: `AddAction`, `rangebind`, `scale`), `IsCommandOn`, mouse look (relative mouse while playing) |
| Engine: movement | Collision against level geometry, world models and solid objects (swept ellipsoids, collide and slide, stair steps), gravity, standing-on, touch notifications (with `GetLastCollision`), ray casts that name the polygon hit and its surface type, inside/outside point tests on the BSP (`GetPointStatus`), object searches in a sphere; the player walks, runs and jumps |
| Engine: models | `.ABC` models ([format notes](docs/abc-format.md)) drawn textured and animated (node hierarchy, interpolated keyframes, vertex animation), first-person weapons over everything; user dims, string keys, node transforms, hidden nodes, attachments; simple fixed lighting for now |
| Engine: effects | Sprites (`.SPR` animations, camera-facing or rotatable, additive or color-keyed, shaped by the DTX alpha mask), particle systems (the game's `DEParticle` lists, gravity, lifetimes) and line systems; muzzle flashes, impacts, smoke, shell casings. Particle size is a guess (`kParticleWorldScale`) |
| Engine: lights | Dynamic lights (`OT_LIGHT` objects: muzzle flashes, projectiles, explosions, flickering level lights) add light per pixel to the world polygons they reach and per vertex to models; `SetGlobalLightScale` tints the baked light, `SetCameraLightAdd` tints the screen (damage flashes) |
| Engine: sound | Sound effects from `SOUND.REZ` (all PCM WAV): a software mixer at 44.1 kHz stereo with local, ambient and 3D sounds (inner/outer radius falloff, left/right pan), loops, sounds attached to objects, handles (`KillSound`, `IsSoundDone`...), volume, a soft limiter; SDL3 output in the game, `--audio-out` in the probe. No reverb or doppler |
| Engine: saves | Quick save/load (F6/F7), the save and load menus, the level-start reload save, and level changes that carry the player over (`SaveObjects`/`RestoreObjects` in our own file format, object references remapped on load, inter-object links, client references so the player is put back into its saved object, the game console's per-level settings). Shogo's original `.sav` files aren't read; portal states aren't saved |
| Engine: not yet | World lighting on models (they use a fixed light plus dynamic lights), music (DirectMusic data: DLS banks, styles, segments; the game is told there's no music driver), Smacker movies (intros are skipped), networking |
| Engine: everything else | Logging stubs (every `ClientDE`/`ServerDE` entry point), generated from the API headers |

`ctest --test-dir build` runs the engine unit tests (game-data tests use `gamedata/`).

Run `shogo-probe` (below) to see which engine functions the game calls, in order.
Environment switches print more: `SHOGO_TRACE_PLAYER` (player and movement object per
frame, probe only), `SHOGO_TRACE_MOVES` (each client move and what it hit),
`SHOGO_TRACE_INPUT`, `SHOGO_TRACE_PHYSICS`, `SHOGO_TRACE_ANIM`, `SHOGO_TRACE_MESSAGES`,
`SHOGO_TRACE_SAVE` (each save and restore), `SHOGO_TRACE_CAMERA` (camera position, light scale and polygons
drawn, every 30 frames), `SHOGO_TRACE_FX` (sprites, particle systems and special-effect messages),
`SHOGO_TRACE_MODELS` (each model's skin and color once), `SHOGO_TRACE_LIGHTS` (each light once, lit
polygon counts), `SHOGO_TRACE_SOUND` (each sound started, and files that don't load), `SHOGO_TRACE_WORLDMODELS`
(each world model moving, returning home or hiding), `SHOGO_TRACE_ALL` (every stub call). `SHOGO_NOMODELS`, `SHOGO_NOEFFECTS`,
`SHOGO_NOLIGHTS`, `SHOGO_NOLIGHTMAPS` and `SHOGO_NOPLACEMENTS` skip models, effects, dynamic lights,
lightmaps (vertex light instead) or world-model movement (any value, even empty, counts as set).

## Setup

```bash
# 1. Game data: extract your GOG installer (needs innoextract: brew install innoextract)
mkdir gamedata && innoextract -s -d gamedata setup_shogo_-_mobile_armor_division_*.exe

# 2. Game source: download shogsrc22.zip (ModDB, "Shogo Mobile Armor Division Source Code",
#    MD5 2c13c09931bc803f3af6e6782ec2df80), then unpack its InstallShield 3 archive
#    without running the 1999 installer:
mkdir -p third_party/shogsrc22/installer
unzip shogo_sourcecode22.zip -d third_party/shogsrc22/installer
python3 -I tools/is3extract.py third_party/shogsrc22/installer/data.z third_party/shogsrc22/source

# 3. Build (needs SDL3: brew install sdl3)
cmake -S . -B build
cmake --build build -j

# 4. Play (as far as it goes): opens a window at the main menu
cd gamedata && ../build/shogo

# Fly through a level (WASD + mouse, Q/E down/up, Shift fast), or take a screenshot
cd gamedata && ../build/shogo-worldview WORLDS/02_QUARTERS.DAT
cd gamedata && ../build/shogo-worldview WORLDS/02_QUARTERS.DAT --screenshot /tmp/q.png --yaw 90

# Start straight in a level (arrow keys move, mouse looks, Space jumps, Ctrl fires;
# the level's opening cutscene runs first)
cd gamedata && ../build/shogo +runworld "Worlds\01_Ambush"

# Look at a model: animation list, or a posed, skinned render
cd gamedata && ../build/shogo-modelview "Models\Player\sanjuro.abc" --list
cd gamedata && ../build/shogo-modelview "Models\Player\sanjuro.abc" --skin "Skins\Player\sanjuro.dtx" \
    --anim RR --time 200 --yaw 30 --out /tmp/sanjuro.png

# Or run the game headless: N frames, optional key presses (VK code @ frame), and a screenshot
cd gamedata && ../build/shogo-probe --frames 40 --key 0x28@5 --key 0x0D@10 \
    --screenshot /tmp/menu.png ../build/object.lto.dylib ../build/cshell.dylib

# New Game headless: single player, normal, skip the intro, dismiss the briefing
cd gamedata && ../build/shogo-probe --frames 160 --key 0x0D@10 --key 0x0D@20 --key 0x0D@30 \
    --key 0x1B@40 --key 0x20@100 --screenshot /tmp/ambush.png ../build/object.lto.dylib ../build/cshell.dylib

# Walk: in Quarters the cutscene hands over control around frame 650; hold the up
# arrow (VK 0x26), turn with the mouse (pixels per frame), print the player per frame
cd gamedata && SHOGO_TRACE_PLAYER=1 ../build/shogo-probe +runworld "Worlds\02_Quarters" --frames 800 \
    --hold 0x26@700-790 --mouse 10,0@740-760 ../build/object.lto.dylib ../build/cshell.dylib

# Record what the player hears to a WAV file (the cutscene, then gunfire)
cd gamedata && ../build/shogo-probe +runworld "Worlds\02_Quarters" --frames 780 --hold 0x11@700-740 \
    --audio-out /tmp/quarters.wav ../build/object.lto.dylib ../build/cshell.dylib

# Fire (left Ctrl, VK 0x11) at the wall in Quarters and look at the muzzle flash and impact
cd gamedata && ../build/shogo-probe +runworld "Worlds\02_Quarters" --frames 712 --hold 0x11@700-712 \
    --screenshot /tmp/fire.png ../build/object.lto.dylib ../build/cshell.dylib

# Quick save (F6), walk and fire, quick load (F7): back where you saved, ammo restored
cd gamedata && SHOGO_TRACE_SAVE=1 ../build/shogo-probe +runworld "Worlds\02_Quarters" --frames 960 \
    --hold 0x75@750-754 --hold 0x26@760-830 --hold 0x11@770-800 --hold 0x76@850-854 \
    --screenshot /tmp/loaded.png ../build/object.lto.dylib ../build/cshell.dylib

# Fire the level's exit trigger: the player and their weapons go on to 03_MCA_Dock (Space skips the text)
cd gamedata && ../build/shogo-probe +runworld "Worlds\02_Quarters" --frames 1700 --trigger exit1b@800 \
    --hold 0x20@950-953 --screenshot /tmp/next.png ../build/object.lto.dylib ../build/cshell.dylib
```

Keys are Windows VK codes; hold them (`--hold`) for a few frames, since the game polls its
commands once a frame and a `--key` press is released within the same frame.

## How the game source is built

The pristine source in `third_party/` is never edited. At configure time
`tools/prepare_source.py` copies it to `build/gamesrc/`, normalizes line endings and
`#include` paths, and applies `patches/shogo-native.patch`.

To change game code: edit files under `build/gamesrc/`, then run `tools/update_patch.sh`
to regenerate the patch **before** re-running CMake (configuring regenerates `build/gamesrc/`).

Layers, from least to most invasive:

- `compat/` — Win32/MSVC stand-ins (`windows.h`, `mbstring.h`, INI files, timers, …)
  force-included into every game file.
- `src/game/` — our own native replacements for the few Win32-only glue files
  (multiplayer wizard, `WinUtil`, `KeyFixes`, `CreditsWin`). The originals are left out of the build.
- `patches/shogo-native.patch` — small source fixes: VC6 for-loop scoping, string-literal
  `const`, implicit `int`, and 64-bit pointer handling. **Not in this repository yet:** a
  diff's context lines are Monolith's source, which their license doesn't allow sharing,
  so a fresh clone can't build the game code until the fixes are published another way.

### 64-bit notes

- API types that were `unsigned long` (4 bytes on Windows) are pinned to 32 bits, so file
  formats, save games and network messages keep their original layout.
- Pointer↔integer truncation is a compile error. Where the game sends a pointer through a
  32-bit message field (dialog queues, the in-game dialog's object handle, the yes/no box
  callback), it goes through a per-module token table (`compat/include/shogo_ptrtoken.h`).
- The engine is compiled with `DIRECTENGINE_COMPILE`, the headers' engine-side view, where the
  slot that skips a game object's C++ vtable is a pointer-sized `void*`.

## Tools

| Tool | Purpose |
|---|---|
| `tools/is3extract.py` | Extract InstallShield 3 `.Z` archives (used for `shogsrc22.zip`) |
| `tools/rezlist.py` | List/extract LithTech `.REZ` resource archives |
| `tools/prepare_source.py` | Copy + normalize + patch the game source into the build tree |
| `tools/update_patch.sh` | Regenerate `patches/shogo-native.patch` from edits in `build/gamesrc/` |
| `tools/fix_forscope.py` | Fix VC6 for-loop variable scoping from Clang error output |
| `tools/gen_engine_stubs.py` | Generate the stub engine from Clang's AST of the API headers |

## Engine layout

| File | Purpose |
|---|---|
| `engine/api.h` | The API headers in engine-side mode (`DIRECTENGINE_COMPILE`) |
| `engine/rez_file.*` | `.REZ` archive reader and layered `ResourceFS` |
| `engine/pe_resources.*` | Reads string tables and named resources from the game's resource DLLs as data |
| `engine/strings.*` | `HSTRING` objects and `FormatString` |
| `engine/stub_support.*` + generated `engine_stubs.cpp` | Logging stubs for everything not yet implemented |
| `engine/surfaces.*` | Software surfaces, PCX decoder, blits |
| `engine/fonts.h`, `engine/fonts_coretext.cpp` | `HDEFONT` fonts via Core Text |
| `engine/client_api.*` | Real `ClientDE` functions (2D, frame, render modes, time, lifecycle) installed over the stubs |
| `engine/game_host.*` | Loads the game modules and drives the client shell (shared by `shogo` and `shogo-probe`) |
| `engine/server.*` | The server: class registry, object creation from world files, `ServerDE` functions, update loop, messages |
| `engine/objects.*` | Engine objects (`HOBJECT`/`HLOCALOBJ`), shared by client and server |
| `engine/messages.*` | `HMESSAGEWRITE`/`HMESSAGEREAD` and the CSBase message functions |
| `engine/client_game.*` | The client's game session: `StartGame`, message delivery, client objects, cameras, `RenderCamera` |
| `engine/interfaces.*` | `Physics()` and `Common()` interfaces |
| `engine/input.*` | Key/mouse bindings and command state |
| `engine/collision.*` | Solid world triangles in a bounding-volume tree; swept-ellipsoid collide and slide |
| `engine/movement.*` | `MoveObject`, gravity, standing-on, touches, ray queries |
| `engine/sound.*` | WAV decoding, the sound mixer, the client and server sound API |
| `engine/effects.*` | Sprites, particle systems and line systems: their state, the client API, drawing |
| `engine/model.*` | `.ABC` models: parsing, posing, animation playback, node transforms, attachments |
| `engine/math3d.*`, `engine/api_common.*` | Quaternion math; rotation helpers and `Parse` shared by both sides |
| `engine/world_dat.*` | `.DAT` world reader (objects, BSPs, world models, lightmaps) |
| `engine/savegame.*` | Saved games and level changes: `SaveObjects`, `RestoreObjects`, client references |
| `engine/dtx.*` | DTX texture decoder |
| `engine/render_world.*` | Software world renderer and sky box |
| `engine/modelview_main.cpp` | `shogo-modelview`: renders one model to a PNG |
| `engine/worldview_main.cpp`, `engine/worldinfo_main.cpp` | `shogo-worldview` (viewer) and `shogo-worldinfo` (parses every level) |
| `engine/image_write*` | PNG screenshots (ImageIO) |
| `engine/shogo_main.cpp` | `shogo`: the game in an SDL3 window |
| `engine/probe_main.cpp` | `shogo-probe`: headless frames, scripted keys, screenshots, call log |

See `docs/engine-api-survey.md` for the engine API overview.
