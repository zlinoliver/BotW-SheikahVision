import Foundation

/// Chinese for the engine's English messages (sv.py, gameimport.py) shown in the window: errors, step details and
/// progress lines (user 2026-11-18: retries and errors were English in the Chinese window). Each pattern matches the
/// whole message; $1, $2... take its captured parts (paths, numbers, codes). An unknown message stays as it is.
enum EngineText {
    private static let table: [(String, String)] = [
        // transfer, connection
        (#"attempt 1/(\d+)"#, "正在连接 Vision Pro…"),
        (#"attempt (\d+)/(\d+)"#, "第 $1/$2 次尝试"),
        (#"the Vision Pro dropped the connection; trying again"#, "Vision Pro 连接断开，正在重试"),
        (#"failed \((.*)\); retrying in (\d+) s"#, "失败（$1），$2 秒后重试"),
        (#"copy failed (\d+) times; run push-game again to retransmit the entire zip \(no byte-level resume\): (.*)"#,
         "传输失败 $1 次，请重试，整个游戏包会重新发送：$2"),
        (#"copied; open the app on the headset and start the import"#, "已传到头显，打开 app 即可开始导入"),
        (#"restarted SheikahVision on the headset; it imports the game by itself"#, "已在头显上重启 SheikahVision，它会自动导入游戏"),
        (#"reusing the up-to-date (.*) \((.*) GB\)"#, "复用已打好的游戏包（$2 GB）"),
        (#"Vision Pro disconnected\. Wake it up and reconnect, then try again\."#, "Vision Pro 已断开。请唤醒并重新连接头显，然后重试。"),
        (#"Xcode did not return usable device information\. Unlock and reconnect Vision Pro, then try again\."#,
         "Xcode 没有返回可用的设备信息。请解锁并重新连接 Vision Pro，然后重试。"),
        (#"the app has no accessible Documents folder: unlock and connect the headset, open SheikahVision once, then try again"#,
         "暂时访问不到 app 的文件夹：请解锁并连接头显，打开一次 SheikahVision，然后重试"),
        (#"(.*) is not installed on the headset: run 'sv install' first"#, "头显上还没有安装这个 app，请先完成安装"),
        (#"installed; open the app once on the headset before push-game"#, "已安装；传游戏前请先在头显上打开一次 app"),
        // signing, account, headset pairing
        (#"no Apple team found in Xcode: sign in under Xcode > Settings > Accounts, or pass --team"#,
         "Xcode 里没有 Apple 账号团队：请在 Xcode › Settings › Accounts 登录"),
        (#"more than one Apple team is available: select a team explicitly with --team"#, "有多个 Apple 团队，请先选择一个"),
        (#"choose a valid 10-character Apple team ID from Xcode > Settings > Accounts"#, "请选择有效的 Apple 团队（Xcode › Settings › Accounts）"),
        (#"macOS is waiting for permission to use your signing key: answer its window with your password and Always Allow"#,
         "macOS 正在等待签名钥匙的授权：请在弹窗里输入 Mac 密码，并点「始终允许」"),
        (#"macOS did not let this Mac sign with your key \((.*)"#, "macOS 不允许用你的签名钥匙签名（$1"),
        (#"Connect and unlock Vision Pro, enable Developer Mode and pair it in Xcode first\. Personal Teams need a registered headset before signing\."#,
         "请先连接并解锁 Vision Pro，打开开发者模式，并在 Xcode 里配对。免费个人团队需要先登记头显才能签名。"),
        (#"no valid profile for the selected team, capabilities and headset: connect and pair Vision Pro in Xcode, then build again"#,
         "所选团队没有可用于这台头显的描述文件：请在 Xcode 里连接并配对 Vision Pro，然后重新构建"),
        (#"no paired Vision Pro: pair it in Xcode first, or pass --device <udid>"#, "没有已配对的 Vision Pro：请先在 Xcode 里配对"),
        (#"more than one Vision Pro is paired: pass --device <udid> \((.*)"#, "配对了多台 Vision Pro，请先选择一台（$1"),
        (#"Renew must use the original app team; build a separate app to change teams\."#, "续签必须使用原来的团队；要换团队，请重新构建一个 app。"),
        (#"no recorded team: run 'sv build' first or pass --team"#, "还没有记录签名团队，请先完成构建"),
        (#"No signing team recorded: build with your selected team first\."#, "还没有记录签名团队，请先用所选团队完成构建。"),
        (#"the built app is missing: run build before renewing"#, "找不到已构建的 app，请先重新构建再续签"),
        (#"no signing key for team (.*) yet: in Xcode Settings > Accounts > Manage Certificates, add Apple Development, then warm(.*)"#,
         "团队 $1 还没有签名钥匙：请在 Xcode › Settings › Accounts › Manage Certificates 里添加 Apple Development"),
        // build, game
        (#"nothing built yet: run 'sv build' first.*"#, "还没有构建，请先完成构建"),
        (#"(.*) is missing: run 'sv build'"#, "找不到已构建的 app，请重新构建"),
        (#"no imported game: run 'sv import <path>' first"#, "还没有导入游戏，请先选择游戏文件夹"),
        (#"the imported game changed: import it again before transferring"#, "导入的游戏有变化，请重新导入后再传输"),
        (#"the imported game contains links: import it again"#, "导入的游戏里有链接文件，请重新导入"),
        (#"source build needs headers: brew install (.*)"#, "从源码构建需要先安装：brew install $1"),
        (#"the public source manifest is missing: choose a complete source checkout or reinstall the installer"#,
         "源码清单缺失：请选择完整的源码，或重新下载安装器"),
        (#"U-King\.rpx -> C\+\+ \(a few minutes\)"#, "U-King.rpx 转成 C++（需要几分钟）"),
        (#"(\d+) translation units"#, "$1 个编译单元"),
        (#"cache hit: (.*)"#, "复用缓存：$1"),
        (#"looking for the game folders"#, "正在查找游戏文件夹"),
        (#"checking: (.*)"#, "正在检查：$1"),
        (#"copying (.*)"#, "正在复制 $1"),
        (#"(.*) does not exist"#, "$1 不存在"),
        (#"(.*) is a Cemu \.wua archive.*"#, "$1 是 Cemu 的 .wua 压缩包，安装器读不了；请选择包含 code、content、meta 的游戏文件夹"),
        (#"(.*) is a disc image, which is not supported.*"#, "$1 是光盘镜像，不支持；请选择解密后、包含 code、content、meta 的游戏文件夹"),
        (#"(.*) is not a folder: choose the decrypted game folder that contains code, content and meta"#,
         "$1 不是文件夹：请选择解密后、包含 code、content、meta 的游戏文件夹"),
        (#"no game folder \(code/, content/, meta/\) found in (.*) or up to 3 levels below it"#, "在 $1 及其下 3 层里没有找到游戏文件夹（code、content、meta）"),
        (#"only DLC was found; the base game \(code/, content/, meta/\) is needed"#, "只找到 DLC；需要本体游戏（code、content、meta）"),
        (#"the folders are from different regions \((.*)\); import one region at a time"#, "这些文件夹来自不同地区（$1），请一次只导入一个地区"),
        (#"not a complete game folder, missing: (.*)"#, "游戏文件夹不完整，缺少：$1"),
        (#"meta/meta\.xml has no title id: this is not Breath of the Wild for Wii U"#, "meta/meta.xml 里没有标题 ID：这不是 Wii U 版《旷野之息》"),
        (#"title id (.*) is not Breath of the Wild for Wii U.*"#, "标题 ID $1 不是 Wii U 版《旷野之息》（应为日版 101C9300、美版 101C9400 或欧版 101C9500）"),
        (#"title_version is (\d+), not 208.*"#, "游戏版本是 $1，不是 208（v208 即 1.5.0 更新）：请同时选择 v208 更新文件夹和本体"),
        (#"code/U-King\.rpx has SHA-256 .*"#, "code/U-King.rpx 校验不通过：文件被修改、损坏，或不是 v208 的可执行文件，请重新提取"),
        (#"no language pack \(content/Pack/Bootup_<lang>\.pack\) found"#, "没有找到语言包（content/Pack/Bootup_<语言>.pack）"),
        (#"language packs (.*) do not belong to the (.*) title id (.*)"#, "语言包 $1 不属于 $2 版（标题 ID $3）"),
        (#"need (.*) GB free in (.*), have (.*) GB"#, "需要 $1 GB 可用空间，现在只有 $3 GB"),
        (#"the copied U-King\.rpx differs from the source"#, "复制后的 U-King.rpx 和原文件不一致，请重试"),
        (#"(.*) files, (.*) GB \((.*)\) in (.*)"#, "$1 个文件，$2 GB"),
        (#"refusing to run a devicectl command with (.*): this tool never deletes on the headset"#, "已拒绝执行带 $1 的命令：安装器从不删除头显上的内容"),
        (#"(.*) failed \((\d+)\): (.*)"#, "$1 失败（$2）：$3"),
    ]
    private static let compiled: [(NSRegularExpression, String)] = table.compactMap { pattern, zh in
        (try? NSRegularExpression(pattern: "^" + pattern + "$", options: [.dotMatchesLineSeparators])).map { ($0, zh) }
    }

    /// the message in the window's language
    static func localized(_ message: String) -> String {
        text(chinese(message) ?? message, message)
    }

    /// the Chinese for an engine message, nil when it has none
    static func chinese(_ message: String) -> String? {
        let range = NSRange(message.startIndex..., in: message)
        for (regex, zh) in compiled {
            guard let m = regex.firstMatch(in: message, range: range) else { continue }
            var out = zh
            for i in stride(from: m.numberOfRanges - 1, through: 1, by: -1) {
                let part = Range(m.range(at: i), in: message).map { String(message[$0]) } ?? ""
                out = out.replacingOccurrences(of: "$\(i)", with: part)
            }
            return out
        }
        return nil
    }
}
