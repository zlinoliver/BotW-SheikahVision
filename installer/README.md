# SheikahVision Mac Installer

A Mac app that builds SheikahVision from your own Wii U copy of *The Legend of Zelda: Breath of the Wild*, signs it with your own Apple account and installs it on your Apple Vision Pro. Players: download it from [Releases](https://github.com/zlinoliver/BotW-SheikahVision/releases) and follow the [installer guide](../docs/INSTALLER.md). This page is for people who want to build or change the installer itself.

Use only your own copy of the game. The installer, this repository and the prebuilt runtime kit contain **no game code, assets or keys**; the only save is the author's optional experience save in the app (off unless the player turns it on). The C++ generated from your game stays on your Mac and must never be shared. Passwords are typed only into Xcode or macOS dialogs; the installer uploads nothing.

## What it does

Five steps, each checked automatically:

1. **Mac**: Apple silicon, Xcode 26 or later with the visionOS platform, the command-line tools and enough disk space.
2. **Apple account**: picks the team signed in to Xcode (a free account works; its apps expire after 7 days and are re-signed from the installer's home screen).
3. **Game files**: a decrypted game folder (`code`, `content`, `meta`), base game and v208 update together; they are verified against known hashes and merged.
4. **Vision Pro**: finds the paired headset (Developer Mode on).
5. **Install**: generates the C++ from your game, compiles it on all cores, signs, installs the app, then transfers the game to the headset, where the app imports it by itself.

A newer installer offers **Install new version**: it rebuilds with the new code and installs over the app; the game and saves on the headset are kept.

## Layout

| Path | What |
|---|---|
| `app/` | The SwiftUI app (Swift package `SheikahInstaller`), English and Simplified Chinese UI, the player guide (`Resources/guide-en.html`, `guide-zh.html`) |
| `engine/` | `sv.py`, the command-line engine the app drives (JSON events, one per line); `gameimport.py`, `runtimekit.py`; tests in `engine/tests` |
| `scripts/` | Building the runtime kit and the app, the release package, source bundling and checks |
| `cmake/` | The host link object and the runtime stamp used by `ios/CMakeLists.txt` for prebuilt runtime kits |

## Building

```sh
# the redistributable visionOS runtime kit (no game files or generated/ needed)
installer/scripts/prebuild_runtime.sh
# the Mac app, ad-hoc signed, with the tracked public source and the runtime kit bundled
installer/scripts/build_installer.sh
# run the UI against this checkout while developing
SV_SOURCE="$PWD" swift run --package-path installer/app SheikahInstaller
```

The engine can also be used on its own, for example:

```sh
xcrun python3 installer/engine/sv.py doctor --json
xcrun python3 installer/engine/sv.py build --team YOURTEAMID --json
xcrun python3 installer/engine/sv.py install --device DEVICE_UDID --json
```

`--work DIR` (default `~/Library/Application Support/SheikahVision`) holds the player's game copy, build cache, state and logs: private, never commit or share it. `--dry-run` prints the plan without changing anything.

## Tests

```sh
(cd installer/engine/tests && xcrun python3 -m unittest discover -s .)
swift test --package-path installer/app
```

Both use synthetic files and never touch a device.

## Releases

```sh
installer/scripts/prebuild_runtime.sh     # after the last commit: the kit is tied to the exact source
installer/scripts/release_installer.sh    # ad-hoc signed zip in build-installer/release/
```

The release app is ad-hoc signed (no Developer ID, not notarized), so it carries no team, company or certificate. The script refuses to package an app that mentions a team ID or name known to the Mac's Xcode, a keychain signing identity, or the local user's name or home folder. Each release is published on GitHub Releases with notes taken from [CHANGELOG.md](../docs/CHANGELOG.md).

Because the app is not notarized, players open it once with System Settings › Privacy & Security › **Open Anyway** (see the [installer guide](../docs/INSTALLER.md)).

## Credits

The installer is modelled on [Glovebox](https://github.com/glovebox-vr/glovebox)'s SwiftUI installer: its card layout, glass fallback, step marks and progress ring, and in the engine its doctor checks, team discovery, signing warm-up and JSON event contract (MIT license, see [Glovebox-NOTICE.md](Glovebox-NOTICE.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
