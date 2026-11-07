import AppKit
import SwiftUI

func text(_ zh: String, _ en: String) -> String {
    InstallerLanguageChoice.resolve(saved: UserDefaults.standard.string(forKey: "installerLanguage"), environment: ProcessInfo.processInfo.environment["SV_UI_LANGUAGE"], preferred: Locale.preferredLanguages.first) == "zh" ? zh : en
}

struct Team: Identifiable {
    let id: String
    let name: String
    let paid: Bool
}

@MainActor
final class InstallerModel: ObservableObject {
    @Published var source = ""
    @Published var team = "" {
        didSet {
            if team != oldValue {
                for step in ["account", "build", "install", "push-game"] { succeeded.remove(step) }
                hasBuild = false; installed = false
            }
        }
    }
    @Published var teams: [Team] = []
    @Published var devices: [[String: Any]] = []
    @Published var gameInfo: [String: Any] = [:]
    @Published var activeStage = ""
    @Published var documentsReady = false
    @Published var documentsReason = ""
    @Published var documentsHasGame = false
    /// the game package is already on the headset (waiting in Import, or being imported): it is not sent again
    @Published var documentsGamePending = false
    /// from the documents probe: true / false when the headset answered whether the app is there, nil when unknown
    @Published var documentsAppInstalled: Bool?
    /// the repository VERSION of this installer, and of the one that built the app now on the headset (nil when unknown)
    @Published var installerVersion = ""
    @Published var builtVersion: String?
    /// the headset app was built from other source than this installer carries (also when VERSION is the same)
    @Published var sourceChanged = false
    /// status found the imported game copy deleted or damaged (renewing still works)
    @Published var gameMissing = false
    private(set) var errorCode = ""
    private var automaticRestore = false
    @Published var estimatedBuildMinutes = 17
    private var compilationStarted = false
    var lastCommandOK = false
    var simulate: ((String, [String], InstallerModel) -> Void)?
    @Published var device = "" {
        didSet {
            installed = device.isEmpty ? !installedDevices.isEmpty : installedDevices[device] != nil
            if device != oldValue { succeeded.subtract(["install", "push-game"]) }
        }
    }
    private var installedDevices: [String: Any] = [:]
    @Published var gamePaths: [String] = []
    @Published var running = false
    @Published var command = ""
    @Published var log = ""
    @Published var status = ""
    @Published var fraction: Double?
    @Published var eta: Int?
    @Published var expiry = ""
    @Published var imported = ""
    @Published var succeeded: Set<String> = []
    @Published var checks: [[String: Any]] = []
    @Published var error: String?
    @Published var installed = false
    @Published var hasBuild = false
    private var diagnosticQueue: [String] = []
    private var exportingDiagnostics = false
    private var collectedDirectory: URL?
    private var persistedLogBytes = 0
    @Published var stepStarted = Date()
    @Published var transferring = false
    /// the game zip's bytes on the headset and its size, while it is copied (sv.py TransferProgress); nil before the first sample
    @Published var transferBytes: Double?
    @Published var transferTotal: Double?
    private var process: Process?
    private var outputTask: Task<Void, Never>?
    private var cancelled = false
    let work: URL
    private let diagnosticsDestination: URL?
    // Reserved integration points; no reminders, updater or resident menu agent this release.
    let renewalReminderEnabled = false
    let automaticUpdatesEnabled = false
    let menuAgentEnabled = false

    init(work: URL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/SheikahVision"), restoreAutomatically: Bool = true, diagnosticsDestination: URL? = nil) {
        self.work = work
        self.automaticRestore = restoreAutomatically
        self.diagnosticsDestination = diagnosticsDestination
        source = ProcessInfo.processInfo.environment["SV_SOURCE"] ?? ""
        if source.isEmpty, let bundled = Bundle.main.resourceURL?.appendingPathComponent("source"),
           FileManager.default.fileExists(atPath: bundled.appendingPathComponent("installer/engine/sv.py").path) {
            let target = work.appendingPathComponent("source")
            do {
                try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
                let stamp = (try? String(contentsOf: bundled.appendingPathComponent("source-version"))) ?? "unknown"
                if (try? String(contentsOf: target.appendingPathComponent("source-version"))) != stamp {
                    // Source copies contain public files only. Game/cache/save paths are siblings, never inside this tree.
                    if FileManager.default.fileExists(atPath: target.path) { try FileManager.default.removeItem(at: target) }
                    try FileManager.default.copyItem(at: bundled, to: target)
                }
                source = target.path
            } catch { self.error = error.localizedDescription }
        }
        if let data = try? Data(contentsOf: work.appendingPathComponent("build.json")),
           let state = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
            team = state["team"] as? String ?? ""
            device = state["selected_device"] as? String ?? ""
            expiry = state["expires"] as? String ?? ""
            hasBuild = (state["app"] as? String).map { FileManager.default.fileExists(atPath: $0 + "/Info.plist") } ?? false
            // Restored build/import status is confirmed by the read-only status command below.
        }
        if let data = try? Data(contentsOf: work.appendingPathComponent("wizard-state.json")),
           let state = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
            if let selected = state["source"] as? String,
               ProcessInfo.processInfo.environment["SV_SOURCE"] == nil,
               FileManager.default.fileExists(atPath: selected + "/installer/engine/sv.py") { source = selected }
            if let selected = state["team"] as? String { team = selected }
            if let selected = state["device"] as? String { device = selected }
        }
        if restoreAutomatically, FileManager.default.fileExists(atPath: source + "/installer/engine/sv.py") {
            Task { [weak self] in
                guard let self, self.automaticRestore else { return }
                self.run("status")
            }
        }

    }

    func useWizardStartup() { automaticRestore = false }

    /// The imported game copy (work/game); the player decides in Finder whether to keep it.
    var gameFolder: URL { work.appendingPathComponent("game") }
    var hasGameFolder: Bool { FileManager.default.fileExists(atPath: gameFolder.appendingPathComponent("code").path) }
    func revealGameFolder() {
        if hasGameFolder { NSWorkspace.shared.activateFileViewerSelecting([gameFolder]) }
        else { NSWorkspace.shared.open(work) }
    }

    func chooseSource() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false
        if panel.runModal() == .OK, let url = panel.url { source = url.path }
    }

    func chooseGame() -> Bool {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.allowsMultipleSelection = true
        panel.message = text("选择已解密的游戏文件夹（含 code、content、meta），可同时选择本体和 v208 更新。", "Choose decrypted game folders (with code, content and meta); select the base game and the v208 update together.")
        if panel.runModal() == .OK {
            gamePaths = panel.urls.map(\.path)
            succeeded.subtract(["import", "build", "install", "push-game"]); hasBuild = false; imported = ""; gameInfo = [:]
            return true
        }
        return false
    }

    func run(_ name: String, extra: [String] = []) {
        guard !running else { return }
        lastCommandOK = false; errorCode = ""
        if name == "build" { compilationStarted = false }
        if let simulate {
            running = true; error = nil; fraction = nil; eta = nil
            if name == "doctor" { checks = [] }
            simulate(name, extra, self)
            return
        }
        guard FileManager.default.fileExists(atPath: URL(fileURLWithPath: source).appendingPathComponent("installer/engine/sv.py").path) else {
            error = text("请先选择 SheikahVision 源码目录。", "Choose the SheikahVision source folder first."); return
        }
        var args = ["python3", source + "/installer/engine/sv.py", name, "--json", "--work", work.path]
        if !team.isEmpty { args += ["--team", team] }
        if !device.isEmpty { args += ["--device", device] }
        args += extra
        if ["doctor", "build"].contains(name), let kit = runtimeKit {
            args += ["--runtime", kit]
        }
        // No shell evaluation: paths and user selections are separate arguments.
        command = (["xcrun"] + args).map { "'" + $0.replacingOccurrences(of: "'", with: "'\\''") + "'" }.joined(separator: " ")
        log += "\n$ " + command + "\n"
        persistLog()
        running = true; cancelled = false; fraction = nil; eta = nil; error = nil
        status = text("正在运行…", "Running…")
        if name == "doctor" { checks = [] }
        let p = Process(), pipe = Pipe()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        p.arguments = args; p.currentDirectoryURL = URL(fileURLWithPath: source)
        var env = ProcessInfo.processInfo.environment
        env["PYTHONUNBUFFERED"] = "1"
        p.environment = env
        p.standardOutput = pipe; p.standardError = pipe; p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch {
            running = false; self.error = error.localizedDescription; return
        }
        process = p
        outputTask = Task {
            do {
                for try await line in pipe.fileHandleForReading.bytes.lines {
                    receive(line, name: name)
                }
            } catch { self.error = error.localizedDescription }
            p.waitUntilExit()
            lastCommandOK = lastCommandOK && p.terminationStatus == 0 && !cancelled
            running = false; process = nil
            if cancelled { status = text("已取消；下次运行会复用已完成的缓存。", "Cancelled; completed cache work will be reused next time.") }
            else if p.terminationStatus != 0, self.error == nil {
                self.error = text("命令未完成。请查看下方日志。", "The command did not finish. See the log below.")
            }
            persistLog()
            if exportingDiagnostics {
                if let next = diagnosticQueue.first {
                    diagnosticQueue.removeFirst(); run(next)
                } else {
                    exportingDiagnostics = false; saveDiagnostics()
                }
            }
        }
    }

    func receive(_ line: String, name: String) {
        log += line + "\n"
        persistLog()
        guard let data = line.data(using: .utf8), let e = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let event = e["event"] as? String else { return }
        if name == "doctor" {
            let file = work.appendingPathComponent("diagnostics/doctor.jsonl")
            if let handle = try? FileHandle(forWritingTo: file) {
                defer { try? handle.close() }
                _ = try? handle.seekToEnd(); try? handle.write(contentsOf: Data((line + "\n").utf8))
            } else { try? Data((line + "\n").utf8).write(to: file) }
        }
        if event == "teams", let list = e["teams"] as? [[String: Any]] {
            teams = list.compactMap { t in
                guard let id = t["id"] as? String else { return nil }
                return Team(id: id, name: t["name"] as? String ?? id, paid: t["paid"] as? Bool ?? false)
            }
            if !teams.contains(where: { $0.id == team }) { team = teams.count == 1 ? teams[0].id : "" }
        }
        if event == "devices", let devices = e["devices"] as? [[String: Any]] {
            self.devices = devices.filter { $0["paired"] as? Bool == true }

        }
        if event == "step" {
            activeStage = e["step"] as? String ?? name
            fraction = nil; eta = nil; stepStarted = Date()
            transferring = e["step"] as? String == "push" && e["status"] as? String == "running"
            transferBytes = nil; transferTotal = nil
        }
        if event == "progress", e["step"] as? String == "push",
           let done = (e["bytes"] as? NSNumber)?.doubleValue, let total = (e["total_bytes"] as? NSNumber)?.doubleValue, total > 0 {
            transferBytes = done; transferTotal = total
        }
        if event == "command", let stage = e["step"] as? String {
            if stage == "build" { compilationStarted = true }
            activeStage = name == "build" && stage == "sign" && !compilationStarted ? "provision" : stage
        }
        if event == "done" {
            lastCommandOK = e["ok"] as? Bool == true
            if name == "documents" {
                documentsReady = e["ready"] as? Bool == true
                documentsReason = e["reason"] as? String ?? "unreachable"
                documentsHasGame = e["has_game"] as? Bool == true
                documentsGamePending = e["game_pending"] as? Bool == true
                documentsAppInstalled = e["app_installed"] as? Bool
            }
        }
        if event == "step", e["step"] as? String == "plan", let detail = e["detail"] as? String,
           let bytes = detail.data(using: .utf8), let plan = (try? JSONSerialization.jsonObject(with: bytes)) as? [String: Any],
           let minutes = plan["estimated_build_minutes"] as? Int { estimatedBuildMinutes = minutes }
        if let minutes = e["estimated_build_minutes"] as? Int { estimatedBuildMinutes = minutes }
        if event == "check" { checks.append(e) }
        if let percent = e["percent"] as? Double { fraction = min(1, max(0, percent / 100)) }
        if let seconds = e["eta_seconds"] as? Int { eta = seconds }
        if let speed = e["bytes_per_second"] as? Double, speed > 0 {
            status = String(format: text("平均传输速度 %.1f MB/s", "Average transfer speed %.1f MB/s"), speed / 1e6)
        } else if let detail = e["detail"] as? String { status = EngineText.localized(detail) }
        else if let line = e["line"] as? String { status = EngineText.localized(line) }
        if event == "error" { errorCode = e["code"] as? String ?? ""; error = text("此步骤未完成：", "This step could not finish: ") + EngineText.localized(e["message"] as? String ?? "") }
        if event == "done", e["ok"] as? Bool == true {
            if name != "account" || (e["warmed"] as? Bool == true && e["team"] as? String == team) {
                succeeded.insert(name)
            }
            if name == "status" {
                let sameTeam = e["team"] as? String == team
                installerVersion = e["installer_version"] as? String ?? installerVersion
                builtVersion = e["built_version"] as? String
                sourceChanged = e["source_changed"] as? Bool == true
                gameMissing = e["game_missing"] as? Bool == true
                hasBuild = (e["built"] as? Bool ?? false) && sameTeam
                if hasBuild && e["team"] as? String == team { succeeded.insert("build") }
                let devices = e["installed_devices"] as? [String: Any] ?? [:]
                installedDevices = sameTeam ? devices : [:]
                installed = device.isEmpty ? !installedDevices.isEmpty : installedDevices[device] != nil
                if installed { succeeded.insert("install") }
                if e["game"] is [String: Any] { succeeded.insert("import") }
            }
            if name == "install" { installed = true }
            if name == "build" { hasBuild = true; succeeded.subtract(["install", "push-game"]) }
            if name == "collect", let path = e["dir"] as? String { collectedDirectory = URL(fileURLWithPath: path) }
            if let date = e["expires"] as? String { expiry = date }
            if let game = e["game"] as? [String: Any] {
                gameInfo = game
                refreshImportedSummary()
            }
        }
    }

    func refreshImportedSummary() {
        guard !gameInfo.isEmpty else { return }
        let region = gameInfo["region"] as? String ?? ""
        let label = ["JPN": text("日版", "Japan"), "USA": text("美版", "USA"), "EUR": text("欧版", "Europe")][region] ?? region
        let patch = gameInfo["text_patch"] as? String == "zh-Hans" ? text("（简体中文汉化）", " (Simplified Chinese patch)") : ""
        imported = "\(label)\(patch) · v\(gameInfo["title_version"] ?? "?") · " + text("v208 RPX 哈希校验通过", "v208 RPX hash verified")
        if let dlc = gameInfo["dlc_ignored"] as? [String], !dlc.isEmpty { imported += "\n" + text("DLC 暂不支持，已忽略", "DLC is not supported yet; ignored") }
    }

    var cardError: String? {
        guard let error else { return nil }
        if error.count <= 220 { return error }
        return text("此步骤未完成。请在「窗口 › 高级」查看具体原因，或导出诊断信息。", "This step could not finish. See Window › Advanced for the detailed reason, or export diagnostics.")
    }

    func cancel() {
        exportingDiagnostics = false; diagnosticQueue = []; transferring = false; transferBytes = nil; transferTotal = nil
        persistLog()
        cancelled = true
        if simulate != nil { running = false; lastCommandOK = false }
        if process?.isRunning == true { process?.interrupt() }
    }

    func importGame() {
        run("import", extra: gamePaths)
    }

    var runtimeKit: String? {
        if let path = ProcessInfo.processInfo.environment["SV_RUNTIME_KIT"] { return path }
        if let path = Bundle.main.resourceURL?.appendingPathComponent("runtime-kit"),
           FileManager.default.fileExists(atPath: path.appendingPathComponent("runtime.json").path) { return path.path }
        return nil
    }

    func canContinue(_ page: Int) -> Bool {
        switch page {
        case 1: return checks.contains { $0["check"] as? String == "disk" } && !checks.contains {
            $0["status"] as? String == "fail" && !["team", "headset"].contains($0["check"] as? String ?? "")
        }
        case 2: return !team.isEmpty && succeeded.contains("account")
        case 3: return succeeded.contains("import")
        case 4: return !device.isEmpty
        case 5: return succeeded.contains("build")
        case 6: return succeeded.contains("push-game")
        default: return true
        }
    }

    func persistLog() {
        let data = Data(log.utf8)
        guard data.count > persistedLogBytes else { return }
        let dir = work.appendingPathComponent("diagnostics")
        let file = dir.appendingPathComponent("wizard.log")
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            if let size = try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize, size > 2 << 20 {
                let old = dir.appendingPathComponent("wizard.prev.log")
                if FileManager.default.fileExists(atPath: old.path) { try FileManager.default.removeItem(at: old) }
                try FileManager.default.moveItem(at: file, to: old)
            }
            if !FileManager.default.fileExists(atPath: file.path) { FileManager.default.createFile(atPath: file.path, contents: nil) }
            let handle = try FileHandle(forWritingTo: file)
            defer { try? handle.close() }
            try handle.seekToEnd(); try handle.write(contentsOf: data.dropFirst(persistedLogBytes))
            persistedLogBytes = data.count
        } catch { self.error = error.localizedDescription }
    }

    func writeDiagnostics(to output: URL) throws {
        persistLog()
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        for name in ["wizard.log", "wizard.prev.log"] {
            let file = work.appendingPathComponent("diagnostics/" + name)
            if FileManager.default.fileExists(atPath: file.path) { try FileManager.default.copyItem(at: file, to: output.appendingPathComponent(name)) }
        }
        for (source, destination) in [("build.log", "build-tail.log"), ("provision.log", "provision-tail.log"), ("diagnostics/doctor.jsonl", "doctor.jsonl")] {
            if let handle = try? FileHandle(forReadingFrom: work.appendingPathComponent(source)) {
                defer { try? handle.close() }
                let size = try handle.seekToEnd()
                try handle.seek(toOffset: size > 256 << 10 ? size - UInt64(256 << 10) : 0)
                try handle.readToEnd()?.write(to: output.appendingPathComponent(destination))
            }
        }
        let feedback = work.appendingPathComponent("feedback")
        let latest = collectedDirectory ?? ((try? FileManager.default.contentsOfDirectory(at: feedback, includingPropertiesForKeys: nil)) ?? []).sorted { $0.lastPathComponent > $1.lastPathComponent }.first
        if let latest {
            let dest = output.appendingPathComponent("feedback")
            try FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
            for name in ["import-log.txt", "game-receipt.json", "runtime.log", "runtime-prev.log"] {
                let file = latest.appendingPathComponent(name)
                if FileManager.default.fileExists(atPath: file.path) { try FileManager.default.copyItem(at: file, to: dest.appendingPathComponent(name)) }
            }
        }
    }

    func exportDiagnostics() {
        guard !running else { return }
        exportingDiagnostics = true; collectedDirectory = nil
        diagnosticQueue = ["doctor"]
        run("collect")
        if !running { exportingDiagnostics = false }
    }

    private func saveDiagnostics() {
        if let diagnosticsDestination {
            do { try writeDiagnostics(to: diagnosticsDestination) } catch { self.error = error.localizedDescription }
            return
        }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
        panel.message = text("选择诊断报告的保存位置。仅包含安装日志、环境检查与游戏导入日志，不包含游戏、密钥或存档。", "Choose where to save diagnostics. Includes installer logs, environment checks and game import logs; no game files, keys or saves.")
        if panel.runModal() == .OK, let parent = panel.url {
            let output = parent.appendingPathComponent("SheikahVision-diagnostics-" + UUID().uuidString.prefix(8))
            do {
                try writeDiagnostics(to: output)
                status = text("诊断报告已保存：", "Diagnostics saved: ") + output.path
            } catch { self.error = error.localizedDescription }
        }
    }
}
