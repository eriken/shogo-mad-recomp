# Shogo: Mobile Armor Division — native for Mac

Play Monolith's 1998 anime mech shooter on a modern Mac. No Windows, no emulator, no
virtual machine: Shogo runs natively on Apple Silicon, with sharp high-resolution graphics,
full music and sound, and the original campaign from start to finish.

You bring your own copy of the game (from GOG); this project gives it a new engine to run on.

## What you get

- **The whole single-player campaign.** All the levels, cutscenes, briefings, weapons and
  mechs, on foot and in the MCA.
- **Crisp, modern graphics.** Play at your Mac's native resolution in a window or full
  screen, with smooth textures and anti-aliased edges, or switch to Original for the
  authentic 1998 look.
- **The real soundtrack.** Shogo's interactive music, which shifts between calm and
  combat as the action heats up, plus all the sound effects in 3D.
- **The intro movies** — or skip them, your call.
- **Saving that just works.** Quick save and quick load, plus the full save/load menus.
- **Comfortable controls.** Mouse look, always-run, a dedicated reload key, the mouse wheel
  as a bindable button, and every key rebindable.

## What you need

- A Mac with Apple Silicon (M1 or newer).
- Shogo from [GOG](https://www.gog.com/game/shogo_mobile_armor_division). You only need
  the Windows installer file (`setup_shogo_-_mobile_armor_division_….exe`), downloaded from
  your GOG library — you don't need a PC to run it.

The Steam version isn't supported yet.

**Mac only, on purpose.** This project is about making Shogo run as well as possible on
Apple Silicon Macs, so it uses Metal and other macOS features directly. Windows and Linux
versions aren't planned, and pull requests adding them won't be merged, but you're welcome
to fork the project and take it there yourself.

## Getting started

1. **Open `Shogo.app`.** The launcher appears.
2. **Import your game.** Click **Import from GOG installer…** and pick the GOG installer
   (or just drag it onto the window). It takes a few seconds; the launcher reads the game
   straight out of the installer and checks every file, without running any Windows code.
3. **Press Play.** That's it.

Importing again later (say, after re-downloading from GOG) keeps your saves and key bindings.

> Right now you build `Shogo.app` yourself — there's no ready-made download yet. See
> [Building it](#building-it) below.

## Settings

Graphics options are in the game under **Options > Display**:

| Setting | What it does |
|---|---|
| **Resolution: Original** | A small window at the game's original resolution, for the classic look |
| **Resolution: Enhanced / Enhanced 2x** | A bigger window (about half or most of your screen), drawn at full sharpness |
| **Resolution: Native** | Full screen at your display's full resolution |
| **Intro movies** | Play or skip the Monolith logo and intro on startup |

Changes apply immediately.

## Controls

The defaults are the original game's, with a couple of comforts added. Every key can be
changed in the game's **Options** menu — WASD fans, that's where to go.

| Action | Key |
|---|---|
| Move / turn | Arrow keys |
| Look | Mouse |
| Fire | Left mouse button / Ctrl |
| Jump | Space |
| Crouch | C |
| Strafe left / right | , / . |
| Pick a weapon | 1–0, or [ and ] for previous / next |
| Reload | R |
| Walk (you run by default) | Hold Shift |
| Quick save / quick load | F6 / F7 |
| Skip a movie | Escape |

## What's not there yet

- **Multiplayer.** Single-player only for now.
- **CD music on three levels.** A few levels originally played a track from the game CD;
  those play without music.
- Saves from the original Windows game can't be loaded — start a fresh campaign.

Found a bug? Open an issue and say which level you were on and what happened.

## Building it

You'll need [Homebrew](https://brew.sh), Xcode's command-line tools, and one extra
download: Monolith's own released Shogo source code (`shogsrc22.zip`, on ModDB as
"Shogo Mobile Armor Division Source Code"). Monolith's licence lets Shogo owners use it
but not share it, so it isn't included here.

```bash
brew install cmake sdl3
```

Then, in this folder, unpack the source and build:

```bash
mkdir -p third_party/shogsrc22/installer
unzip shogo_sourcecode22.zip -d third_party/shogsrc22/installer
python3 -I tools/is3extract.py third_party/shogsrc22/installer/data.z third_party/shogsrc22/source
cmake -S . -B build
cmake --build build -j
```

The finished app is `build/Shogo.app`. Developer tools, tests and how the engine works are
covered in the [developer notes](docs/development.md).

## Credits and legal stuff

Made by Erik Wessel — shogo-recomp.unlocking364@passmail.net

**THIS PORT IS NOT MADE BY OR SUPPORTED BY Monolith Productions, or any of its affiliates
and subsidiaries.**

This is a free fan project. The repository contains only its own code: no game files, art,
music or Monolith source. The game's data is read from your own copy at runtime and never
shipped with the app. The app includes a build of the game code from Monolith's released
Shogo source, under that source's licence; it's free and only works with the full game.
