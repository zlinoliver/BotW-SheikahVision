import AppKit
import SwiftUI
import WebKit

// Explicit preview mode never starts the engine. These fixtures also drive the screenshot set.
@MainActor
enum PreviewScenes {
    static let names = ["welcome", "mac-disk", "account-teams", "review-account", "review-headset", "game-choose", "game-result", "headset-missing", "install-ready", "install-building", "install-app", "install-error", "finished", "home-5", "home-1", "home-0", "home-update", "home-newer-code", "home-game-missing", "headset-remembered", "transfer-wired", "transfer-wifi", "headset-connecting", "headset-unreachable", "install-ready-cached"]
    static func apply(_ name: String, to flow: WizardFlow) {
        flow.isPreview = true
        flow.busy = false; flow.installError = nil; flow.waitingForApp = false
        flow.probingDevices = false; flow.probedDevices = []; flow.totalProgress = 0; flow.verifiedDevice = ""; flow.reviewing = false; flow.toast = nil; flow.now = Date()
        let m = flow.engine
        m.simulate = { _, _, engine in engine.lastCommandOK = false; engine.running = false }
        m.running = false; m.error = nil; m.hasBuild = false; m.eta = nil; m.fraction = nil
        m.imported = ""; m.gameInfo = [:]; m.transferring = false; m.expiry = ""
        m.installerVersion = ""; m.builtVersion = nil; m.gameMissing = false; m.sourceChanged = false
        m.team = "TEAMA00001"
        m.checks = [["check": "disk", "status": "pass"]]
        m.succeeded = ["account", "import"]
        m.device = "PREVIEW-HEADSET"
        m.devices = [["name": "Vision Pro", "udid": m.device, "paired": true, "reachable": true]]
        flow.probedDevices = m.devices
        flow.verifiedDevice = m.device
        flow.mode = .setup; flow.step = .install
        switch name {
        case "welcome": flow.mode = .welcome
        case "mac-disk":
            flow.step = .mac; m.succeeded = []
            m.checks = [["check": "disk", "status": "fail", "detail": text("可用 18 GB，需要 25 GB。", "18 GB available; 25 GB needed.")]]
        case "account-teams":
            flow.step = .account; m.team = ""; m.succeeded = []
            m.teams = [Team(id: "TEAMA00001", name: "Sample Company", paid: true), Team(id: "DEMO000001", name: "Sample Personal Team", paid: false)]
        case "review-account":
            flow.step = .account; flow.reviewing = true
            m.teams = [Team(id: "TEAMA00001", name: "Sample Company", paid: true), Team(id: "DEMO000001", name: "Sample Personal Team", paid: false)]
        case "review-headset": flow.step = .headset; flow.reviewing = true
        case "game-choose": flow.step = .game; m.succeeded = ["account"]
        case "game-result":
            flow.step = .game
            m.imported = text("日版（简体中文汉化） · v208 · RPX 哈希校验通过", "Japan (Simplified Chinese patch) · v208 · RPX hash verified")
            m.gameInfo = ["region": "JPN", "title_version": 208, "text_patch": "zh-Hans", "file_count": 12345, "size_bytes": Int64(12_400_000_000)]
        case "headset-missing": flow.step = .headset; m.device = ""; m.devices = []; flow.probedDevices = []; flow.verifiedDevice = ""
        case "headset-remembered": flow.step = .headset; m.devices = []; flow.probedDevices = []; flow.verifiedDevice = ""
        case "headset-connecting", "headset-unreachable":
            flow.step = .headset; flow.verifiedDevice = ""
            m.devices[0]["availability"] = "available"; m.devices[0]["reachable"] = false
            flow.probedDevices = []
            flow.probingDevices = name == "headset-connecting"
            if name == "headset-unreachable" {
                var failed = m.devices[0]; failed["probe_error"] = "unavailable"; flow.probedDevices = [failed]
            }
        case "install-ready-cached": m.hasBuild = true; m.succeeded.insert("build")
        case "install-building": flow.busy = true; m.activeStage = "build"; m.fraction = 0.42; flow.totalProgress = 0.302; m.eta = 540
        case "install-app": flow.busy = true; m.activeStage = "install"; m.fraction = 0.5; flow.totalProgress = 0.68; m.eta = 60
        case "install-error": flow.installError = WizardFlow.disconnectedMessage
        case "finished":
            flow.mode = .finished; flow.gameKept = false
            m.teams = [Team(id: "TEAMA00001", name: "Sample Personal Team", paid: false)]
        case "transfer-wired", "transfer-wifi":
            m.devices[0]["transport"] = name == "transfer-wired" ? "wired" : "localNetwork"
            m.gameInfo = ["size_bytes": Int64(12_000_000_000)]
            m.activeStage = "push"; m.transferring = true; m.stepStarted = flow.now.addingTimeInterval(-180)
            flow.busy = true; flow.tick()
        case "home-newer-code":
            flow.mode = .home; m.hasBuild = true; m.installerVersion = "0.1.0"; m.builtVersion = "0.1.0"; m.sourceChanged = true
            m.gameInfo = ["size_bytes": Int64(12_812_880_822)]
            m.expiry = ISO8601DateFormatter().string(from: flow.now.addingTimeInterval(5 * 86400))
        case "home-game-missing":
            flow.mode = .home; m.hasBuild = true; m.gameMissing = true
            m.expiry = ISO8601DateFormatter().string(from: flow.now.addingTimeInterval(5 * 86400))
        case "home-update":
            flow.mode = .home; m.hasBuild = true; m.installerVersion = "0.2.0"; m.builtVersion = "0.1.0"
            m.gameInfo = ["size_bytes": Int64(12_812_880_822)]
            m.expiry = ISO8601DateFormatter().string(from: flow.now.addingTimeInterval(5 * 86400))
        case "home-5", "home-1", "home-0":
            flow.mode = .home; m.hasBuild = true
            m.gameInfo = ["size_bytes": Int64(12_812_880_822)]
            m.expiry = ISO8601DateFormatter().string(from: flow.now.addingTimeInterval(name == "home-5" ? 5 * 86400 : name == "home-1" ? 86400 : 0))
        default: break
        }
    }
    static func capture(_ flow: WizardFlow) async {
        guard let path = ProcessInfo.processInfo.environment["SV_UI_CAPTURE_DIR"] else { return }
        let directory = URL(fileURLWithPath: path)
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let requested = ProcessInfo.processInfo.environment["SV_UI_SCENES"]?.split(separator: ",").map(String.init) ?? names
            guard requested.allSatisfy({ names.contains($0) }) else { throw NSError(domain: "UISnapshot", code: 3) }
            for name in requested {
                apply(name, to: flow)
                try await Task.sleep(nanoseconds: 350_000_000)
                guard let window = NSApp.windows.first(where: { $0 is InstallerWindow }),
                      let view = window.contentView, window.frame.size == NSSize(width: 900, height: 600),
                      !window.styleMask.contains(.resizable), view.bounds.size == NSSize(width: 900, height: 600),
                      let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
                    throw NSError(domain: "UISnapshot", code: 1)
                }
                view.cacheDisplay(in: view.bounds, to: bitmap)
                guard let png = bitmap.representation(using: .png, properties: [:]) else { throw NSError(domain: "UISnapshot", code: 2) }
                try png.write(to: directory.appendingPathComponent(name + ".png"))
                print("Captured \(name): \(Int(view.bounds.width))×\(Int(view.bounds.height)); window \(Int(window.frame.width))×\(Int(window.frame.height)); resizable=\(window.styleMask.contains(.resizable))")
            }
            // SV_UI_GUIDE=<section>: also snapshot the guide window opened at that section (guide-<section>.png)
            if let section = ProcessInfo.processInfo.environment["SV_UI_GUIDE"] {
                InstallerWindows.openGuide(section)
                try await Task.sleep(nanoseconds: 1_500_000_000)
                guard let web = NSApp.windows.compactMap({ $0.contentView as? WKWebView }).first else { throw NSError(domain: "UISnapshot", code: 4) }
                let image = try await web.takeSnapshot(configuration: nil)
                guard let tiff = image.tiffRepresentation, let png = NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]) else { throw NSError(domain: "UISnapshot", code: 5) }
                try png.write(to: directory.appendingPathComponent("guide-\(section.isEmpty ? "top" : section).png"))
            }
            NSApp.terminate(nil)
        } catch {
            fputs("Snapshot failed: \(error)\n", stderr)
            exit(1)
        }
    }

}
