# Building SheikahVision

[简体中文](BUILDING.zh-CN.md)

This guide builds SheikahVision from source with **your own** game files, signs it with **your own** Apple account, and installs it on **your own** Apple Vision Pro. Nothing from the game is downloaded or shared at any step.

> [!IMPORTANT]
> You need your own legally purchased Wii U copy of *The Legend of Zelda: Breath of the Wild* with update v208, dumped by yourself. This project provides no game files, keys or prebuilt apps, and does not help with obtaining them.

## Overview

| Step | What happens | Time (first run, M-series Mac) |
|---|---|---|
| 1. Prepare the Mac | Xcode, Homebrew packages | — |
| 2. Prepare the game files | Dump your own copy, combine base game and update, check the hash | — |
| 3. Recompile | `U-King.rpx` → C++ in `generated/` | about 1 minute |
| 4. (Optional) Mac build | Play / test on the Mac, also in VR mode | about 10 minutes |
| 5. Configure signing | Your team and a bundle ID of your own | — |
| 6. Build for Vision Pro | Dependencies, the app, signing | about 15 minutes |
| 7. Install and add the game | App over the network or cable, game files onto the headset | game copy: 10–30 minutes |

## 1. Prepare the Mac

- A Mac with **Apple Silicon**.
- **Xcode 26 or newer** from the App Store, with the **visionOS** platform installed (Xcode › Settings › Components).
- [Homebrew](https://brew.sh), then:

```bash
brew install cmake ninja uv boost glm sdl3 fmt openssl@3 zstd libpng
```

(`sdl3`, `fmt`, `openssl@3`, `zstd` and `libpng` are only needed for the Mac build; the Vision Pro build downloads and builds its own SDL3, fmt and zstd.)

Clone the repository:

```bash
git clone https://github.com/zlinoliver/BotW-SheikahVision.git
cd BotW-SheikahVision
```

## 2. Prepare the game files

SheikahVision needs the game as an **extracted, decrypted game folder** made from your own copy. Disc images are not used.

> [!NOTE]
> You are responsible for complying with the laws of your country. Console modification and dumping tools are not covered here, and are not discussed in this project's issues or discussions.

### What you need

- Your own Wii U and your own copy of *Breath of the Wild* (disc or eShop).
- The game **updated to v208 on the console** (shown as version 1.5.0 in the game). The update is downloaded by the console; it is not on the disc.
- No DLC and no save data are needed.

### Dump your own copy

From your own console, dump two titles as decrypted files, in the format Cemu uses (a folder holding `code/`, `content/` and `meta/`):

1. the **base game**, and
2. its **update** (v208).

Folder names differ between dumping tools; what matters is that each dump holds `code/`, `content/` and `meta/`.

### Already playing it in Cemu?

If you already play *Breath of the Wild* in Cemu on a computer, you most likely have both parts already:

| Part | Where Cemu keeps it |
|---|---|
| Base game | The game folder you added to Cemu's game list (it holds `code/`, `content/` and `meta/`). If you installed the game into Cemu instead, it is in `mlc01/usr/title/00050000/<title>` |
| Update (v208) | `mlc01/usr/title/0005000e/<title>` |

`<title>` is `101c9300` (Japan), `101c9400` (USA) or `101c9500` (Europe). Cemu shows where `mlc01` is under **Options › General settings › General › Custom MLC path** (called MLC path in older versions); on a Mac it defaults to `~/Library/Application Support/Cemu/mlc01`. The game list shows the installed update's version (it should be 208).

Copy both folders out (do not move them, Cemu still needs them), then combine them as below.

### Combine the dumps into one folder

| Dump | What it contains | What to do |
|---|---|---|
| Base game | Complete `code/`, `content/`, `meta/` | Start from this |
| Update (v208) | The same three folders, with only the changed files, including the v208 `U-King.rpx` | Copy over the base game, replacing files with the same name |
| DLC (if you dumped it) | A separate dump | Not needed; leave it out |

The result is one folder with exactly these three folders inside:

```
game/
├── code/      the executable U-King.rpx (v208), plus app.xml and cos.xml
├── content/   the game's data, about 12 GB
└── meta/      meta.xml: title ID and version, used to recognise the region and the update
```

### Check it

```bash
shasum -a 256 /path/to/game/code/U-King.rpx
# ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30
```

The recompiler and the game patches are made for exactly this file. Supported copies:

| Region | Title ID | Status |
|---|---|---|
| Japan | `00050000101C9300` | Tested on a real copy |
| USA | `00050000101C9400` | Tested on a real copy |
| Europe | `00050000101C9500` | Supported, not tested yet |

### Where the folder goes

- **On the Mac** (step 3 and the Mac build): the scripts look for a folder named `game` next to this repository. If yours is elsewhere, set `BOTW_GAME=/path/to/game`.
- **On the Vision Pro** (step 7): copy it from the Mac with `device.sh copy-game`, or import it in the launcher, as the folder or as a **zip** of it. In the zip, `code/`, `content/` and `meta/` can be at the top level or inside one folder. A zip made with "store only, no compression" imports fastest. The launcher checks every file against known hashes.

### Not supported

- **WUD / WUX disc images.** They are encrypted images of the whole disc, and the v208 update is not on the disc anyway. Use a decrypted dump of the base game and the update instead.
- Other packaged formats (for example WUA archives).
- A base game without the v208 update, or a v208 update without the base game.

### If the launcher rejects the files

| Message | Cause |
|---|---|
| Not a supported Wii U Breath of the Wild title | The title ID in `meta/meta.xml` is not one of the three above |
| This is not v208 / U-King.rpx does not match v208 | The update was not copied over the base game, or it is not v208 |
| Required game files are missing | One of `code/U-King.rpx`, `code/app.xml`, `code/cos.xml`, `content/` or `meta/meta.xml` is missing, often because the zip holds one folder level too many |
| Region and language packs do not match | Files from different regions were mixed |
| DLC detected and ignored | Harmless: DLC is not supported yet |

## 3. Recompile the game to C++

```bash
uv run python -I tools/recompile.py /path/to/game/code/U-King.rpx generated
```

This writes `generated/` (about 700 MB of C++). It is derived from the game and **must never be committed or shared**; `.gitignore` already excludes it. Run this again whenever you pull changes that add or remove a game hook (`BOTW_HOOK` / `BOTW_REPLACE` in `runtime/src/game/`).

## 4. (Optional) Build and run on the Mac

Useful for development and testing without a headset.

```bash
cmake -B build -G Ninja && ninja -C build
BOTW_GAME=/path/to/game tools/play.sh            # flat, windowed (F11: fullscreen)
BOTW_VR=1 BOTW_GAME=/path/to/game tools/play.sh  # VR mode: both eyes side by side
```

Saves and settings live in `~/Library/Application Support/SheikahVision`. To test from a save without touching your real one, see `tools/test_save.sh` (point `BOTW_TEST_SAVE` at a folder with a copy of your own save).

## 5. Configure signing (your team, your bundle ID)

1. Sign in to Xcode with your Apple account: **Xcode › Settings › Accounts**. A free Personal Team works; apps signed with it expire after **7 days** (see [Renewing](#renewing-free-accounts)). A paid Apple Developer Program team lasts a year.
2. Create your local settings file (it is ignored by git and stays on your Mac):

```bash
cp ios/local.env.example ios/local.env
```

3. Edit `ios/local.env`:

```bash
# Your Apple team ID (Xcode › Settings › Accounts › your team; or developer.apple.com › Membership).
# Leave it unset to use the team of the first "Apple Development" certificate in your keychain.
BOTW_TEAM=YOURTEAMID

# A bundle ID of your own. Bundle IDs are unique across all Apple accounts, so it must not be one
# somebody else already registered (com.botw.sheikahvision is the maintainer's). Use your name or domain.
BOTW_BUNDLE_ID=com.yourname.sheikahvision
```

If `BOTW_BUNDLE_ID` is not set, it defaults to `com.<your Mac user name>.sheikahvision`.

## 6. Build for Apple Vision Pro

1. **Prepare the headset:** on the Vision Pro, turn on **Settings › Privacy & Security › Developer Mode** (restart when asked). Pair it with Xcode: **Xcode › Window › Devices and Simulators**, select the Vision Pro and enter the code shown on the headset (**Settings › General › Remote Devices**). The Mac and the headset must be on the same network, or connected with the Developer Strap.
2. **Make the provisioning profile** (registers the headset with your team; increased memory limit only, which free teams may use):

```bash
BOTW_SDK=xros ios/scripts/provision.sh
```

3. **Build and sign** (downloads and builds SDL3, fmt and zstd the first time):

```bash
ios/scripts/build.sh xros
```

The signed app is `build-ios/xros/SheikahVision.app`.

## 7. Install and add the game

1. **Install** (Vision Pro awake and unlocked):

```bash
BOTW_SDK=xros ios/scripts/device.sh install
```

Installing again later replaces the app and keeps its data (game files, saves).

2. **Trust your developer certificate** (free teams): on the headset, **Settings › General › VPN & Device Management** › your account › Trust.
3. **Add the game files** — either way works:
   - **From the Mac** (incremental, unchanged files are skipped):

     ```bash
     BOTW_SDK=xros BOTW_GAME=/path/to/game ios/scripts/device.sh copy-game
     ```

   - **On the headset:** open SheikahVision › Settings › **Game Files** › Import, and choose the game folder (or a zip of it) from Files. The launcher checks every file against known hashes; nothing is uploaded.
4. **Play:** open SheikahVision, press **Launch Game**. The first launch compiles shaders and takes longer. Pair the PS VR2 Sense controllers with the headset beforehand (Settings › Bluetooth).

## Logs

```bash
BOTW_SDK=xros ios/scripts/device.sh log    # copies runtime.log to build-ios/runtime-device.log
```

Attach the relevant lines when you report a bug (remove anything personal first).

## Renewing (free accounts)

A free Personal Team's app stops opening after 7 days. Run steps 6.2, 6.3 and 7.1 again; game files and saves stay on the headset.

## Troubleshooting

| Problem | What to do |
|---|---|
| `no Apple Development certificate in the keychain` | Xcode › Settings › Accounts › Manage Certificates › + Apple Development, or set `BOTW_TEAM` |
| Provisioning says the bundle ID is not available | It is registered by another account; choose another `BOTW_BUNDLE_ID` |
| `CoreDeviceError 4016` (device cannot fulfill the request) | The headset is asleep, off your head or locked; wake and unlock it, then retry |
| `CoreDeviceError 3002` (install failed) | Usually transient; retry the install |
| The app does not open on the headset | Trust the developer certificate (step 7.2); renew if 7 days have passed |
| The launcher says the game files are missing or wrong | Check the `U-King.rpx` hash (step 2); make sure the v208 update was copied over the base game |
| Build errors after pulling | Re-run the recompiler (step 3), then build again |

More questions: [GitHub Discussions](https://github.com/zlinoliver/BotW-SheikahVision/discussions).
