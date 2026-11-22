# Changelog

What changed in each version of SheikahVision. The same notes are on the [Releases](https://github.com/zlinoliver/BotW-SheikahVision/releases) page, together with the Mac installer for that version.

**To update:** download the new installer from Releases, open it and choose **Install new version**. The app is rebuilt on your Mac and installed over the old one; your game and saves on the headset are kept.

每个版本更新了什么。同样的内容也写在 [Releases](https://github.com/zlinoliver/BotW-SheikahVision/releases) 页面，附带这个版本的 Mac 安装器。

**如何更新：** 从 Releases 下载新的安装器，打开后选择 **安装新版本**。App 会在你的 Mac 上重新编译并覆盖安装，头显上的游戏和存档都会保留。

## Unreleased

### Changed (Apple Vision Pro M2 only)

- **Frame pacing that follows what the game can hold (#12).** On M2 the game runs below 45 fps, and each picture stayed on screen for 2, 3 or 4 refreshes in no order, which looked like heavy judder. The game and the display now share one pace: 45, 30 or 22.5 fps, whichever the game can hold evenly, chosen automatically (45 or 22.5 fps when visionOS lowers the display to 45 Hz on a hot headset). **Steady 30 fps** in the settings still fixes it at 30.
- **Lighter Smooth preset on M2**, and Smooth is the default on a fresh install. If you started an earlier version, choose **Smooth** in the settings yourself.
- The HUD panel follows your head sooner and more gently, so the minimap no longer slides out of view and jumps back.
- M5 and other models are unchanged.

### 变更（仅 Apple Vision Pro M2）

- **帧节奏按游戏能稳住的帧率走（#12）。** M2 上游戏跑不到 45 帧，每张画面停留 2、3、4 次刷新没有规律，看起来抖得厉害。现在游戏和显示共用一个节奏，自动在 45、30、22.5 帧中选一个能稳住的（头显过热、visionOS 把显示降到 45 Hz 时在 45 和 22.5 帧之间选）。设置里的 **稳定 30 帧** 仍会固定为 30。
- **M2 的「流畅」档更轻**，全新安装默认就是「流畅」。之前启动过旧版的，请在设置里自己选 **流畅**。
- HUD 面板更早、更平缓地跟随头部，小地图不会再滑出视野后突然弹回。
- M5 和其它机型不变。

## 0.1.1

### Fixed

- The right eye's picture shook when turning the head or moving, while the left eye was steady. The right eye is drawn across two frames, so it was made with one head pose but reprojected as if it had the left eye's newer pose. Each eye is now drawn and reprojected with its own pose (found with headset A/B tests on Apple Vision Pro M5).
- A frame now uses per-pixel depth for reprojection only when both eyes' depth arrived for it; before, an eye whose depth was missing used depth from three frames earlier.
- On a fresh install, the app now creates its **Import** folder at first launch, so it shows in the Files app right away as the place to put a game package. Before, it appeared only after **Prepare Game › Check and import**.

### 修复

- 转头或移动时右眼画面抖动，左眼稳定。右眼的画面是跨两帧完成的：画的时候用的是一个头部姿态，补偿时却按左眼较新的姿态处理。现在每只眼都按自己的姿态绘制和补偿（在 Apple Vision Pro M5 上通过对比测试定位）。
- 只有两只眼的深度都到齐时，这一帧才使用逐像素深度做补偿；之前缺深度的那只眼会用三帧前的深度。
- 全新安装后，App 首次启动就会建好 **Import** 文件夹，「文件」App 里马上能看到放游戏包的位置；之前要先点 **准备游戏 › 检查并导入** 才会出现。

## 0.1.0 — first public release / 首个公开版本

### New

- **Mac installer.** Builds the app on your Mac from your own Wii U copy, signs it with your own Apple account (a free account works), installs it on Vision Pro and transfers the game, in five guided steps. Renew a free account's signing in one click; install new versions without sending the game again. See the [installer guide](INSTALLER.md).
- The app imports the game package by itself when it arrives on the headset.
- First-person VR with native stereo rendering, reprojected to 90 Hz (game at 45 fps); motion controls for sword, shield, bow and throwing; BetterVR-style body slots and gestures; PS VR2 Sense controllers.
- Sheikah Slate runes aim where you look. Magnesis: once an object is held, the right stick moves it left / right and up / down, and holding R2 or the right grip while pushing the right stick forward / back pushes it away or pulls it closer.
- The Camera rune and the scope are shown flat on a screen in front of you, as in BetterVR; in them the left grip confirms and the right grip goes back.
- Settings › Controls › Story cutscenes: Automatic, or every story cutscene on the screen in front.
- Optional experience save, switched on from the home page or Settings › Saves (off by default): the author's completed game, in a save folder of its own, to try everything without starting over.

### Fixed

- Grey-green noise pattern in the sky and distance around Death Mountain.
- Grey blocks behind translucent HUD elements (such as the spirit orb counter).
- Camera rune photos came out black; the viewfinder was overexposed.
- Magnesis and Stasis targets appeared beside the crosshair and came and went.
- Pins placed with the scope floated in mid-air.
- The game nearly froze after using runes, for example on top of a Cryonis pillar.
- The game camera could break after Link stopped in water, which slowed the game to a crawl later on and turned the map cursor the wrong way.
- Riding: the view sat beside the saddle, and pushing the stick forward turned the horse.
- The stick barely moved through lists in conversations, such as the stable's horses.
- Riding: arrows always flew straight ahead; now the bow follows your hands and the arrow follows the aiming line.
- Distant enemies and animals you looked at from the side could stand frozen until you came close.
- A vertical line splitting the sky into two shades when looking far to the side or behind.
- Recordings and screenshots had a dark band down the right side.
- The game crashed when the Great Plateau Tower's movie started; the game's movies now play.
- A rare crash while walking (a graphics command the game sent garbled): the frame is now skipped instead.
- Save thumbnails in the load menu were black; they now show the view from just before the save.
- Installer: the progress bar stood still for minutes while the game was copied to the headset; it now shows the gigabytes sent and the time left. A copy cut off part-way is now sent again instead of being taken for a finished one.
- Installer: if the app was removed from the headset, the installer now offers to install it again (with the game) instead of only renewing the signature.
- Installer: renewing with a free Apple account now gets a fresh 7-day signature; before, it kept the old expiry date.
- Talks played around you no longer swing the view with every camera pan, which could cause motion sickness.
- The map opened during a conversation flickered.
- Holding an object with Magnesis, you could not turn; the right stick now turns you and the object together. With snap turning the view follows the object in steps of the snap angle, so the turn does not cause motion sickness.
- On the screen in front (dialogs, shops, the title), the left grip confirms and the right grip goes back, as in the menus.

### Known issues

- Picture sharpness is still limited by the GPU budget; heavy areas (Korok Forest, towns) can dip below 45 fps.
- If the game still slows to a crawl after a rune, please send Help › Export diagnostics from the installer with an [issue](https://github.com/zlinoliver/BotW-SheikahVision/issues): the log now names the slow physics query.
- After a few minutes of play, once the headset warms up, visionOS may lower the display to 45 Hz and the picture stutters noticeably. Choose Settings › Picture quality › Smooth on Vision Pro if this happens. Adapting the resolution automatically is planned.
- Apple Vision Pro (M2) is untested.

### 新内容

- **Mac 安装器：** 五个引导步骤，在你的 Mac 上用你自己的 Wii U 拷贝编译 App，用你自己的 Apple 账号签名（免费账号即可），装到 Vision Pro 并传输游戏。免费账号一键续签；安装新版本时不用重传游戏。见[安装器使用指南](INSTALLER.zh-CN.md)。
- 游戏包传到头显后，App 会自动导入。
- 第一人称 VR：每只眼原生立体渲染，头显重投影到 90 Hz（游戏 45 帧）；剑、盾、弓和投掷的体感操作；BetterVR 式的身体位置和手势；PS VR2 Sense 手柄。
- 希卡之石的符文跟着视线瞄准。磁力抓取：吸住物体后，右摇杆左右移动、上下升降；按住 R2 或右握把，再把右摇杆往前或往后推，把物体推远或拉近。
- 相机符文和望远镜显示在前方的平面屏幕上（和 BetterVR 一样），其中左握把确认、右握把返回。
- 设置 › 操作 › 剧情过场：自动，或所有剧情过场都在前方屏幕上播放。
- 可选的体验存档，在首页或 设置 › 存档 里打开（默认关闭）：作者自己打通的存档，放在单独的文件夹里，不用从头玩就能体验所有内容。

### 修复

- 死亡之山一带天空和远景出现灰绿色花纹。
- 半透明界面元素（如精灵之球计数）下面出现灰色块。
- 相机符文拍出的照片是黑的、取景画面过曝。
- 磁力和静止的目标出现在准星旁边、时有时无。
- 望远镜打的标记点悬在半空。
- 用完符文后游戏几乎卡住（例如站到制冰器造的冰柱上）。
- Link 在水里停下后游戏镜头可能出错，之后游戏变得很卡，地图光标方向也会反。
- 骑马时视角偏在马鞍一侧，往前推摇杆马会扭头。
- 对话中的列表（如马厩选马）摇杆很难上下选择。
- 骑马射箭只能朝前；现在弓跟着手走，箭沿着瞄准线飞。
- 从侧面看远处的敌人和动物时，它们可能定住不动，走近才恢复。
- 往侧面或身后看时，天空被一条竖线分成两种亮度。
- 录屏和截图右边有一条暗色条。
- 启动塔的视频开始播放时游戏崩溃；现在游戏里的视频都能正常播放。
- 走路时偶尔崩溃（游戏发出的一条图形命令出错）：现在跳过那一帧，不再崩溃。
- 读档菜单里的存档缩略图是黑的；现在显示存档前的画面。
- 安装器：把游戏传到头显时，进度条会几分钟不动；现在显示已传多少 GB 和剩余时间。传输中途断开后，残缺的包现在会重新发送，不会再被当成已传完。
- 安装器：如果头显上的 app 被删了，安装器现在会提示重新安装（包括游戏），而不是只显示续签。
- 安装器：用免费 Apple 账号续签，现在会拿到新的 7 天签名；之前续签后到期日不变。
- 在你周围播放的对话不再随镜头摇动而转动视角，减少晕动。
- 对话中打开的地图会闪烁。
- 磁力抓取举着物体时无法转身；现在右摇杆会带着你和物体一起转。快速转身模式下，视角按转身角度一次跳到物体那边，不会因为连续转动而头晕。
- 前方屏幕上的画面（对话框、商店、标题）左握把确认、右握把返回，和菜单一致。

### 已知问题

- 画面清晰度仍受 GPU 预算限制；重场景（克洛格森林、城镇）帧率可能低于 45。
- 如果用完符文后游戏仍然变得很卡，请在安装器里用「帮助 › 导出诊断信息」，附在 [Issue](https://github.com/zlinoliver/BotW-SheikahVision/issues) 里：日志里现在会记下是哪个物理查询慢。
- 连续游玩几分钟、头显发热后，系统可能把刷新率降到 45 Hz，画面会明显卡顿；可以在 Vision Pro 上的 设置 › 画质 里选「流畅」。之后会做自动调整分辨率来解决。
- Apple Vision Pro（M2）尚未测试。
