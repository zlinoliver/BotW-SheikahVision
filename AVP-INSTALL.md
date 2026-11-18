# Installing SheikahVision on Apple Vision Pro

SheikahVision plays *The Legend of Zelda: Breath of the Wild* in first-person VR on Apple Vision Pro. It is an unofficial fan project, not affiliated with Nintendo.

This repository and its releases contain no game code, no game assets, no keys and no prebuilt game app. You build the app on your own Mac from your own game files, sign it with your own Apple account and install it on your own headset.

## What you need

- **Your own copy of the game.** You must supply the Wii U version of *Breath of the Wild* with the **v208 update** (1.5.0), from a copy you own, as a game folder with `code/`, `content/` and `meta/` inside. Disc images (WUD / WUX) and single-file archives (WUA) are not supported. Japan and USA copies are tested; Europe is supported but not tested yet. No DLC or save data is needed.
- **A Mac:** Apple silicon, macOS 15.6 or later, Xcode 26 or later with the **visionOS** component (Xcode › Settings › Components), and at least 25 GB of free disk space for the build (about 40 GB is comfortable).
- **Build tools:** `brew install cmake ninja uv` with [Homebrew](https://brew.sh).
- **An Apple account** signed in under Xcode › Settings › Accounts. A free account works; the app then stops opening after 7 days and has to be re-signed. A paid developer account lasts a year.
- **Apple Vision Pro** with Developer Mode on, paired with your Mac. Tested on the M5 model with visionOS 27; the M2 model is untested.
- **PlayStation VR2 Sense controllers**, paired with the headset (Settings › Bluetooth).

## Pair Vision Pro with your Mac

1. Put the headset and the Mac on the same Wi-Fi.
2. **Vision Pro:** open Settings › General › Remote Devices and stay on that page.
3. **Mac:** in Xcode, pair the headset (Device Hub › + › Pair Nearby Device, or Window › Devices and Simulators › Pair in older Xcode) and type the code the headset shows.
4. **Vision Pro:** turn on Settings › Privacy & Security › Developer Mode and restart when asked.

## Option 1: the Mac installer (recommended)

The installer is a Mac app that runs the build steps below for you. It contains no game data and no prebuilt Vision Pro app; it builds the app on your Mac from your game folder.

1. Download `SheikahVision-Installer-<version>.zip` from [Releases](https://github.com/zlinoliver/BotW-SheikahVision/releases/latest), unzip it and move **SheikahVision Installer** to Applications.
2. The installer is not notarized: open it once, then allow it under System Settings › Privacy & Security › **Open Anyway**.
3. Follow its five steps: Mac, Apple account, Game files, Vision Pro, Install. In the game files step, select your base game folder and your v208 update folder together; the installer combines them.
4. **Install** compiles (about 15 minutes the first time), signs, installs the app on the headset and sends your game files over Wi-Fi (about 13 GB). Wear the headset and keep it unlocked from the install step on.
5. With a free account, trust your developer certificate on the headset: Settings › General › VPN & Device Management › your account › Trust.
6. The app imports the game by itself on first launch, then you can play.

More detail on each step, renewing and updating: [docs/INSTALLER.md](docs/INSTALLER.md).

## Option 2: build from source

Run these from a checkout of this repository.

### 1. Install the dependencies

```bash
brew install cmake ninja uv boost glm
```

### 2. Put your game files in place

Make one folder from your base game and your v208 update: start from the base game folder and copy the update's `code/`, `content/` and `meta/` over it, replacing files with the same name. The result holds exactly `code/`, `content/` and `meta/`.

Check that the executable is the v208 one:

```bash
shasum -a 256 /path/to/game/code/U-King.rpx
# ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30
```

### 3. Recompile the game executable to C++

```bash
uv run python -I tools/recompile.py /path/to/game/code/U-King.rpx generated
```

This writes `generated/` from your own files. It is derived from the game, so never commit or share it (`.gitignore` already excludes it).

### 4. Configure signing

```bash
cp ios/local.env.example ios/local.env
```

Edit `ios/local.env`: set `BOTW_TEAM` to your Apple team ID (or leave it unset to use your first Apple Development certificate) and `BOTW_BUNDLE_ID` to a bundle ID of your own, such as `com.yourname.sheikahvision`.

### 5. Build for Vision Pro

```bash
BOTW_SDK=xros ios/scripts/provision.sh
ios/scripts/build.sh xros
```

The signed app is `build-ios/xros/SheikahVision.app`.

### 6. Install and add your game files

With the headset awake and unlocked:

```bash
BOTW_SDK=xros ios/scripts/device.sh install
BOTW_SDK=xros BOTW_GAME=/path/to/game ios/scripts/device.sh copy-game
```

You can also import the game folder (or a zip of it) on the headset under SheikahVision › Settings › Game Files › Import. With a free account, trust your developer certificate (Settings › General › VPN & Device Management) before the first launch.

### 7. Play

Open SheikahVision and press **Launch Game**. The first launch compiles shaders and takes longer. The controls are in the [README](README.md#controls) and in the app under Settings › Guide.

## Renewing a free account

An app signed with a free Apple account stops opening after 7 days. Choose **Renew signing** in the installer, or run the provision, build and install commands from steps 5 and 6 again. Your game files and saves stay on the headset.

## Help

Open an [issue](https://github.com/zlinoliver/BotW-SheikahVision/issues) or ask in [Discussions](https://github.com/zlinoliver/BotW-SheikahVision/discussions). Never post game files, keys or links to them.
