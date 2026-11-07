# Contributing to SheikahVision

Thanks for your interest! SheikahVision is a spare-time community project. Every contribution helps — bug reports with logs, fixes, research, measurements, documentation and translations.

[简体中文说明](#简体中文)

## Ground rules (please read first)

1. **No game data, ever.** Do not post, attach or link game files, dumps, keys (title keys, common keys), extracted assets, saves taken from others, generated recompiled code or IPA files — in issues, pull requests, discussions or commits. Do not ask for them or explain how to obtain them. Such content is removed and the account may be blocked.
2. **Bring your own copy.** Everyone works from a legally purchased Wii U copy they dumped themselves.
3. **Be kind.** See the [Code of Conduct](CODE_OF_CONDUCT.md).
4. **Patience.** The maintainer works on this in spare time; reviews may take a few days.

## Ways to help

- **Report bugs** with the [bug report template](https://github.com/zlinoliver/BotW-SheikahVision/issues/new/choose). A `runtime.log` excerpt makes most bugs fixable.
- **Pick an issue** labelled `good first issue` or `help wanted`, and say in the issue that you're on it.
- **No headset?** Many problems (game logic, controls logic, rendering) reproduce in the Mac VR mode — see [step 4 of the build guide](../docs/BUILDING.md#4-optional-build-and-run-on-the-mac).
- **Measure.** Performance and picture-quality work needs data from real headsets, especially the M2 Vision Pro.
- **Document.** Build steps on a fresh Mac, troubleshooting, translations.

## Development flow

1. Fork the repository and create a branch: `fix/ladder-climbing`, `feat/bicubic-upscale`, `docs/building`.
2. Keep one change per pull request. Small PRs get reviewed faster.
3. Build and test:
   - On the Mac (VR mode) when the change can be checked there — say what you ran.
   - On a Vision Pro when the change touches headset-only parts (CompositorServices, ARKit, controllers) — attach the relevant log lines, and a short video for interaction changes.
4. Open the pull request with the template filled in.

## Code style

- Follow the style of the surrounding code (C++20 in the runtime, Swift 6 in the visionOS app, Objective-C++ in the bridges).
- Comments explain **why**, not what. When a value or workaround comes from a test, say so (device, date, what was observed) — the existing code does this throughout and it keeps decisions traceable.
- Credit sources: when you port an idea or an address from BetterVR or another project, name the file or function.
- Game patches live in `runtime/src/game/`. A new `BOTW_HOOK` / `BOTW_REPLACE` needs the recompiler to run again (`tools/recompile.py`) — mention it in the PR.
- New options go behind an environment variable first, off by default, until tested on a headset.

## Logs

- The app writes `runtime.log` (and `runtime-prev.log` for the previous run) into its data container on the headset; on the Mac, into the user folder you run with.
- Before attaching a log, remove anything personal (paths with your user name, device names if you prefer).
- Logs never contain game assets, but **do not attach captures or screenshots that show copyrighted material beyond normal gameplay**.

## Commit messages

- English, imperative mood, explain the change and the reason: `VR ladders: stick not turned on a ladder (it reached the game as a 45° diagonal)`.

## License

By contributing you agree that your contribution is licensed under the MIT License (MPL 2.0 for changes inside the Cemu-derived code).

---

## 简体中文

感谢参与！SheikahVision 是业余维护的社区项目，任何形式的贡献都有帮助：附日志的问题报告、修复、研究、测量数据、文档和翻译。

**基本规则：**

1. **任何时候都不涉及游戏数据。** 不要在问题、PR、讨论或提交中发布、附带或链接游戏文件、导出内容、密钥、提取的资源、他人的存档、重编译生成的代码或 IPA，也不要索要或讲解获取方法。发现即删除，账号可能被封禁。
2. **自备正版。** 所有人都使用自己购买并自行导出的 Wii U 拷贝。
3. **友善交流。** 见[行为准则](CODE_OF_CONDUCT.md)。
4. **耐心等待。** 维护者业余时间处理，审核可能需要几天。

**怎么参与：**

- 用[问题模板](https://github.com/zlinoliver/BotW-SheikahVision/issues/new/choose)报告问题，有 `runtime.log` 片段的话请附上。
- 认领标签为 `good first issue` 或 `help wanted` 的问题，在问题里说一声。
- 没有头显也能参与：很多问题在 Mac 的 VR 模式下就能复现，见[编译指南第 4 步](../docs/BUILDING.zh-CN.md#4可选在-mac-上编译运行)。
- 提供测量数据，特别是 M2 Vision Pro 上的性能数据。
- 完善文档：在全新 Mac 上走一遍编译步骤、常见问题、翻译。

**开发流程、代码风格、日志和提交说明**同上文英文部分：一个 PR 只做一件事；注释说明原因并注明测试设备和日期；移植 BetterVR 等项目的做法时注明出处；新增 hook 需要重跑 `tools/recompile.py`；新选项先放在默认关闭的环境变量后面。提交说明用英文。
