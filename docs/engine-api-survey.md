# LithTech 1.0 engine API survey (Shogo v2.2)

Source: Monolith's `shogsrc22.zip`, `AppHeaders/` (30 files, ~7,400 lines).
The source itself lives in `third_party/shogsrc22/` and must not be redistributed (see EULA §8).

## How the game binds to the engine

The engine loads four game modules from `SHOGO.REZ` and calls a few C exports in each:

| Module | Built from | Export(s) | Engine hands it |
|---|---|---|---|
| `CSHELL.DLL` | `ClientShellDLL/` (188 files) | `GetClientShellFunctions`, `GetClientShellVersion`, `SetInstanceHandle` | `ClientDE*` |
| `OBJECT.LTO` | `ObjectDLL/` (262 files) | `ObjectDLLSetup(int *nDefs, ServerDE*, int *version)` → `ClassDef**` | `ServerDE*` |
| `CRES.DLL` / `SRES.DLL` | `ClientRes/`, `ServerRes/` | Win32 string resources only | — |

Everything after that goes through two big interface tables of function pointers, plus a set of callbacks going the other way.

## Engine → game callbacks

- **Client shell (`ClientShellDE`, 21 callbacks):** OnEngineInitialized/Term, OnCommandOn/Off, OnKeyDown/Up, OnMessage, OnModelKey, PreLoadWorld, OnEnterWorld/ExitWorld, SpecialEffectNotify, OnObjectRemove/Move/Rotate, Pre/Post/Update, SRand, DemoSerialize, OnEvent, OnTouchNotify.
- **Server shell (`ServerShellDE`, 15 callbacks):** OnAddClient/RemoveClient, OnClientEnterWorld/ExitWorld, Pre/PostStartWorld, OnMessage, OnObjectMessage, OnCommandOn/Off, Update, OnPlaybackFinish, CacheFiles, SRand, ServerAppMessageFn.
- **Object classes:** 326 `BEGIN_CLASS` definitions across 104 ObjectDLL files, plus 12 built-in engine classes in `engineobjects.c` (BaseClass, Model, WorldModel, Container, Sprite, Sound, StartPoint, OutsideDef, FastApproxArea, Light, ObjectLight, DirLight). The engine reads their property tables to spawn objects from the world `.DAT` files.

## Game → engine API (what we must implement)

| Interface | Function pointers | Virtual methods | Notes |
|---|---|---|---|
| `ClientDE` (`client_de.h`) | 236 | 21 | Rendering, 2D surfaces, sound, music, input, networking, client objects |
| `ServerDE` (`server_de.h`) | 168 | 38 | Object lifecycle, physics/collision, props, world loading, clients, save/load |
| `CSBase` (shared base) | — | 32 | Legacy message read/write |
| `CommonLT`, `PhysicsLT`, `LMessage` | — | 10 + 20 + ~20 | Newer-style helper interfaces |

**Roughly 400 function pointers plus ~120 virtual methods.** Many are small (getters/setters, string helpers, rotation math), and client and server share ~40 of them.

### ClientDE by area
- **Session / renderer control:** StartGame, Disconnect, Shutdown, Start3D/End3D, RenderCamera, RenderObjects, FlipScreen, render modes.
- **2D surfaces (~35):** create/delete surfaces, fonts, colors, blit/scale/warp/transform, string drawing. Used by all menus and the HUD.
- **Client-side objects (~60):** create/delete, position/rotation/scale/flags, cameras, particle systems, line systems, poly grids (water), lights, models and animation, containers.
- **Sound (~15) and music (~17):** PlaySound with 3D positioning, reverb; music is the IMA song/playlist/motif API.
- **Input (~12):** device bindings, axis offsets, device tracking.
- **Console vars and commands, timing, file lists, config files.**
- **Networking (~10):** DirectPlay-style services and sessions, lobby launch, TCP/IP address.
- **Video (4):** StartVideo/StopVideo/UpdateVideo/IsVideoPlaying (Smacker).

### ServerDE by area
- **Objects and classes (~40):** CreateObject(Props), RemoveObject, class lookup, links, attachments, lists, SetNextUpdate, deactivation.
- **Physics and collision (~25):** IntersectSegment, CastRay, Find*Touching*, TeleportObject, Get/SetObjectState, blocking priority, containers, plus `PhysicsLT` (MoveObject etc.).
- **Props (12):** GetProp* read object properties from the world file during spawn.
- **Models (~20):** node transforms, animation, command strings (`OnModelKey` drives footsteps and weapon events), filenames.
- **Clients (~15), console vars, sky, lighting, sound, save/restore, LoadWorld/RunWorld.**

## Portability of the game code itself

- No inline assembly and no file-system Win32 calls in the game code.
- **Win32 UI:** 124 `HWND`, 93 `MessageBox`, 12 `HINSTANCE`, 3 `GetAsyncKeyState`. Mostly error popups and window handles; replace with small shims.
- **MSVC string functions:** 115 uses of `stricmp`/`strupr`/`itoa` in 36 files; shim with macros.
- **DirectPlay lobby:** 1 file; stub out.
- **64-bit:** the API mostly uses `void*` for user data, and only one `(DWORD)pointer` cast was found. Handles (`HOBJECT`, `HMESSAGE...`) are opaque pointers. A native arm64 build looks feasible, but needs a real compile to confirm (struct packing and `DDWORD` sizes in save games and network messages are the risks).
- The code targets VC++ 6 (pre-standard C++). Expect a pass of fixes for modern Clang (`for`-scope, implicit `int`, missing includes).

## What this means for the port

1. The interface is fully specified by headers we have, so the new engine can be written against it directly, with no guessing about function signatures.
2. Behavior (what each call does, especially physics, collision and rendering) is not specified. That has to come from the file formats, the Tools help file (`stools22.zip`) and observing the original game.
3. A sensible first milestone: compile `CSHELL` and `OBJECT` as native macOS libraries against stub implementations of `ClientDE`/`ServerDE` that log every call. That proves the game code builds and shows which API calls are used first, in order.
