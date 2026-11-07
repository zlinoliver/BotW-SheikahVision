import AppKit
import SwiftUI
import WebKit

@main
struct SheikahInstaller {
    @MainActor static func main() {
        let app = NSApplication.shared
        app.appearance = NSAppearance(named: .darkAqua)
        let preview = ProcessInfo.processInfo.arguments.contains("--preview") || ProcessInfo.processInfo.arguments.contains("--capture-ui")
        let engine = InstallerModel(work: preview ? FileManager.default.temporaryDirectory.appendingPathComponent("sv-ui-preview-" + UUID().uuidString) : FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/SheikahVision"), restoreAutomatically: false)
        let flow = WizardFlow(engine: engine)
        if preview { PreviewScenes.apply(ProcessInfo.processInfo.environment["SV_UI_SCENE"] ?? "welcome", to: flow) }
        InstallerWindows.flow = flow
        let windows = InstallerWindows()
        app.delegate = windows
        withExtendedLifetime(windows) { app.run() }
    }
}

final class InstallerWindow: NSWindow {
    override func setFrame(_ frameRect: NSRect, display flag: Bool) {
        super.setFrame(NSRect(origin: frameRect.origin, size: NSSize(width: 900, height: 600)), display: flag)
    }
    override func constrainFrameRect(_ frameRect: NSRect, to screen: NSScreen?) -> NSRect { frameRect }
}

@MainActor
final class InstallerWindows: NSObject, NSApplicationDelegate {
    static var flow: WizardFlow?
    private var main: NSWindow?
    private var advanced: NSWindow?
    func applicationDidFinishLaunching(_ notification: Notification) {
        guard let flow = Self.flow else { return }
        makeMenus()
        NotificationCenter.default.addObserver(self, selector: #selector(languageChanged), name: InstallerLanguage.changed, object: nil)
        let host = NSHostingView(rootView: Wizard().environmentObject(flow))
        host.sizingOptions = []; host.safeAreaRegions = []
        let window = InstallerWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 600),
                              styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.title = text("SheikahVision 安装器", "SheikahVision Installer")
        window.titleVisibility = .hidden; window.titlebarAppearsTransparent = true
        window.contentView = host; window.isReleasedWhenClosed = false
        window.setFrame(NSRect(x: 0, y: 0, width: 900, height: 600), display: false)
        window.minSize = NSSize(width: 900, height: 600); window.maxSize = window.minSize
        window.center(); window.setFrame(NSRect(origin: window.frame.origin, size: NSSize(width: 900, height: 600)), display: false)
        window.makeKeyAndOrderFront(nil); main = window
        NSApp.setActivationPolicy(.regular); NSApp.activate()
        Task {
            try? await Task.sleep(nanoseconds: 200_000_000)
            window.setFrame(NSRect(origin: window.frame.origin, size: NSSize(width: 900, height: 600)), display: true)
            if ProcessInfo.processInfo.arguments.contains("--capture-ui") { await PreviewScenes.capture(flow) }
            else { flow.boot() }
        }
    }
    @objc func languageChanged() {
        makeMenus()
        main?.title = text("SheikahVision 安装器", "SheikahVision Installer")
        advanced?.title = text("高级", "Advanced")
    }
    @objc func openAdvanced() { if let flow = Self.flow { showAdvanced(flow) } }
    @objc func stop() { Self.flow?.cancel() }
    @objc func exportDiagnostics() { if let flow = Self.flow, !flow.busy, !flow.engine.running { flow.engine.exportDiagnostics() } }
    static let releasesPage = URL(string: "https://github.com/zlinoliver/BotW-SheikahVision/releases")!
    @objc func revealGameFiles() { Self.flow?.engine.revealGameFolder() }
    /// The short player guide (Resources/guide-zh.html / guide-en.html) in a window of its own, at a section.
    static func openGuide(_ section: String = "") { (NSApp.delegate as? InstallerWindows)?.showGuide(section) }
    @objc func openGuideMenu() { showGuide("") }
    private var guide: NSWindow?
    func showGuide(_ section: String) {
        guard let file = Bundle.module.url(forResource: text("guide-zh", "guide-en"), withExtension: "html") else { return }
        let view: WKWebView
        if let window = guide, let existing = window.contentView as? WKWebView { view = existing; window.title = text("使用指南", "Guide") }
        else {
            view = WKWebView(frame: NSRect(x: 0, y: 0, width: 700, height: 640))
            view.setValue(false, forKey: "drawsBackground")
            let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .closable, .resizable, .miniaturizable], backing: .buffered, defer: false)
            window.title = text("使用指南", "Guide"); window.contentView = view; window.isReleasedWhenClosed = false
            window.appearance = NSAppearance(named: .darkAqua); window.center(); guide = window
        }
        let target = section.isEmpty ? file : URL(string: file.absoluteString + "#" + section) ?? file
        view.loadFileURL(target, allowingReadAccessTo: file.deletingLastPathComponent())
        guide?.makeKeyAndOrderFront(nil)
    }
    private func makeMenus() {
        let menu = NSMenu()
        let app = NSMenu(title: "SheikahVision")
        app.addItem(withTitle: text("退出 SheikahVision", "Quit SheikahVision"), action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let appItem = NSMenuItem(); appItem.submenu = app; menu.addItem(appItem)
        let edit = NSMenu(title: text("编辑", "Edit"))
        for (title, selector, key) in [(text("剪切", "Cut"), "cut:", "x"), (text("复制", "Copy"), "copy:", "c"), (text("粘贴", "Paste"), "paste:", "v"), (text("全选", "Select All"), "selectAll:", "a")] {
            edit.addItem(withTitle: title, action: Selector(selector), keyEquivalent: key)
        }
        let editItem = NSMenuItem(); editItem.submenu = edit; menu.addItem(editItem)
        let window = NSMenu(title: text("窗口", "Window"))
        window.addItem(withTitle: text("关闭窗口", "Close window"), action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        window.addItem(withTitle: text("最小化", "Minimize"), action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        let advanced = window.addItem(withTitle: text("高级…", "Advanced…"), action: #selector(openAdvanced), keyEquivalent: "a")
        advanced.keyEquivalentModifierMask = [.option, .command]; advanced.target = self
        let stop = window.addItem(withTitle: text("停止当前任务", "Stop current task"), action: #selector(self.stop), keyEquivalent: "."); stop.target = self
        let windowItem = NSMenuItem(); windowItem.submenu = window; menu.addItem(windowItem); NSApp.windowsMenu = window
        let help = NSMenu(title: text("帮助", "Help"))
        let diagnostics = help.addItem(withTitle: text("导出诊断信息…", "Export diagnostics…"), action: #selector(exportDiagnostics), keyEquivalent: ""); diagnostics.target = self
        let guideItem = help.addItem(withTitle: text("使用指南", "Guide"), action: #selector(openGuideMenu), keyEquivalent: "?"); guideItem.target = self
        let files = help.addItem(withTitle: text("在访达中显示游戏文件", "Show Game Files in Finder"), action: #selector(revealGameFiles), keyEquivalent: ""); files.target = self
        let helpItem = NSMenuItem(); helpItem.submenu = help; menu.addItem(helpItem); NSApp.helpMenu = help
        NSApp.mainMenu = menu
    }
    func showAdvanced(_ flow: WizardFlow) {
        if let advanced { advanced.makeKeyAndOrderFront(nil); return }
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 540),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.title = text("高级", "Advanced")
        window.contentView = NSHostingView(rootView: AdvancedView().environmentObject(flow))
        window.isReleasedWhenClosed = false; window.center(); window.makeKeyAndOrderFront(nil); advanced = window
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { Self.flow?.cancel() }
}

struct Wizard: View {
    @ObservedObject private var language = InstallerLanguage.shared
    @EnvironmentObject var flow: WizardFlow
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    var body: some View {
        ZStack(alignment: .bottomLeading) {
            CoverBackdrop()
            page.id("\(flow.mode)-\(flow.step)-\(language.choice.rawValue)")
                .transition(reduceMotion ? .opacity : .opacity.combined(with: .offset(y: 12)))
                .padding(.horizontal, 26).padding(.top, 26).padding(.bottom, 40)  // the footer line has the bottom 30
        }
        .overlay(alignment: .bottom) { InstallerFooter().id(language.choice).padding(.horizontal, 26).padding(.bottom, 11) }
        // the project's name over the landscape (2026-11-11), plain text instead of the old badge capsule; the game is
        // named descriptively ("for"), never as an official title, and the footer says it is unofficial
        .overlay(alignment: .topLeading) { InstallerTitle().id(language.choice).padding(.leading, 34).padding(.top, 50).allowsHitTesting(false) }
        .overlay(alignment: .topTrailing) {
            if flow.mode == .setup { stepBar.id(language.choice).padding(.top, 12).padding(.trailing, 16) }
        }
        .overlay(alignment: .bottomTrailing) {
            Menu {
                ForEach(InstallerLanguageChoice.allCases) { choice in
                    Button { flow.changeLanguage(choice) } label: {
                        if language.choice == choice { Label(choice.label, systemImage: "checkmark") }
                        else { Text(choice.label) }
                    }
                }
            } label: {
                Label("中文 / English", systemImage: "globe").font(.system(size: 12, weight: .semibold))
                    .padding(.horizontal, 12).padding(.vertical, 7).installerGlass(Capsule())
            }.menuStyle(.borderlessButton).fixedSize().foregroundStyle(.white).id(language.choice)
                .accessibilityLabel(text("切换语言", "Change language"))
                .padding(.trailing, 26).padding(.bottom, 40)
        }
        .overlay(alignment: .top) {
            if let toast = flow.toast {
                Label(toast, systemImage: "checkmark.circle.fill").font(.callout)
                    .padding(10).installerGlass(Capsule()).padding(.top, 58).allowsHitTesting(false)
            }
        }
        // the fixed frame itself ignores the title bar's safe area: the cover reaches the window's top edge (traffic
        // lights over it) and nothing is pushed off the bottom
        .frame(width: 900, height: 600).clipped().ignoresSafeArea().preferredColorScheme(.dark).tint(Theme.accent)
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.25), value: flow.step)
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.25), value: flow.mode)
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in flow.becameActive() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in flow.cancel() }
        .onReceive(Timer.publish(every: 1, on: .main, in: .common).autoconnect()) { _ in flow.tick() }
        .dropDestination(for: URL.self) { urls, _ in
            guard flow.mode == .setup, flow.step == .game, !flow.busy, !flow.isPreview else { return false }
            flow.acceptGameURLs(urls); return true
        }
    }
    private var stepBar: some View {
        HStack(spacing: 4) {
            ForEach(SetupStep.allCases) { step in
                Button { flow.select(step) } label: {
                    HStack(spacing: 5) {
                        StatusMark(state: flow.state(step))
                        Text(step.title)
                        if step == flow.step, flow.busy, step == .install {
                            Text("\(Int(flow.totalProgress * 100))%").monospacedDigit()
                        }
                    }
                    .font(.system(size: 11, weight: .medium)).padding(.horizontal, 8).padding(.vertical, 7)
                    .foregroundStyle(flow.step == step ? .white : .white.opacity(0.55))
                    .background(flow.step == step ? .white.opacity(0.12) : .clear, in: Capsule())
                }.buttonStyle(.plain).allowsHitTesting(!flow.busy && !flow.engine.running && flow.state(step) == .done)
            }
        }.padding(4).installerGlass(Capsule())
    }

    @ViewBuilder private var page: some View {
        switch flow.mode {
        case .welcome:
            StepCard(title: text("在 Vision Pro 里走进海拉鲁", "Step into Hyrule on Vision Pro"), subtitle: text("用你自己的 Wii U 正版拷贝，在这台 Mac 上构建。安装器不含游戏代码、资源或密钥，不收集密码，不上传文件。", "Build on this Mac from your own purchased Wii U copy. No game code, assets or keys included. No passwords collected, no files uploaded.")) {
                HStack { Image(systemName: "laptopcomputer"); Text("Apple Silicon · Xcode 26+ · Vision Pro") }.font(.callout).foregroundStyle(.secondary)
                if flow.checkingHeadset {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text(text("正在确认 Vision Pro 上的 SheikahVision 还在（最多 15 秒）…", "Checking SheikahVision on Vision Pro (up to 15 seconds)…"))
                    }.font(.callout).foregroundStyle(.secondary)
                }
            } footer: { primary(text("开始", "Get started")) { flow.start() }.disabled(flow.busy) }
        case .home: homeCard
        case .finished:
            StepCard(title: flow.gameKept ? text("已更新到新版本", "Updated") : text("最后一步：在头显上导入游戏", "Last step: import the game on Vision Pro"),
                     subtitle: flow.gameKept ? text("头显里的游戏和存档都保留了，打开 SheikahVision 就能继续玩。", "The game and saves on the headset are kept; open SheikahVision and play.")
                                             : flow.gameWaiting ? text("游戏包之前已经传到头显，这次没有重传；SheikahVision 会自动导入（几分钟）：", "The game package was already on the headset, so it was not sent again; SheikahVision imports it by itself (a few minutes):")
                                             : text("游戏包已经传到头显，SheikahVision 会自动导入（几分钟）：", "The game package is on the headset, and SheikahVision imports it by itself (a few minutes):")) {
                if !flow.gameKept {
                    VStack(alignment: .leading, spacing: 8) {
                        // free teams must trust the developer before the first launch; paid teams skip this step
                        let free = flow.engine.teams.first { $0.id == flow.engine.team }?.paid == false
                        let steps = (free ? [text("头显「设置 › 通用 › VPN 与设备管理」：信任你的 Apple 账号", "Vision Pro Settings › General › VPN & Device Management: trust your Apple account")] : []) + [
                            text("戴上头显，SheikahVision 已重新打开并开始导入，首页显示进度（没打开的话手动打开它）", "Put on Vision Pro: SheikahVision has reopened and is importing, with progress on its home screen (open it if it isn't)"),
                            text("导入完成后，开始游戏", "When the import finishes, start playing")]
                        ForEach(steps.indices, id: \.self) { i in
                            Label { Text(steps[i]) } icon: { Text("\(i + 1)").font(.system(size: 11, weight: .bold)).foregroundStyle(.black).frame(width: 18, height: 18).background(Theme.glow, in: Circle()) }
                        }
                    }.font(.callout)
                    Text(text("中途退出也没关系，下次打开会接着导。没有自动开始的话：点 Prepare Game › Check and import。", "Leaving midway is fine; it resumes next time. If it doesn't start by itself: Prepare Game › Check and import.")).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            } footer: { primary(text("完成", "Done")) { flow.finish() } }
        case .setup:
            if flow.reviewing { reviewCard }
            else {
                switch flow.step {
                case .mac: macCard
                case .account: accountCard
                case .game: gameCard
                case .headset: headsetCard
                case .install: installCard
                }
            }
        }
    }

    private var reviewCard: some View {
        StepCard(title: flow.step.title, subtitle: reviewSummary) {
            StatusMark(state: .done)
        } footer: {
            primary(text("返回当前步骤", "Back to setup")) { flow.continueSetup() }
            Button(reviewChangeTitle) { flow.redo(flow.step) }.buttonStyle(InstallerSecondaryPill())
        }
    }
    /// What the finished step settled, so the player can see what a change would replace.
    private var reviewSummary: String {
        switch flow.step {
        case .account: return text("签名用的团队：", "Signing team: ") + (flow.engine.teams.first { $0.id == flow.engine.team }?.name ?? flow.engine.team)
        case .game: return flow.engine.imported
        case .headset: return text("要安装的头显：", "Installing on: ") + flow.headsetName
        default: return text("此步骤已完成。", "This step is complete.")
        }
    }
    private var reviewChangeTitle: String {
        switch flow.step {
        case .account: return text("更换账号或团队", "Change account or team")
        case .game: return text("重新选择游戏文件", "Choose game files again")
        case .headset: return text("更换头显", "Change headset")
        default: return text("重新检查", "Check again")
        }
    }
    private var macCard: some View {
        let failure = flow.firstFailure
        let key = failure?["check"] as? String ?? ""
        return StepCard(title: flow.busy ? text("正在检查这台 Mac", "Checking this Mac") : macTitle(key), subtitle: macDetail(failure)) {
            if flow.busy { ProgressView().controlSize(.small) }
            else if let error = flow.engine.cardError, failure == nil { errorLine(error) }
        } footer: {
            if !flow.busy {
                if key != "mac", failure != nil { primary(macAction(key)) { openFix(key) } }
                Button(text("重新检查", "Check again")) { flow.retryCheck() }.buttonStyle(.plain)
            }
        }
    }
    private func macTitle(_ key: String) -> String {
        switch key {
        case "mac": return text("需要 Apple Silicon Mac", "An Apple Silicon Mac is required")
        case "xcode": return text("先准备好 Xcode", "Set up Xcode first")
        case "visionos": return text("添加 visionOS 平台", "Add the visionOS platform")
        case "disk": return text("这台 Mac 的空间不够", "This Mac needs more free space")
        case "tools": return text("还缺少编译工具", "Build tools are missing")
        default: return text("这台 Mac 已准备好", "This Mac is ready")
        }
    }
    private func macDetail(_ failure: [String: Any]?) -> String {
        guard let failure else { return text("检查 Xcode、visionOS 和磁盘空间。通过后会自动继续。", "Checking Xcode, visionOS and disk space. We’ll continue automatically.") }
        let key = failure["check"] as? String ?? ""
        switch key {
        case "mac": return text("这台 Mac 不满足系统要求；需要 Apple Silicon，以及能运行 Xcode 26 的 macOS。", "This Mac does not meet the requirements. Use Apple Silicon and a macOS version supported by Xcode 26.")
        case "xcode": return text("请安装 Xcode 26 或更新版本，打开一次并完成初始设置。", "Install Xcode 26 or later, open it once and finish setup.")
        // what the component is for (user 2026-11-14: players took it for the simulator)
        case "visionos": return text("在 Xcode › Settings › Components 里下载 visionOS。它包含给头显编译 App 必需的 visionOS SDK；会顺带下载几 GB 的模拟器，用不到，但没法单独跳过。",
                                     "Download visionOS in Xcode › Settings › Components. It holds the visionOS SDK, needed to build the app for the headset; the download also brings a few GB of simulator, which is not used but cannot be skipped.")
        case "disk": return text("游戏副本和编译需要 Mac 上的空间。清理后点「重新检查」。", "The game copy and the build need space on this Mac. Free some up, then check again. ") + "\n" + (failure["detail"] as? String ?? "")
        default: return (failure["fix"] as? String ?? failure["detail"] as? String ?? "")
        }
    }
    private func macAction(_ key: String) -> String {
        switch key {
        case "xcode": return text("打开 App Store", "Open App Store")
        case "visionos": return text("打开 Xcode 设置", "Open Xcode settings")
        case "disk": return text("打开 Mac 存储设置", "Open Mac storage settings")
        default: return text("查看安装指引", "View setup instructions")
        }
    }
    private func openFix(_ key: String) {
        guard !flow.isPreview else { return }
        if key == "visionos" { openXcodeSettings() }
        else {
            let link = key == "disk" ? "x-apple.systempreferences:com.apple.settings.Storage" : key == "xcode" ? "macappstore://itunes.apple.com/app/id497799835" : "https://brew.sh"
            if let url = URL(string: link) { NSWorkspace.shared.open(url) }
        }
    }

    private var accountCard: some View {
        StepCard(title: flow.engine.teams.isEmpty ? text("在 Xcode 登录 Apple 账号", "Sign in to Xcode") : text("用你的 Apple 账号签名", "Sign with your Apple account"), subtitle: text("在 Xcode › Settings › Accounts 登录，免费账号即可。", "Sign in under Xcode › Settings › Accounts. A free account works."), width: 570) {
            if flow.engine.teams.count > 1 {
                TeamChooser(teams: flow.engine.teams, selection: teamBinding).disabled(flow.busy)
            } else if let team = flow.engine.teams.first { Text(team.name).font(.headline) }
            // decided 2026-11-11: say it once before installing; no reminders beyond the home page's days remaining
            Label(text("免费账号 7 天后要用本安装器重新签名，存档不会丢。", "Free accounts: re-sign here every 7 days; saves are kept."),
                  systemImage: "clock.arrow.circlepath").font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if !flow.engine.teams.isEmpty {
                Label(text("macOS 弹窗要密码时，输入 Mac 登录密码并点「始终允许」。", "When macOS asks, enter your Mac password and click Always Allow."), systemImage: "key.fill")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if let error = flow.engine.cardError { errorLine(error) }
        } footer: {
            if flow.busy { ProgressView().controlSize(.small); Text(text("正在检查签名…", "Checking signing…")).font(.callout) }
            else if flow.engine.teams.isEmpty { primary(text("打开 Xcode 账号设置", "Open Xcode accounts")) { openXcodeSettings() } }
            else {
                primary(text("继续", "Continue")) { flow.warmSigning() }.disabled(flow.engine.team.isEmpty)
                // another Apple account is added in Xcode; the team list refreshes when the installer is active again
                Button(text("打开 Xcode 账号设置", "Open Xcode accounts")) { openXcodeSettings() }.buttonStyle(InstallerSecondaryPill())
            }
        }
    }
    private var teamBinding: Binding<String> { Binding(get: { flow.engine.team }, set: { flow.engine.team = $0; flow.persist() }) }
    private var gameCard: some View {
        StepCard(title: flow.engine.succeeded.contains("import") ? text("游戏文件已准备好", "Game files are ready") : text("选择你的游戏文件", "Choose your game files"), subtitle: flow.engine.imported.isEmpty ? text("选择已解密的游戏文件夹（含 code、content、meta），也可以直接拖进窗口。", "Choose decrypted game folders (with code, content and meta), or drop them here.") : flow.engine.imported) {
            if !flow.engine.gameInfo.isEmpty { gameSummary }
            if !flow.busy && !flow.engine.succeeded.contains("import") {
                // one line only; where the folders are (Cemu included) is in the README (decided 2026-11-11)
                Text(text("本体和 v208 更新可以一起选，会自动合并。", "Select the base game and the v208 update together; they are combined for you.")).font(.callout)
                Button(text("不知道游戏文件在哪？查看说明", "Where are my game files? Read the guide")) { InstallerWindows.openGuide("game") }
                    .buttonStyle(.plain).font(.callout).foregroundStyle(Theme.accent)
            }
            if flow.busy { HStack { ProgressView().controlSize(.small); Text(text("正在校验并整理文件…", "Checking and preparing files…")) }.font(.callout) }
            if let error = flow.engine.cardError { errorLine(error) }
        } footer: {
            if !flow.busy && !flow.engine.succeeded.contains("import") { primary(flow.engine.error == nil ? text("选择游戏…", "Choose game…") : text("重新选择…", "Choose again…")) { flow.chooseGame() } }
        }
    }
    private var gameSummary: some View {
        HStack(spacing: 16) {
            Label("v208", systemImage: "checkmark.seal.fill")
            Text((flow.engine.gameInfo["file_count"] as? NSNumber).map { fileCount($0.intValue) } ?? text("文件数未知", "File count unavailable"))
            if let bytes = flow.engine.gameInfo["size_bytes"] as? Int64 { Text(ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)) }
        }.font(.callout).foregroundStyle(Theme.accent)
    }
    private var headsetCard: some View {
        StepCard(title: text("连接你的 Vision Pro", "Connect your Vision Pro"), subtitle: text("Vision Pro 和 Mac 连同一个 Wi-Fi，然后：", "Put Vision Pro on the same Wi-Fi as this Mac, then:"), width: 570) {
            if let connection = flow.connectionStatus {
                Text(connection).font(.callout).foregroundStyle(Theme.warning)
            } else if flow.rememberedDeviceUnavailable {
                Text(text("记住的头显尚未连接，正在等待。", "Your saved headset is not connected yet. Waiting…")).font(.callout).foregroundStyle(Theme.warning)
            }
            if flow.onlineDevices.count > 1 || (!flow.onlineDevices.isEmpty && flow.rememberedDeviceUnavailable) {
                Picker(text("选择头显", "Choose a headset"), selection: Binding(get: { flow.onlineDevices.contains { $0["udid"] as? String == flow.engine.device } ? flow.engine.device : "" }, set: { flow.chooseDevice($0) })) {
                    Text(text("请选择", "Choose one")).tag("")
                    ForEach(flow.onlineDevices.indices, id: \.self) { i in Text(flow.onlineDevices[i]["name"] as? String ?? "Vision Pro").tag(flow.onlineDevices[i]["udid"] as? String ?? "") }
                }
            } else {
                Guide(kind: .pairing).font(.caption)
                Button(text("详细步骤", "Detailed steps")) { InstallerWindows.openGuide("pair") }.buttonStyle(.plain).font(.callout).foregroundStyle(Theme.accent)
            }
            if let error = flow.engine.cardError { errorLine(error) }
        } footer: {
            if flow.onlineDevices.count > 1 {
                primary(text("使用这台头显", "Use this headset")) { flow.chooseDevice(flow.engine.device) }
                    .disabled(!flow.onlineDevices.contains { $0["udid"] as? String == flow.engine.device })
            } else {
                ProgressView().controlSize(.small)
                Text(text("正在查找…", "Looking…")).font(.callout).foregroundStyle(.secondary)
            }
        }
    }
    @ViewBuilder private var installCard: some View {
        if flow.busy {
            StepCard(title: text("正在安装到 ", "Installing on ") + flow.headsetName, subtitle: flow.needsManualLaunch ? text("请在头显上打开 SheikahVision。正在等待，不需要重新安装。", "Open SheikahVision on Vision Pro. We’re still waiting; no reinstall is needed.") : flow.engine.transferring ? flow.transferSubtitle : flow.stageTitle + " · " + remainingText, width: 540) {
                HStack(spacing: 22) {
                    ProgressRing(fraction: flow.totalProgress)
                    VStack(alignment: .leading, spacing: 8) {
                        Text("\(Int(flow.totalProgress * 100))%").font(.system(size: 36, weight: .bold, design: .rounded)).monospacedDigit()
                        if flow.engine.transferring, flow.transferAmount != nil {
                            // the bytes on the headset (sv.py TransferProgress), with the average speed
                            Text(flow.engine.status) + Text(text(" · 已用 ", " · elapsed ")) + Text(flow.engine.stepStarted, style: .timer)
                        } else if flow.engine.transferring {
                            Text(text("按时间估算 · 已用 ", "Time estimate · elapsed ")) + Text(flow.engine.stepStarted, style: .timer)
                        } else if flow.needsHeadsetNow {
                            Text(text("请戴上 Vision Pro 并保持解锁，直到完成。", "Put on Vision Pro and keep it unlocked until this finishes."))
                        } else {
                            Text(text("编译时不用戴头显，可以先摘下；要安装时会提示你戴上。", "No need to wear Vision Pro while compiling; we’ll ask you to put it on for the install."))
                        }
                    }.font(.callout).foregroundStyle(.secondary)
                }
            } footer: { Button(text("取消", "Cancel")) { flow.cancel() }.buttonStyle(.bordered).buttonBorderShape(.capsule) }
        } else if let error = flow.installError {
            StepCard(title: text("安装暂时停下来了", "Installation paused"), subtitle: error) { EmptyView() } footer: {
                primary(text("重试", "Try again")) { flow.install() }
                Button(text("导出诊断信息", "Export diagnostics")) { flow.engine.exportDiagnostics() }.buttonStyle(.plain).disabled(flow.engine.running)
            }
        } else {
            StepCard(title: text("准备安装", "Ready to install"), subtitle: flow.installDuration + text("。编译时不用戴头显，安装和传输时要戴上。", ". No headset while compiling; wear it for the install and transfer.")) {
                Label(flow.headsetName, systemImage: "visionpro")
                Text(text("覆盖安装，存档保留。", "Installs over the app; saves are kept.")).font(.callout).foregroundStyle(.secondary)
            } footer: { primary(text("安装", "Install")) { flow.install() } }
        }
    }
    private var remainingText: String { flow.engine.eta.map { String(format: text("约 %d 分钟", "about %d min"), max(1, $0 / 60)) } ?? text("正在估算时间", "estimating time") }
    /// The headset app was built by an older installer than this one: this installer can install the new version.
    /// The headset app is older than this installer: another version, or the same version from older code. Renewing
    /// only re-signs that older app, so the home page offers Install new version instead.
    private var newerHere: String? {
        let current = flow.engine.installerVersion
        if let built = flow.engine.builtVersion, !current.isEmpty, built != current {
            return text("有新版本 \(current) 可以安装，游戏和存档都会保留。", "Version \(current) is ready. Install it to update; your game and saves stay.")
        }
        if flow.engine.sourceChanged {
            return text("有新版本可以安装，游戏和存档都会保留。", "A new version is ready. Install it to update; your game and saves stay.")
        }
        return nil
    }
    private var homeCard: some View {
        let days = flow.daysRemaining
        return StepCard(title: signingTitle(days), subtitle: text("续签只重签并覆盖安装，不编译、不重传游戏。", "Renewing re-signs and reinstalls only, with no build or game transfer."), width: 600) {
            if let reason = newerHere {
                Label(reason, systemImage: "arrow.up.circle.fill").foregroundStyle(Theme.glow).font(.callout).fixedSize(horizontal: false, vertical: true)
            }
            if flow.engine.gameMissing {
                Label(text("Mac 上的游戏副本已删除：续签不受影响；安装新版本时会请你重新选择游戏文件。", "The game copy on this Mac was deleted. Renewing still works; installing a new version will ask for the game folder again."), systemImage: "info.circle").foregroundStyle(.secondary).font(.callout)
            }
            if let days, days <= 2 { Label(text("即将到期，建议现在续签。", "Expires soon. Renew now to keep playing."), systemImage: "exclamationmark.circle.fill").foregroundStyle(Theme.warning).font(.callout) }
            if flow.busy { ProgressView().controlSize(.small) }
            if let error = flow.engine.cardError { errorLine(error) }
        } footer: {
            if newerHere != nil {
                // a newer version is the right next step here: it gets the white button, renewing the old app the outlined one
                primary(text("安装新版本", "Install new version")) { flow.reinstall() }.disabled(flow.busy)
                Button(text("续签", "Renew signing")) { flow.renew() }.buttonStyle(InstallerSecondaryPill()).disabled(flow.busy || flow.engine.running || !flow.engine.hasBuild)
            } else {
                primary(text("续签", "Renew signing")) { flow.renew() }.disabled(flow.busy || flow.engine.running || !flow.engine.hasBuild)
                // new versions are published as new installers on GitHub; download it there, then open it
                Button { if !flow.isPreview { NSWorkspace.shared.open(InstallerWindows.releasesPage) } } label: {
                    Label(text("检查新版本", "Check for updates"), systemImage: "arrow.up.right.square")
                }.buttonStyle(InstallerSecondaryPill())
            }
            if !flow.engine.gameMissing {
                Spacer(minLength: 0)
                // the game copy stays on this Mac (the transfer zip is removed after a transfer); keeping it is the player's call
                Button { if !flow.isPreview { flow.engine.revealGameFolder() } } label: {
                    let size = (flow.engine.gameInfo["size_bytes"] as? NSNumber).map { " · " + ByteCountFormatter.string(fromByteCount: $0.int64Value, countStyle: .file) } ?? ""
                    Label(text("游戏文件", "Game files") + size, systemImage: "folder")
                }.buttonStyle(InstallerSecondaryPill()).help(text("在访达中查看 Mac 上的游戏文件", "Show the game files on this Mac in Finder"))
            }
        }
    }
    private func primary(_ title: String, action: @escaping () -> Void) -> some View {
        Button(title, action: action).buttonStyle(InstallerPill()).tint(.white).buttonBorderShape(.capsule).controlSize(.large)
    }
    private func errorLine(_ message: String) -> some View {
        Text(message).font(.callout).foregroundStyle(Theme.warning).fixedSize(horizontal: false, vertical: true)
    }
    private func openXcodeSettings() {
        guard !flow.isPreview else { return }
        // Xcode does not publish a stable URL for an individual Settings pane.
        if let xcode = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.dt.Xcode") {
            NSWorkspace.shared.open(xcode)
        } else if let store = URL(string: "macappstore://itunes.apple.com/app/id497799835") {
            NSWorkspace.shared.open(store)
        }
        flow.engine.status = text("在 Xcode 菜单选择 Settings › Accounts（平台在 Components）。", "In Xcode, open Settings › Accounts (Components for platforms).")
    }
}

struct AdvancedView: View {
    @ObservedObject private var language = InstallerLanguage.shared
    @EnvironmentObject var flow: WizardFlow
    @State private var selectedCommand = "doctor"
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(text("高级工具", "Advanced tools")).font(.title2.bold())
            Text(flow.engine.source).font(.caption).textSelection(.enabled)
            HStack {
                Button(text("选择源码目录…", "Choose source folder…")) { flow.chooseSource() }
                Button(text("复制命令", "Copy command")) {
                    NSPasteboard.general.clearContents(); NSPasteboard.general.setString(flow.engine.command, forType: .string)
                }
            }.disabled(flow.busy || flow.engine.running || flow.isPreview)
            TextField(text("设备 UDID", "Device UDID"), text: Binding(get: { flow.engine.device }, set: { flow.engine.device = $0 })).disabled(flow.busy || flow.engine.running)
            HStack {
                Picker(text("手动重跑", "Run a step"), selection: $selectedCommand) {
                    ForEach(["doctor", "account", "import", "devices", "status", "build", "install", "launch", "documents", "push-game", "renew", "collect"], id: \.self) { Text($0).tag($0) }
                }
                Button(text("运行", "Run")) { flow.runAdvanced(selectedCommand) }
            }.disabled(flow.busy || flow.engine.running || flow.isPreview)
            Text(text("手动运行仍会检查前置条件。钥匙串授权仅在系统窗口中进行。", "Manual commands still check prerequisites. Keychain authorization stays in system dialogs.")).font(.caption).foregroundStyle(.secondary)
            ScrollView { Text(flow.engine.log).font(.system(.caption, design: .monospaced)).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled) }
            if let error = flow.engine.cardError { Text(error).foregroundStyle(Theme.warning) }
            if flow.engine.running { Button(text("取消", "Cancel")) { flow.cancel() } }
        }.padding(24).preferredColorScheme(.dark).id(language.choice)
    }
}
