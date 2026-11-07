# 编译旷野视界（SheikahVision）

[English](BUILDING.md)

本指南用**你自己的**游戏文件从源码编译旷野视界，用**你自己的** Apple 账号签名，并安装到**你自己的** Apple Vision Pro。整个过程不会下载或分享任何游戏内容。

> [!IMPORTANT]
> 你需要自己购买的 Wii U 正版《塞尔达传说 旷野之息》及 v208 更新，并由你自己导出。本项目不提供游戏文件、密钥或编译好的 App，也不协助获取。

## 概览

| 步骤 | 内容 | 首次耗时（M 系列 Mac） |
|---|---|---|
| 1. 准备 Mac | Xcode、Homebrew 软件包 | — |
| 2. 准备游戏文件 | 导出自己的拷贝，合并本体和更新，校验哈希 | — |
| 3. 重编译 | `U-King.rpx` → `generated/` 里的 C++ | 约 1 分钟 |
| 4.（可选）Mac 版 | 在 Mac 上游玩 / 测试，也支持 VR 模式 | 约 10 分钟 |
| 5. 配置签名 | 你的团队和你自己的 Bundle ID | — |
| 6. 编译 Vision Pro 版 | 依赖库、App、签名 | 约 15 分钟 |
| 7. 安装并放入游戏 | 通过网络或数据线安装 App，把游戏文件传到头显 | 传游戏：10–30 分钟 |

## 1. 准备 Mac

- **Apple Silicon** 的 Mac。
- App Store 安装 **Xcode 26 或更新版本**，并安装 **visionOS** 平台组件（Xcode › Settings › Components）。
- 安装 [Homebrew](https://brew.sh)，然后：

```bash
brew install cmake ninja uv boost glm sdl3 fmt openssl@3 zstd libpng
```

（`sdl3`、`fmt`、`openssl@3`、`zstd`、`libpng` 只有 Mac 版需要；Vision Pro 版会自己下载并编译 SDL3、fmt、zstd。）

克隆仓库：

```bash
git clone https://github.com/zlinoliver/BotW-SheikahVision.git
cd BotW-SheikahVision
```

## 2. 准备游戏文件

旷野视界需要用你自己的拷贝制作的**已解密、已解包的游戏文件夹**，不使用光盘镜像。

> [!NOTE]
> 请遵守你所在国家或地区的法律。本项目不介绍主机改装和导出工具，问题和讨论区也不讨论这些内容。

### 需要准备

- 你自己的 Wii U，和你自己的《旷野之息》（光盘版或 eShop 数字版）。
- 游戏已**在主机上更新到 v208**（游戏内显示为版本 1.5.0）。更新由主机联网下载，不在光盘里。
- 不需要 DLC，也不需要存档。

### 导出你自己的拷贝

从你自己的主机上导出以下两项，格式为已解密的文件，也就是 Cemu 使用的格式（一个包含 `code/`、`content/`、`meta/` 的文件夹）：

1. **游戏本体**；
2. 它的**更新**（v208）。

不同导出工具生成的文件夹名可能不同，关键是每份导出里都有 `code/`、`content/`、`meta/`。

### 已经在 Cemu 里玩过？

如果你已经在电脑上用 Cemu 玩过《旷野之息》，本体和更新这两部分多半已经都有了：

| 部分 | Cemu 把它放在哪 |
|---|---|
| 游戏本体 | 你添加到 Cemu 游戏列表里的那个游戏文件夹（里面有 `code/`、`content/`、`meta/`）。如果是把游戏安装进了 Cemu，则在 `mlc01/usr/title/00050000/<title>` |
| 更新（v208） | `mlc01/usr/title/0005000e/<title>` |

`<title>` 为 `101c9300`（日版）、`101c9400`（美版）或 `101c9500`（欧版）。`mlc01` 的位置可以在 Cemu 的 **Options › General settings › General › Custom MLC path** 里看到（旧版本叫 MLC path）；Mac 上默认在 `~/Library/Application Support/Cemu/mlc01`。游戏列表里会显示已安装更新的版本号（应为 208）。

把这两个文件夹复制出来（不要移动，Cemu 还要用），再按下面的方法合并。

### 合并成一个文件夹

| 导出的内容 | 里面有什么 | 怎么处理 |
|---|---|---|
| 游戏本体 | 完整的 `code/`、`content/`、`meta/` | 以它为底 |
| 更新（v208） | 同样的三个文件夹，只有改动过的文件，包括 v208 的 `U-King.rpx` | 覆盖到本体上，替换同名文件 |
| DLC（如果导出了） | 单独一份 | 不需要，不用放进来 |

合并后是一个文件夹，里面正好是这三个子文件夹：

```
game/
├── code/      可执行文件 U-King.rpx（v208），以及 app.xml、cos.xml
├── content/   游戏数据，约 12 GB
└── meta/      meta.xml：游戏 ID 和版本号，用来识别地区和更新
```

### 校验

```bash
shasum -a 256 /path/to/game/code/U-King.rpx
# ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30
```

重编译器和游戏补丁都只针对这一个文件。支持的版本：

| 地区 | 游戏 ID | 状态 |
|---|---|---|
| 日版 | `00050000101C9300` | 已用真实拷贝测试 |
| 美版 | `00050000101C9400` | 已用真实拷贝测试 |
| 欧版 | `00050000101C9500` | 已支持，尚未测试 |

### 文件夹放在哪

- **Mac 上**（第 3 步和 Mac 版）：脚本默认在仓库旁边名为 `game` 的文件夹里找游戏；放在别处时设置 `BOTW_GAME=/path/to/game`。
- **Vision Pro 上**（第 7 步）：从 Mac 用 `device.sh copy-game` 传过去，或在启动器里导入这个文件夹或它的 **zip**。zip 里的 `code/`、`content/`、`meta/` 可以在最外层，也可以套在一个文件夹里。用"仅存储、不压缩"方式打包的 zip 导入最快。启动器会按已知哈希逐个校验。

### 不支持的格式

- **WUD / WUX 光盘镜像。** 它们是整张光盘的加密镜像，而且 v208 更新本来就不在光盘里。请改用已解密导出的本体和更新。
- 其他打包格式（例如 WUA）。
- 只有本体没有 v208 更新，或只有更新没有本体。

### 启动器拒绝导入时

| 提示 | 原因 |
|---|---|
| Not a supported Wii U Breath of the Wild title | `meta/meta.xml` 里的游戏 ID 不是上面三个之一 |
| This is not v208 / U-King.rpx does not match v208 | 更新没有覆盖到本体上，或者不是 v208 |
| Required game files are missing | 缺少 `code/U-King.rpx`、`code/app.xml`、`code/cos.xml`、`content/` 或 `meta/meta.xml`，常见原因是 zip 多套了一层文件夹 |
| Region and language packs do not match | 混入了不同地区的文件 |
| DLC detected and ignored | 无影响：暂不支持 DLC |

## 3. 把游戏重编译成 C++

```bash
uv run python -I tools/recompile.py /path/to/game/code/U-King.rpx generated
```

会生成 `generated/`（约 700 MB 的 C++）。它由游戏衍生而来，**绝不能提交或分享**，`.gitignore` 已经排除了它。拉取到新增或删除游戏 hook（`runtime/src/game/` 里的 `BOTW_HOOK` / `BOTW_REPLACE`）的改动后，要重新运行这一步。

## 4.（可选）在 Mac 上编译运行

适合没有头显时开发和测试。

```bash
cmake -B build -G Ninja && ninja -C build
BOTW_GAME=/path/to/game tools/play.sh            # 平面窗口（F11 全屏）
BOTW_VR=1 BOTW_GAME=/path/to/game tools/play.sh  # VR 模式：左右眼并排显示
```

存档和设置在 `~/Library/Application Support/SheikahVision`。想从某个存档开始测试又不动真实存档，见 `tools/test_save.sh`（把 `BOTW_TEST_SAVE` 指向一份你自己存档的拷贝）。

## 5. 配置签名（你的团队、你的 Bundle ID）

1. 在 Xcode 登录你的 Apple 账号：**Xcode › Settings › Accounts**。免费个人团队即可，签出的 App **7 天**后过期（见[续签](#续签免费账号)）；付费开发者账号有效期一年。
2. 创建本地设置文件（被 git 忽略，只留在你的 Mac 上）：

```bash
cp ios/local.env.example ios/local.env
```

3. 编辑 `ios/local.env`：

```bash
# 你的 Apple 团队 ID（Xcode › Settings › Accounts › 你的团队；或 developer.apple.com › Membership）。
# 不填则使用钥匙串里第一个 "Apple Development" 证书所属的团队。
BOTW_TEAM=YOURTEAMID

# 你自己的 Bundle ID。Bundle ID 在所有 Apple 账号之间唯一，不能用别人已注册的
# （com.botw.sheikahvision 是维护者的）。请用你的名字或域名。
BOTW_BUNDLE_ID=com.yourname.sheikahvision
```

不设置 `BOTW_BUNDLE_ID` 时，默认是 `com.<你的 Mac 用户名>.sheikahvision`。

## 6. 编译 Apple Vision Pro 版

1. **准备头显：** 在 Vision Pro 上打开 **设置 › 隐私与安全性 › 开发者模式**（按提示重启）。与 Xcode 配对：**Xcode › Window › Devices and Simulators**，选中 Vision Pro，输入头显上显示的配对码（**设置 › 通用 › 远程设备**）。Mac 和头显需在同一网络，或用开发者套件有线连接。
2. **生成描述文件**（把头显注册到你的团队；只用"更高内存上限"权限，免费团队可用）：

```bash
BOTW_SDK=xros ios/scripts/provision.sh
```

3. **编译并签名**（第一次会下载并编译 SDL3、fmt、zstd）：

```bash
ios/scripts/build.sh xros
```

签名后的 App 是 `build-ios/xros/SheikahVision.app`。

## 7. 安装并放入游戏

1. **安装**（头显亮屏并解锁）：

```bash
BOTW_SDK=xros ios/scripts/device.sh install
```

以后再次安装会覆盖 App，保留数据（游戏文件、存档）。

2. **信任开发者证书**（免费团队）：头显上 **设置 › 通用 › VPN 与设备管理** › 你的账号 › 信任。
3. **放入游戏文件**，两种方式任选：
   - **从 Mac 传**（增量传输，未变化的文件会跳过）：

     ```bash
     BOTW_SDK=xros BOTW_GAME=/path/to/game ios/scripts/device.sh copy-game
     ```

   - **在头显上导入：** 打开旷野视界（头显上的 App 名为 SheikahVision）› 设置 › **游戏文件** › 导入，从"文件"里选择游戏文件夹（或它的 zip）。启动器会按已知哈希逐个校验，不会上传任何内容。
4. **开玩：** 打开旷野视界，点 **启动游戏**。第一次启动要编译着色器，会慢一些。事先在头显上配对 PS VR2 Sense 手柄（设置 › 蓝牙）。

## 日志

```bash
BOTW_SDK=xros ios/scripts/device.sh log    # 把 runtime.log 拷到 build-ios/runtime-device.log
```

报告问题时附上相关几行（先去掉个人信息）。

## 续签（免费账号）

免费个人团队签的 App 7 天后打不开。重新运行第 6.2、6.3 和 7.1 步即可，游戏文件和存档会保留在头显上。

## 常见问题

| 问题 | 处理 |
|---|---|
| `no Apple Development certificate in the keychain` | Xcode › Settings › Accounts › Manage Certificates › + Apple Development，或设置 `BOTW_TEAM` |
| 生成描述文件时提示 Bundle ID 不可用 | 已被别的账号注册，换一个 `BOTW_BUNDLE_ID` |
| `CoreDeviceError 4016`（设备无法满足请求） | 头显休眠、未佩戴或锁屏；唤醒并解锁后重试 |
| `CoreDeviceError 3002`（安装失败） | 通常是暂时性的，重试安装 |
| 头显上打不开 App | 信任开发者证书（第 7.2 步）；超过 7 天需续签 |
| 启动器提示游戏文件缺失或不对 | 检查 `U-King.rpx` 的哈希（第 2 步）；确认 v208 更新已覆盖到本体 |
| 拉取新代码后编译出错 | 重新运行重编译（第 3 步），再编译 |

更多问题：[GitHub Discussions](https://github.com/zlinoliver/BotW-SheikahVision/discussions)。
