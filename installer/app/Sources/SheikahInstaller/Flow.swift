import AppKit
import Combine
import SwiftUI

enum SetupStep: Int, CaseIterable, Identifiable {
    case mac, account, game, headset, install
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .mac: return "Mac"
        case .account: return text("Apple 账号", "Apple account")
        case .game: return text("游戏文件", "Game files")
        case .headset: return "Vision Pro"
        case .install: return text("安装", "Install")
        }
    }
}

enum StepState { case done, working, attention, todo, waiting }
enum FlowMode { case welcome, setup, finished, home }

enum TransferConnection {
    case wired, wifi, unknown
    init(transport: String?) {
        switch transport?.lowercased() {
        case "wired", "usb": self = .wired
        case "localnetwork", "wifi", "wireless": self = .wifi
        default: self = .unknown
        }
    }
}

struct TransferEstimate {
    let bytes: Double
    let connection: TransferConnection
    init(bytes: Double?, connection: TransferConnection) {
        self.bytes = bytes.flatMap { $0 > 0 && $0.isFinite ? $0 : nil } ?? 12_000_000_000
        self.connection = connection
    }
    var seconds: Double { bytes / (connection == .wired ? 26_000_000 : 7_000_000) }
    func totalMinutes(build: Int) -> ClosedRange<Int> {
        let fast = max(1, build) + 1 + Int(ceil(bytes / 26_000_000 / 60))
        let slow = max(1, build) + 1 + Int(ceil(bytes / 7_000_000 / 60))
        if connection == .unknown { return fast...slow }
        let value = connection == .wired ? fast : slow
        return value...value
    }
}

func minuteCount(_ count: Int) -> String { text("\(count) 分钟", "\(count) " + (count == 1 ? "minute" : "minutes")) }
func fileCount(_ count: Int) -> String { text("\(count) 个文件", "\(count) " + (count == 1 ? "file" : "files")) }
func signingTitle(_ days: Int?) -> String {
    guard let days else { return text("欢迎回来", "Welcome back") }
    if days <= 0 { return text("签名今天到期", "Signing expires today") }
    return text("签名还剩 \(days) 天", "Signing expires in \(days) " + (days == 1 ? "day" : "days"))
}

struct InstallProgress {
    // Generation, compilation, signing, installation, and game transfer.
    static let stages: [(Set<String>, Double, Double)] = [
        (["plan", "recompile"], 0, 0.05),
        (["provision", "dependencies", "configure", "build"], 0.05, 0.60),
        (["sign", "package"], 0.65, 0.03),
        (["install", "launch", "documents"], 0.68, 0.07),
        (["zip", "push", "push-game"], 0.75, 0.25)
    ]
    static func overall(stage: String, fraction: Double?, elapsed: TimeInterval, transferSeconds: Double) -> Double {
        guard let (_, start, weight) = stages.first(where: { $0.0.contains(stage) }) else { return 0 }
        let local: Double
        if stage == "push", fraction == nil { local = min(0.96, elapsed / transferSeconds) }
        else if stage == "zip" { local = 0 }
        else { local = fraction ?? 0 }
        return min(0.99, start + weight * max(0, min(1, local)))
    }
}

@MainActor
final class WizardFlow: ObservableObject {
    let engine: InstallerModel
    @Published var mode: FlowMode = .welcome
    @Published var step: SetupStep = .mac
    @Published var busy = false
    @Published var toast: String?
    /// set by the home page's Update: the game is only sent if the headset does not have it
    var updating = false
    /// the last install skipped the transfer because the headset already had the game
    @Published var gameKept = false
    /// the launch check is asking the headset whether the app and its game are still there (boot)
    @Published var checkingHeadset = false
    /// the last install found the game package already on the headset and let the app import it
    @Published var gameWaiting = false
    @Published var waitingForApp = false
    @Published var installError: String?
    @Published var totalProgress = 0.0
    @Published var verifiedDevice = ""
    @Published var probingDevices = false
    @Published var probedDevices: [[String: Any]] = []
    private var lastCandidates: [String] = []
    private var lastProbeAt: Date?
    var deviceProbeInterval: TimeInterval = 10
    @Published var reviewing = false
    @Published var now = Date()
    var advanceDelay: UInt64 = 600_000_000
    var pollDelay: UInt64 = 2_000_000_000
    var documentDelay: UInt64 = 3_000_000_000
    private var operation: Task<Void, Never>?
    private var advance: Task<Void, Never>?
    private var observation: AnyCancellable?
    private var started = false
    private var booted = false
    private var unreachableSince: Date?
    private var notOpenedSince: Date?
    private var installing = false
    var unreachableTimeout: TimeInterval = 30
    var manualLaunchDelay: TimeInterval = 180
    var clock: () -> Date = Date.init
    private var completed = false
    var isPreview = false

    init(engine: InstallerModel) {
        self.engine = engine
        engine.useWizardStartup()
        observation = engine.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
        if let data = try? Data(contentsOf: stateFile),
           let saved = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
            started = saved["started"] as? Bool ?? false
            completed = saved["completed"] as? Bool == true && saved["team"] as? String == engine.team && saved["device"] as? String == engine.device
            if saved["accountTeam"] as? String == engine.team, !engine.team.isEmpty { engine.succeeded.insert("account") }
        }
    }

    private var stateFile: URL { engine.work.appendingPathComponent("wizard-state.json") }
    func persist() {
        guard !isPreview else { return }
        let state: [String: Any] = ["started": started, "completed": completed,
                                  "accountTeam": engine.succeeded.contains("account") ? engine.team : "",
                                  "team": engine.team, "device": engine.device, "source": engine.source]
        try? FileManager.default.createDirectory(at: engine.work, withIntermediateDirectories: true)
        try? JSONSerialization.data(withJSONObject: state).write(to: stateFile, options: .atomic)
    }

    func boot() {
        guard !booted, !isPreview else { return }; booted = true
        operation = Task {
            busy = true
            _ = await execute("status")
            // the record says installed; ask the headset (read-only) whether the app and its game are still there. A
            // deleted app went to the renew page, and renewing put back an empty app (user 2026-11-18). Offline, the
            // record stands
            // at most 15 s, with a line on the welcome card: an offline headset made this wait up to about 70 s with the
            // button greyed out and nothing said (Fable's review). A probe cut short leaves the record as it is
            if completed && engine.installed && engine.hasBuild {
                checkingHeadset = true
                var timedOut = false
                let watchdog = Task { @MainActor in
                    try? await Task.sleep(nanoseconds: 15_000_000_000)
                    if !Task.isCancelled && engine.running { timedOut = true; engine.cancel() }
                }
                let answered = await execute("documents")
                watchdog.cancel(); checkingHeadset = false
                if timedOut { engine.error = nil; engine.status = "" }
                if answered && !timedOut && headsetLacksApp {
                    completed = false; persist()
                    engine.installed = false
                    engine.succeeded.subtract(["install", "push-game"])
                }
            }
            busy = false
            if completed && engine.installed && engine.hasBuild { mode = .home }
            else if started { mode = .setup; enter(firstOpenStep) }
        }
    }

    /// after a documents probe: the headset answered, and the app is gone, or has no game and none waiting to import
    var headsetLacksApp: Bool {
        // ready: the headset listed the app's Documents (its reason is then empty, which the model keeps as
        // "unreachable": checked first, or an answered probe read as offline, device 2026-11-18)
        if engine.documentsReady { return !engine.documentsHasGame && !engine.documentsGamePending }
        // the listing failed: only an app the headset says is gone counts (a locked headset or a dropped connection
        // fails the listing too, and must not send a finished install back to resend 13 GB; Fable's review)
        return engine.documentsAppInstalled == false
    }

    var firstOpenStep: SetupStep { SetupStep.allCases.first { state($0) != .done } ?? .install }
    var headsetName: String { engine.devices.first { $0["udid"] as? String == engine.device }?["name"] as? String ?? "Vision Pro" }
    var onlineDevices: [[String: Any]] { probedDevices.filter { $0["paired"] as? Bool == true && $0["reachable"] as? Bool == true } }
    var candidateDevices: [[String: Any]] {
        engine.devices.filter { d in
            let availability = d["availability"] as? String ?? (d["reachable"] as? Bool == true ? "connected" : "offline")
            return d["paired"] as? Bool == true && ["connected", "available"].contains(availability)
        }
    }
    var connectionStatus: String? {
        guard let candidate = candidateDevices.first else { return nil }
        let name = candidate["name"] as? String ?? "Vision Pro"
        let failed = probedDevices.contains { $0["udid"] as? String == candidate["udid"] as? String && $0["probe_error"] is String }
        if !probingDevices && failed {
            return text("\(name) 暂时连不上：请亮屏并保持在附近", "\(name) can’t be reached yet: keep it awake and nearby")
        }
        if probingDevices || onlineDevices.isEmpty { return text("发现 \(name)，正在连接…", "Found \(name), connecting…") }
        return nil
    }
    var deviceVerified: Bool { !verifiedDevice.isEmpty && verifiedDevice == engine.device }
    var rememberedDeviceUnavailable: Bool { !engine.device.isEmpty && !onlineDevices.contains { $0["udid"] as? String == engine.device } }
    var transferEstimate: TransferEstimate {
        let device = engine.devices.first { $0["udid"] as? String == engine.device }
        return TransferEstimate(bytes: (engine.gameInfo["size_bytes"] as? NSNumber)?.doubleValue,
                                connection: TransferConnection(transport: device?["transport"] as? String))
    }
    var installDuration: String {
        let range = transferEstimate.totalMinutes(build: engine.succeeded.contains("build") && engine.hasBuild ? 2 : engine.estimatedBuildMinutes)
        if range.lowerBound == range.upperBound { return text("约 ", "About ") + minuteCount(range.lowerBound) }
        return text("约 \(range.lowerBound)–\(range.upperBound) 分钟", "About \(range.lowerBound)–\(range.upperBound) minutes")
    }
    /// "5.3 / 13.0 GB" while the copy reports its bytes, nil before
    var transferAmount: String? {
        guard let done = engine.transferBytes, let total = engine.transferTotal, total > 0 else { return nil }
        return String(format: "%.1f / %.1f GB", done / 1e9, total / 1e9)
    }
    var transferSubtitle: String {
        if let amount = transferAmount {
            if let eta = engine.eta, eta > 0 {
                let left = eta < 60 ? text("不到 1 分钟", "under a minute") : minuteCount(Int(ceil(Double(eta) / 60)))
                return text("传输游戏 · 已传 \(amount) · 约剩 \(left)", "Transferring game · \(amount) sent · about \(left) left")
            }
            return text("传输游戏 · 已传 \(amount)", "Transferring game · \(amount) sent")
        }
        let elapsed = max(0, now.timeIntervalSince(engine.stepStarted))
        let used = minuteCount(Int(elapsed / 60))
        if elapsed >= transferEstimate.seconds {
            return text("传输游戏 · 已用 \(used) · 仍在传输", "Transferring game · elapsed \(used) · still transferring")
        }
        let expected = minuteCount(Int(ceil(transferEstimate.seconds / 60)))
        return text("传输游戏 · 已用 \(used) · 预计约 \(expected)", "Transferring game · elapsed \(used) · about \(expected) estimated")
    }
    static var disconnectedMessage: String { text("头显已断开。请唤醒头显并重新连接，然后点重试。", "Vision Pro disconnected. Wake it up and reconnect, then try again.") }
    var firstFailure: [String: Any]? {
        engine.checks.first { $0["status"] as? String == "fail" && !["team", "headset"].contains($0["check"] as? String ?? "") }
    }
    var daysRemaining: Int? {
        let formatter = ISO8601DateFormatter()
        let date = formatter.date(from: engine.expiry) ?? ISO8601DateFormatter().date(from: engine.expiry + "T00:00:00Z")
        return date.map { Int(ceil($0.timeIntervalSince(now) / 86400)) }
    }
    /// Stages that talk to the headset (it must be worn and unlocked); compiling on the Mac does not.
    var needsHeadsetNow: Bool { ["install", "launch", "documents", "push", "push-game"].contains(engine.activeStage) }
    var stageTitle: String {
        switch engine.activeStage {
        case "plan", "recompile": return text("生成游戏代码", "Generating game code")
        case "provision": return text("准备签名", "Preparing signing")
        case "configure", "dependencies": return text("准备编译环境", "Preparing the build")
        case "sign", "package": return text("签名", "Signing")
        case "install": return text("安装 app", "Installing the app")
        case "launch", "documents": return text("等待 app 打开", "Waiting for the app")
        case "zip": return text("整理传输包", "Preparing game transfer")
        case "push", "push-game": return text("传输游戏", "Transferring game")
        default: return text("编译", "Compiling")
        }
    }

    func state(_ target: SetupStep) -> StepState {
        if target == step, busy { return .working }
        let done: Bool
        switch target {
        case .mac: done = engine.canContinue(1)
        case .account: done = engine.canContinue(2)
        case .game: done = engine.succeeded.contains("import")
        case .headset: done = deviceVerified
        case .install: done = completed
        }
        if done { return .done }
        if target == step, engine.error != nil || installError != nil || target == .account || target == .game || target == .headset || firstFailure != nil { return .attention }
        if target.rawValue > step.rawValue { return .waiting }
        return .todo
    }

    func start() { guard !busy, !engine.running else { return }; started = true; mode = .setup; persist(); enter(firstOpenStep) }
    func select(_ target: SetupStep) {
        guard !busy, !engine.running, state(target) == .done else { return }
        advance?.cancel(); operation?.cancel(); step = target; reviewing = true
    }
    func continueSetup() { reviewing = false; enter(firstOpenStep) }
    /// Reopen a finished step to change it (another team, game or headset). What depends on it is redone later: a new
    /// team or game resets the build, a new headset the install (InstallerModel's didSet and the import reset below).
    func redo(_ target: SetupStep) {
        guard !busy, !engine.running, !installing, mode == .setup else { return }
        switch target {
        case .account: engine.succeeded.remove("account")
        case .game:
            engine.succeeded.subtract(["import", "build", "install", "push-game"]); engine.hasBuild = false
            engine.imported = ""; engine.gameInfo = [:]
        case .headset: verifiedDevice = ""
        default: break
        }
        persist(); enter(target)
    }
    func enter(_ target: SetupStep) {
        advance?.cancel(); operation?.cancel(); step = target; reviewing = false; engine.error = nil
        guard !isPreview else { return }
        operation = Task { await checkCurrentStep() }
    }
    func becameActive() {
        guard mode == .setup, !busy, !engine.running, !reviewing, [.mac, .account].contains(step) else { return }
        enter(step)
    }
    func retryCheck() { enter(step) }

    private func execute(_ name: String, extra: [String] = []) async -> Bool {
        guard !Task.isCancelled else { return false }
        engine.run(name, extra: extra)
        while engine.running {
            if Task.isCancelled { engine.cancel(); return false }
            tick()
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return !Task.isCancelled && engine.lastCommandOK
    }

    func tick() {
        now = clock()
        guard busy, step == .install else { return }
        let candidate = InstallProgress.overall(stage: engine.activeStage, fraction: engine.fraction,
                                               elapsed: now.timeIntervalSince(engine.stepStarted), transferSeconds: transferEstimate.seconds)
        totalProgress = max(totalProgress, candidate)
    }

    private func checkCurrentStep() async {
        switch step {
        case .mac:
            busy = true; _ = await execute("doctor"); busy = false
            // A missing account is handled on the next card, not as a Mac failure.
            if engine.canContinue(1) { engine.error = nil; didComplete() }
        case .account:
            busy = true; _ = await execute("account", extra: ["--list-only"]); busy = false
            if engine.canContinue(2) { didComplete() }
        case .game:
            if engine.succeeded.contains("import") { didComplete() }
        case .headset:
            verifiedDevice = ""; probedDevices = []; lastCandidates = []; lastProbeAt = nil
            while !Task.isCancelled, step == .headset {
                let listed = await execute("devices")
                guard !Task.isCancelled else { return }
                if !listed { engine.devices = [] }
                let candidates = candidateDevices
                let signature = candidates.map { ($0["udid"] as? String ?? "") + ":" + ($0["transport"] as? String ?? "") }.sorted()
                let ids = Set(candidates.compactMap { $0["udid"] as? String })
                probedDevices.removeAll { !ids.contains($0["udid"] as? String ?? "") }
                let due = lastProbeAt.map { clock().timeIntervalSince($0) >= deviceProbeInterval } ?? true
                if !candidates.isEmpty && (signature != lastCandidates || due) {
                    probingDevices = true
                    let confirmed = await execute("devices", extra: ["--probe"])
                    probingDevices = false
                    guard !Task.isCancelled else { return }
                    probedDevices = confirmed ? engine.devices : candidates.map {
                        var d = $0; d["reachable"] = false; d["probe_error"] = "probe failed"; return d
                    }
                    lastProbeAt = clock()
                }
                lastCandidates = signature
                if onlineDevices.count == 1, let value = onlineDevices[0]["udid"] as? String,
                   engine.device.isEmpty || engine.device == value {
                    engine.device = value; verifiedDevice = value; didComplete(); return
                }
                try? await Task.sleep(nanoseconds: pollDelay)
            }
        case .install: break
        }
    }

    func chooseDevice(_ value: String) {
        guard !busy, !value.isEmpty, onlineDevices.contains(where: { $0["udid"] as? String == value }) else { return }
        engine.device = value; verifiedDevice = value; didComplete()
    }
    func warmSigning() {
        guard !isPreview, !busy, !engine.team.isEmpty else { return }
        operation?.cancel()
        operation = Task {
            busy = true; let ok = await execute("account"); busy = false
            if ok && engine.canContinue(2) { didComplete() }
            else if ok { engine.error = text("请在 Xcode 账号 → 管理证书中添加 Apple Development，再点继续。", "Add Apple Development in Xcode Accounts → Manage Certificates, then continue.") }
        }
    }
    func chooseGame() {
        guard !isPreview else { return }
        if engine.chooseGame() { importSelected() }
    }
    func acceptGameURLs(_ urls: [URL]) {
        guard !isPreview, !busy, mode == .setup, step == .game, !urls.isEmpty else { return }
        engine.gamePaths = urls.filter(\.isFileURL).map(\.path)
        engine.succeeded.subtract(["import", "build", "install", "push-game"]); engine.hasBuild = false; engine.imported = ""; engine.gameInfo = [:]
        if !engine.gamePaths.isEmpty { importSelected() }
    }
    func importSelected() {
        guard !busy else { return }
        operation?.cancel()
        operation = Task {
            busy = true; engine.importGame()
            while engine.running && !Task.isCancelled { try? await Task.sleep(nanoseconds: 50_000_000) }
            busy = false
            if engine.lastCommandOK && !Task.isCancelled { didComplete() }
        }
    }

    private func didComplete() {
        persist(); let from = step
        toast = from.title + text("已准备好", " is ready")
        advance?.cancel()
        advance = Task {
            try? await Task.sleep(nanoseconds: advanceDelay)
            guard !Task.isCancelled, step == from, mode == .setup, !reviewing else { return }
            toast = nil
            if let next = SetupStep(rawValue: from.rawValue + 1) { enter(next) }
        }
    }

    func install() {
        guard !isPreview, !busy, !engine.running, !installing else { return }
        // An advanced/manual action cannot bypass these prerequisites.
        guard SetupStep.allCases.dropLast().allSatisfy({ state($0) == .done }) else { enter(firstOpenStep); return }
        operation?.cancel(); advance?.cancel(); installError = nil; engine.error = nil; gameKept = false; gameWaiting = false
        installing = true
        // like Glovebox: asleep, the Mac can neither build nor reach the headset
        let activity = isPreview ? nil : ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .idleSystemSleepDisabled], reason: "Installing SheikahVision on Apple Vision Pro")
        operation = Task {
            busy = true; totalProgress = 0; unreachableSince = nil; notOpenedSince = nil
            defer { busy = false; waitingForApp = false; installing = false; if let activity { ProcessInfo.processInfo.endActivity(activity) } }
            if !engine.succeeded.contains("build"), !(await awaitStage("build")) { return }
            if !engine.succeeded.contains("install"), !(await awaitStage("install")) { return }
            if !engine.succeeded.contains("push-game") {
                engine.activeStage = "launch"
                let launched = await execute("launch")
                if !launched && engine.errorCode == "E_DEVICE_UNREACHABLE" { installError = Self.disconnectedMessage; return }
                guard !Task.isCancelled else { return }
                engine.error = nil; waitingForApp = true
                engine.activeStage = "documents"
                while !Task.isCancelled {
                    let ok = await execute("documents")
                    now = clock()
                    if ok && engine.documentsReady { break }
                    if ok && engine.documentsReason == "not-opened" {
                        unreachableSince = nil
                        if notOpenedSince == nil { notOpenedSince = now }
                    } else if ok {
                        notOpenedSince = nil
                        if unreachableSince == nil { unreachableSince = now }
                        if let since = unreachableSince, now.timeIntervalSince(since) >= unreachableTimeout {
                            installError = Self.disconnectedMessage; return
                        }
                    }
                    if !ok { installError = engine.cardError ?? text("暂时无法访问头显。", "Cannot reach Vision Pro yet."); return }
                    try? await Task.sleep(nanoseconds: documentDelay)
                }
                waitingForApp = false
                // an update installs over an app that already holds the imported game: do not send 13 GB again.
                // A package already waiting in Import (or being imported) is not sent again either: the app imports it
                // by itself on launch, and a second copy would overwrite the file it is reading.
                gameKept = updating && engine.documentsHasGame
                gameWaiting = !engine.documentsHasGame && engine.documentsGamePending
                if gameKept || gameWaiting {
                    engine.succeeded.insert("push-game")
                } else {
                    guard !Task.isCancelled, await awaitStage("push-game") else { return }
                }
            }
            guard !Task.isCancelled else { return }
            completed = true; updating = false; totalProgress = 1; mode = .finished; persist()
        }
    }
    private func awaitStage(_ name: String) async -> Bool {
        engine.activeStage = name
        let ok = await execute(name)
        if !ok && !Task.isCancelled { installError = engine.cardError ?? text("此步骤未完成，请重试。", "This step did not finish. Try again.") }
        return ok
    }
    var needsManualLaunch: Bool { waitingForApp && notOpenedSince.map { now.timeIntervalSince($0) >= manualLaunchDelay } == true }
    func cancel() {
        operation?.cancel(); advance?.cancel(); engine.cancel(); busy = false; waitingForApp = false
        engine.status = text("已取消。", "Cancelled.")
        if installing {
            installError = text("已取消。重试会复用已完成的编译缓存；传输中断后会整包重传。", "Cancelled. Retry reuses completed build work; interrupted transfers restart the entire zip.")
        }
    }
    func finish() { mode = .home; persist() }
    func reinstall() {
        guard !busy else { return }
        completed = false; updating = true; engine.succeeded.subtract(["build", "install", "push-game"])
        mode = .setup; persist(); enter(.mac)
    }
    func renew() {
        guard !isPreview, !busy, !engine.running, engine.hasBuild else { return }
        operation = Task {
            busy = true; defer { busy = false }
            if await execute("renew") { toast = text("续签完成", "Signing renewed"); persist() }
        }
    }
    func changeLanguage(_ choice: InstallerLanguageChoice) {
        let disconnected = installError == Self.disconnectedMessage
        let cancelled = installError == text("已取消。重试会复用已完成的编译缓存；传输中断后会整包重传。", "Cancelled. Retry reuses completed build work; interrupted transfers restart the entire zip.")
        InstallerLanguage.shared.select(choice)
        engine.refreshImportedSummary()
        if disconnected { installError = Self.disconnectedMessage }
        if cancelled { installError = text("已取消。重试会复用已完成的编译缓存；传输中断后会整包重传。", "Cancelled. Retry reuses completed build work; interrupted transfers restart the entire zip.") }
        toast = nil
        objectWillChange.send()
    }
    func chooseSource() {
        guard !isPreview, !busy, !engine.running else { return }
        let old = engine.source
        engine.chooseSource()
        if old != engine.source {
            engine.succeeded.subtract(["doctor", "build", "install", "push-game"])
            engine.checks = []; engine.hasBuild = false; persist()
        }
    }
    func runAdvanced(_ name: String) {
        guard !isPreview, !busy, !engine.running else { return }
        if name == "import" {
            operation?.cancel(); advance?.cancel(); mode = .setup; step = .game; reviewing = false
            chooseGame(); return
        }
        if name == "renew", mode == .home { renew(); return }
        if ["build", "install", "launch", "documents", "push-game", "renew"].contains(name) {
            guard !engine.team.isEmpty, deviceVerified,
                  engine.succeeded.contains("import"), engine.succeeded.contains("account") else {
                engine.error = text("请先完成账号、游戏文件和头显步骤。", "Complete the account, game files and headset steps first."); return
            }
            if name != "build", !engine.hasBuild {
                engine.error = text("请先完成编译。", "Build the app first."); return
            }
        }
        operation?.cancel()
        operation = Task {
            busy = true; defer { busy = false }
            _ = await execute(name, extra: name == "collect" && !deviceVerified ? ["--local-only"] : [])
        }
    }

}
