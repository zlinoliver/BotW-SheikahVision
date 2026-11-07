# 用 Mac 安装器安装

[English](INSTALLER.md) · [简体中文](INSTALLER.zh-CN.md)

旷野视界的 Mac 安装器会在你的 Mac 上用你自己的《塞尔达传说 旷野之息》Wii U 拷贝编译 Vision Pro 版 App，用你自己的 Apple 账号签名，再装到你的头显上。非官方粉丝项目，与任天堂无关。想用命令行自己编译？见 [BUILDING.zh-CN.md](BUILDING.zh-CN.md)。

## 准备

- Apple 芯片的 Mac，系统 **macOS 15.6 或更新**，装好 **Xcode 26 或更新版本**。Xcode 是必需的：编译、签名 App 和连接头显都靠它。可以在 [Mac App Store](https://apps.apple.com/app/xcode/id497799835) 免费安装（下载量很大，需要一些时间），装好后打开一次、同意许可协议。
- 在 Xcode › Settings › Components 里下载 **visionOS**。它包含 visionOS SDK，也就是 Xcode 给 Vision Pro 编译 App 必需的开发包。这一步会顺带下载 visionOS 模拟器（几 GB），安装器用不到它，但 Xcode 没有提供跳过的办法。
- 除了 Xcode 本身，编译还需要至少 25 GB 可用磁盘空间（约 40 GB 比较宽裕）。
- 编译工具 cmake、ninja、uv。用 [Homebrew](https://brew.sh) 安装：`brew install cmake ninja uv`。缺什么，安装器第一步会告诉你，并给出命令。
- 安装器自带的预编译部分是用某个 Xcode 版本编出来的。如果你的 Xcode 版本不同，安装器会自己编译这部分，这时还需要 `brew install boost glm`（第一步同样会提示），第一次编译也会更久。
- 在 Xcode › Settings › Accounts 登录 Apple 账号，免费账号即可。密码只在 Xcode 或 macOS 的系统窗口里输入，安装器不会问你要密码。
- 签名时 macOS 可能弹窗要你的 Mac 登录密码：输入后用鼠标点「始终允许」。
- 一对已和头显配对的 PlayStation VR2 Sense 手柄（设置 › 蓝牙）。

## 下载和第一次打开

1. 从 [Releases](https://github.com/zlinoliver/BotW-SheikahVision/releases/latest) 下载 `SheikahVision-Installer-<版本>.zip`，解压后把 **SheikahVision Installer** 拖到「应用程序」。
2. 双击打开。安装器没有经过公证，macOS 第一次会拦下来，点「完成」。
3. 打开 系统设置 › 隐私与安全性，滚到「安全性」，点 **仍要打开** 并确认（可能要输入 Mac 密码）。

只需要这一次。装到头显上的游戏 App 仍然用你自己的 Apple 账号签名。

之后安装器会带你走五步：Mac、Apple 账号、游戏文件、Vision Pro、安装。每一步都会自动检查；点顶部已完成的步骤可以回去修改。

## 游戏文件

需要你自己的 Wii U 拷贝，已解密的游戏文件夹（里面有 `code`、`content`、`meta`），并且有 v208 更新（1.5.0，Wii U 版的最后一个更新）。本体和更新可以一起选，安装器会自动合并，不用手动把更新覆盖到本体上。日版、美版、欧版都可以；简体中文汉化版只要没有改动 `code/U-King.rpx` 也可以。

**已经在 Cemu 里玩的：**

| | |
|---|---|
| 本体 | 安装进 Cemu 的：`mlc01/usr/title/00050000/<编号>`；没安装、直接从游戏列表加载的：游戏列表里的那个文件夹 |
| v208 更新 | `mlc01/usr/title/0005000e/<编号>` |
| 编号 | 日版 `101c9300`，美版 `101c9400`，欧版 `101c9500` |
| mlc01 在哪 | Cemu 的 Options › General settings › General 里的 Custom MLC path（旧版叫 MLC path）。没改过的话，在 `~/Library/Application Support/Cemu/mlc01` |

最简单：直接选 `mlc01/usr/title` 文件夹，安装器会自己找到本体和更新，其他游戏会被忽略。

不支持的格式：Cemu 的 `.wua` 单文件压缩包（Title Manager 里转换过的游戏）、`.wud` / `.wux` 光盘镜像。Cemu 没有把 `.wua` 解压回文件夹的功能，请用原来的游戏文件夹。

安装器只读取并复制一份，不会移动或改动你原来的文件。自己整理文件时请复制，不要移动。

## 连接头显

Vision Pro 和 Mac 连同一个 Wi-Fi，然后：

1. **头显：** 打开「设置 › 通用 › 远程设备」，保持在这个页面。
2. **Mac：** 打开 Xcode 的 Device Hub，点 + › Pair Nearby Device，选你的 Vision Pro，点下一步，输入头显上显示的配对码。旧版 Xcode 没有 Device Hub：在 Window › Devices and Simulators 里选 Vision Pro，点 Pair。
3. **头显：** 配对之后，「设置 › 隐私与安全性」里会出现「开发者模式」。打开它，按提示重启；开机后点「打开」确认。

安装器每隔几秒自动查找，配对好就会继续。找不到时，请戴上头显、解锁，确认在同一个 Wi-Fi。

## 安装

- 点「安装」后依次：在 Mac 上编译（第一次十几分钟）→ 签名 → 装到头显 → 通过 Wi-Fi 传游戏（约 13 GB，半小时左右）。
- 编译时不用戴头显，可以先摘下。安装和传输时要戴上并保持解锁，卡片会提醒你。安装期间 Mac 不会自动睡眠。
- 免费账号第一次打开 App 前，要在头显「设置 › 通用 › VPN 与设备管理」里信任你的 Apple 账号。
- 传完后安装器会重新打开头显上的 SheikahVision，它会自动导入游戏（首页显示进度，几分钟），完成后就可以开始玩了。中途退出下次打开会接着导；没有自动开始的话，点 **Prepare Game › Check and import**。

操作方式见 [README](../README.zh-CN.md#操作) 和头显 App 里的 设置 › 教程。

## 续签

免费 Apple 账号签的 App 7 天后会打不开；付费开发者账号一年有效。到时打开安装器点 **续签**：只重新签名并覆盖安装，不编译、不重传游戏，存档不会丢。续签时要戴上头显。主页会显示还剩几天。

## 更新

主页「检查新版本」会打开 [Releases](https://github.com/zlinoliver/BotW-SheikahVision/releases) 页面，每个版本都写了更新内容（也可以看[更新记录](CHANGELOG.md)）。下载新版安装器并打开，主页会显示 **安装新版本**：它用新代码重新编译并覆盖安装，游戏和存档都保留，头显里已有游戏就不再重传。窗口右下角是安装器的版本号；头显 App 的版本在 设置 › 关于。

## Mac 上的文件

- 游戏副本、编译缓存都在 `~/Library/Application Support/SheikahVision`。主页的「游戏文件」按钮可以在访达里打开游戏文件夹。
- 传给头显用的 13 GB 压缩包，传完会自动删除。
- 游戏传到头显后，可以删掉 Mac 上的游戏副本腾出空间：续签不受影响；以后安装新版本时，会请你重新选择游戏文件。
- 遇到问题：「帮助 › 导出诊断信息」，里面不含游戏、密钥或存档。可以附在 [Issue](https://github.com/zlinoliver/BotW-SheikahVision/issues) 里，或到 [Discussions](https://github.com/zlinoliver/BotW-SheikahVision/discussions) 提问。
