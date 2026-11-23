# Contributing

Thanks for wanting to help! Bug reports and fixes are very welcome.

## What this project is (and isn't)

Shogo native targets **macOS on Apple Silicon only**. It draws with Metal and uses
other macOS frameworks (Core Text, ImageIO) directly, and that focus is deliberate:
one platform, done well.

- **Welcome:** bug fixes, things that play differently from the original game,
  performance, missing engine features, and better Mac integration.
- **Not accepted:** ports to Windows, Linux or other platforms, Intel Mac builds,
  and changes that add cross-platform layers. Pull requests like these will be closed,
  however well made. You're very welcome to fork the project and take it there
  yourself.

If you're unsure whether something fits, open an issue first and ask.

## Reporting a bug

Say which level you were on, what you did, and what happened compared to the original
game. A screenshot or a save game helps a lot.

## Code rules

- **Never commit Monolith's source code, the game's data, or anything copied from
  them.** Monolith's licence forbids sharing its source; changes to the game code go
  in `patches/shogo-native.hpatch`, made with `tools/update_patch.sh`, which checks
  that it quotes none of it. See the [developer notes](docs/development.md).
- **Don't use leaked LithTech engine source** or code derived from it. The engine
  here is written from the game's data and the released game source only.
- Run the tests (`ctest --test-dir build`) before sending a pull request.
