# Changes

Each release's section here becomes its release notes on GitHub, so write it for
players. The Release workflow refuses to publish a version that has no section.

## 0.3.0

- **The game moves to `~/Games/Shogo`.** The imported game now sits in `~/Games/Shogo`.
  Your saves and settings stay where they were, in
  `~/Library/Application Support/Shogo Recomp/Game`. The first time you open the new
  version, the launcher moves an existing import there for you.
- **Uninstall.** A new **Uninstall…** button in the launcher removes the game from
  `~/Games/Shogo` after asking first. Your saves and settings are kept, so they're
  there again when you re-import.

## 0.2.2

- **App icon.** Shogo now has its own icon, so Finder, Spotlight and launchers like
  Raycast no longer show a blank one. It's original art, not the game's. While you
  play, the Dock still shows the game's own icon.

## 0.2.1

- **Opens without a warning.** The app is now signed by its developer and notarized
  by Apple, so macOS no longer says it "couldn't verify" Shogo the first time you
  open it.

## 0.2.0

- **Import from Steam.** Steam's copy of Shogo is Windows-only, so the launcher
  shows how to download it with SteamCMD (installed with Homebrew), with buttons
  that copy each Terminal command. Then pick the downloaded folder to import it.
- **Pick or drop either copy.** Import from GOG asks for the GOG installer, and
  Import from Steam asks for the game folder. You can also drag either onto the
  launcher.
- **WASD by default.** New players start with WASD movement and mouse look.
  Importing again keeps your own key bindings and saves.
- **Multiplayer says it isn't ready yet**, instead of opening menus that can't
  connect.

## 0.1.0

- **First release.** Shogo's whole single-player campaign runs natively on Apple
  Silicon. That includes the levels, cutscenes, music and sound, the intro movies,
  and saving. The game runs at your Mac's native resolution, or in the Original look.
- **Import from GOG.** The launcher reads the game straight out of GOG's installer,
  without running any Windows code.
