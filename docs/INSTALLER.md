# Installing with the Mac installer

[English](INSTALLER.md) · [简体中文](INSTALLER.zh-CN.md)

The SheikahVision Mac installer builds the Vision Pro app on your Mac from your own Wii U copy of *The Legend of Zelda: Breath of the Wild*, signs it with your own Apple account and installs it on your headset. Unofficial fan project, not affiliated with Nintendo. Prefer the command line? See [BUILDING.md](BUILDING.md).

## Before you start

- An Apple silicon Mac on **macOS 15.6 or later**, with **Xcode 26 or later**. Xcode is required: it compiles and signs the app and talks to the headset. Install it for free from the [Mac App Store](https://apps.apple.com/app/xcode/id497799835) (a large download, allow some time), open it once and accept the license.
- In Xcode › Settings › Components, download **visionOS**. It holds the visionOS SDK, the part Xcode needs to build apps for Vision Pro. The download also brings the visionOS Simulator (a few GB); the installer does not use it, but Xcode offers no way to skip it.
- At least 25 GB of free disk space for the build (about 40 GB is comfortable), on top of Xcode itself.
- The build tools cmake, ninja and uv. With [Homebrew](https://brew.sh): `brew install cmake ninja uv`. The installer's first step tells you what is missing and shows the command.
- The installer carries prebuilt parts made with one Xcode version. With a different Xcode it builds them itself: it then also needs `brew install boost glm` (the first step shows this too), and the first build takes longer.
- Sign in under Xcode › Settings › Accounts; a free Apple account works. Passwords go only into Xcode or macOS dialogs; the installer never asks for one.
- While signing, macOS may ask for your Mac password: enter it, then click **Always Allow** with the mouse.
- A pair of PlayStation VR2 Sense controllers paired with the headset (Settings › Bluetooth).

## Download and first launch

1. Download `SheikahVision-Installer-<version>.zip` from [Releases](https://github.com/zlinoliver/BotW-SheikahVision/releases/latest), unzip it and move **SheikahVision Installer** to Applications.
2. Double-click it. The installer is not notarized, so macOS blocks it the first time: choose **Done**.
3. Open System Settings › Privacy & Security, scroll to Security, click **Open Anyway** and confirm (you may be asked for your Mac password).

This is needed once. The game app on your headset is still signed with your own Apple account.

The installer then walks you through five steps: Mac, Apple account, Game files, Vision Pro and Install. Each is checked automatically; click a finished step at the top to change it.

## Game files

You need your own Wii U copy as a decrypted game folder (with `code`, `content` and `meta` inside) plus the v208 update (1.5.0, the last Wii U update). Select the base game and the update together; the installer combines them, so there is no need to copy the update over the base. Japan, USA and Europe copies all work; a Simplified Chinese patched copy works as long as `code/U-King.rpx` is unchanged.

**Already playing in Cemu?**

| | |
|---|---|
| Base game | Installed into Cemu: `mlc01/usr/title/00050000/<id>`; loaded straight from the game list: the folder in the list |
| v208 update | `mlc01/usr/title/0005000e/<id>` |
| id | Japan `101c9300`, USA `101c9400`, Europe `101c9500` |
| Where mlc01 is | Cemu's Options › General settings › General, Custom MLC path (MLC path in older versions). If you never changed it: `~/Library/Application Support/Cemu/mlc01` |

Simplest: choose the `mlc01/usr/title` folder itself. The installer finds the base and the update and ignores other games.

Not supported: Cemu's single-file `.wua` archives (games converted in the Title Manager) and `.wud` / `.wux` disc images. Cemu cannot unpack a `.wua` back into a folder, so use the original game folder.

The installer only reads and copies; your original files are never moved or changed. If you rearrange files yourself, copy them, don't move them.

## Connect Vision Pro

Put Vision Pro on the same Wi-Fi as your Mac, then:

1. **Vision Pro:** open Settings › General › Remote Devices and stay on that page.
2. **Mac:** in Xcode's Device Hub, click + › Pair Nearby Device, pick your Vision Pro, click Next and type the code the headset shows. Older Xcode has no Device Hub: use Window › Devices and Simulators, select Vision Pro and click Pair.
3. **Vision Pro:** after pairing, Developer Mode appears in Settings › Privacy & Security. Turn it on and restart when asked; after the restart, tap Turn On.

The installer keeps looking every few seconds and continues once the headset is paired. If it can't find it, put the headset on, unlock it and check that both are on the same Wi-Fi.

## Install

- **Install** compiles on your Mac (about a quarter of an hour the first time), signs, installs on the headset and sends the game over Wi-Fi (about 13 GB, roughly half an hour).
- No need to wear Vision Pro while compiling. For the install and the transfer, wear it and keep it unlocked; the card tells you when. Your Mac does not go to sleep during the install.
- With a free account, before the first launch trust your Apple account in Vision Pro Settings › General › VPN & Device Management.
- When the transfer is done, the installer reopens SheikahVision on the headset and it imports the game by itself (progress on its home screen, a few minutes). Then play. Leaving midway is fine, it resumes next time; if it doesn't start by itself, tap **Prepare Game › Check and import**.

The controls are in the [README](../README.md#controls) and in the app under Settings › Guide.

## Renew

An app signed with a free Apple account stops opening after 7 days; a paid developer account lasts a year. Open the installer and choose **Renew signing**: it only re-signs and reinstalls, with no compiling and no game transfer, and your saves are kept. Wear the headset while renewing. The home page shows how many days are left.

## Update

**Check for updates** on the home page opens the [Releases](https://github.com/zlinoliver/BotW-SheikahVision/releases) page, where each version lists what changed (also in the [changelog](CHANGELOG.md)). Download the new installer and open it; the home page then offers **Install new version**, which rebuilds with the new code and installs over the app. Your game and saves are kept, and the game is not sent again if the headset already has it. The installer's version is shown at the bottom right of the window; the app's is under Settings › About on the headset.

## Files on your Mac

- The game copy and build caches live in `~/Library/Application Support/SheikahVision`. The **Game files** button on the home page opens the game folder in Finder.
- The 13 GB package sent to the headset is deleted from your Mac after the transfer.
- Once the game is on the headset you may delete the game copy on your Mac to free space: renewing still works, and installing a new version will ask for the game folder again.
- Something wrong? **Help › Export diagnostics**. It contains no game files, keys or saves. Attach it to an [issue](https://github.com/zlinoliver/BotW-SheikahVision/issues) or ask in [Discussions](https://github.com/zlinoliver/BotW-SheikahVision/discussions).
