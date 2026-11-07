# Third-party notices

SheikahVision's own code is under the [MIT License](../LICENSE). It builds on the projects below, which keep their own licenses.

## Included in this repository

| Project | Use in SheikahVision | License | License text |
|---|---|---|---|
| [BotWRecomp](https://github.com/IshaanKalra2103/BotWRecomp) (Ishaan Kalra) | The static recompiler, the Wii U runtime and HLE this project is based on | MIT | [LICENSES/BotWRecomp-MIT.txt](BotWRecomp-MIT.txt) |
| [BetterVR](https://github.com/Crementif/BotW-BetterVR) (Crementif and contributors) | The VR layer follows its methods, research and game addresses, and ports parts of its patches to C++ (marked in the source, e.g. `runtime/src/game/vr_frame.cpp`, `vr_render.cpp`) | MIT | [LICENSES/BetterVR-MIT.txt](BetterVR-MIT.txt) |
| [Cemu](https://github.com/cemu-project/Cemu) (commit a53cf0e) | Latte graphics layer, Metal renderer, shader decompiler, in `runtime/cemu/`; changes are marked `// BotWRecomp:` | MPL 2.0 (file-level: these files stay under MPL 2.0) | [runtime/cemu/LICENSE.txt](../runtime/cemu/LICENSE.txt) |
| [Dear ImGui](https://github.com/ocornut/imgui) (Omar Cornut) | Debug UI, in `runtime/cemu/dependencies/imgui/` | MIT | [LICENSE.txt](../runtime/cemu/dependencies/imgui/LICENSE.txt) |
| [metal-cpp](https://developer.apple.com/metal/cpp/) (Apple) | C++ bindings for Metal, in `runtime/cemu/dependencies/metal-cpp/` | Apache 2.0 | [LICENSE.txt](../runtime/cemu/dependencies/metal-cpp/LICENSE.txt) |

## Downloaded or installed at build time (not included)

| Project | Use | License |
|---|---|---|
| SDL3 | Window, input and audio | zlib |
| fmt | Formatting | MIT |
| zstd | Compression | BSD |
| Boost, GLM, OpenSSL, libpng (Mac build, from Homebrew) | Runtime support | Boost / MIT / Apache 2.0 / libpng |

## Not part of this repository

No game code, assets or keys are included. The C++ produced by the recompiler (`generated/`) is derived from your own copy of the game and must never be committed or shared.

*The Legend of Zelda* and *Breath of the Wild* are trademarks of Nintendo. This project is not affiliated with or endorsed by Nintendo.
