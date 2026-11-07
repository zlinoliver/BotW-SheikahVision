import XCTest
@testable import SheikahInstaller

final class ReviewTests: XCTestCase {
    func temp() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: url) }
        return url
    }

    @MainActor func testTeamsWarmupAndInvalidation() throws {
        let m = InstallerModel(work: try temp(), restoreAutomatically: false)
        m.receive(#"{"event":"teams","teams":[{"id":"TEAMA00001"},{"id":"ABCDE12345"}]}"#, name: "account")
        XCTAssertEqual(m.team, "")
        m.team = "TEAMA00001"
        m.receive(#"{"event":"done","ok":true,"warmed":false,"team":"TEAMA00001"}"#, name: "account")
        XCTAssertFalse(m.canContinue(2))
        m.receive(#"{"event":"done","ok":true,"warmed":true,"team":"TEAMA00001"}"#, name: "account")
        XCTAssertTrue(m.canContinue(2))
        m.succeeded.insert("build"); m.hasBuild = true
        m.team = "ABCDE12345"
        XCTAssertFalse(m.canContinue(2)); XCTAssertFalse(m.hasBuild)
        XCTAssertFalse(m.succeeded.contains("build"))
    }

    @MainActor func testPagePrerequisitesAndHeadsetWarning() throws {
        let m = InstallerModel(work: try temp(), restoreAutomatically: false)
        m.checks = [["check":"disk","status":"pass"],["check":"headset","status":"fail"]]
        XCTAssertTrue(m.canContinue(1))
        XCTAssertFalse(m.canContinue(3)); XCTAssertFalse(m.canContinue(4)); XCTAssertFalse(m.canContinue(5))
        m.succeeded.insert("import"); m.device = "HEADSET"; m.succeeded.insert("build")
        XCTAssertTrue(m.canContinue(3)); XCTAssertTrue(m.canContinue(4)); XCTAssertTrue(m.canContinue(5))
    }

    @MainActor func testRestoredStatusAndTransferReset() throws {
        let m = InstallerModel(work: try temp(), restoreAutomatically: false)
        m.team = "TEAMA00001"; m.device = "HEADSET"
        m.receive(#"{"event":"done","ok":true,"built":true,"team":"TEAMA00001","installed_devices":{"HEADSET":123},"game":{"region":"JPN","title_version":208}}"#, name: "status")
        XCTAssertTrue(m.succeeded.contains("import")); XCTAssertTrue(m.hasBuild); XCTAssertTrue(m.installed)
        m.device = "OTHER"; XCTAssertFalse(m.installed)
        m.receive(#"{"event":"progress","percent":99,"eta_seconds":4}"#, name: "push-game")
        m.receive(#"{"event":"step","step":"push","status":"running"}"#, name: "push-game")
        XCTAssertNil(m.fraction); XCTAssertNil(m.eta); XCTAssertTrue(m.transferring)
        m.team = "ABCDE12345"
        m.receive(#"{"event":"done","ok":true,"built":true,"team":"TEAMA00001","installed_devices":{"HEADSET":123}}"#, name: "status")
        XCTAssertFalse(m.hasBuild); XCTAssertFalse(m.installed)
    }

    @MainActor func testLogAppendRotationAndDiagnosticContents() throws {
        let work = try temp()
        let first = InstallerModel(work: work, restoreAutomatically: false)
        first.receive(#"{"event":"notice","message":"first session"}"#, name: "doctor")
        let second = InstallerModel(work: work, restoreAutomatically: false)
        second.receive(#"{"event":"notice","message":"second session"}"#, name: "doctor")
        let wizard = work.appendingPathComponent("diagnostics/wizard.log")
        let log = try String(contentsOf: wizard)
        XCTAssertTrue(log.contains("first session")); XCTAssertTrue(log.contains("second session"))
        try Data(repeating: 65, count: 3 << 20).write(to: wizard)
        second.receive("after rotation", name: "doctor")
        XCTAssertTrue(FileManager.default.fileExists(atPath: work.appendingPathComponent("diagnostics/wizard.prev.log").path))
        try Data(repeating: 66, count: 300 << 10).write(to: work.appendingPathComponent("build.log"))
        let feedback = work.appendingPathComponent("feedback/20261104")
        try FileManager.default.createDirectory(at: feedback, withIntermediateDirectories: true)
        try Data("import failed".utf8).write(to: feedback.appendingPathComponent("import-log.txt"))
        let output = work.appendingPathComponent("export")
        try second.writeDiagnostics(to: output)
        XCTAssertEqual(try Data(contentsOf: output.appendingPathComponent("build-tail.log")).count, 256 << 10)
        XCTAssertTrue(FileManager.default.fileExists(atPath: output.appendingPathComponent("feedback/import-log.txt").path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: output.appendingPathComponent("doctor.jsonl").path))
    }

    @MainActor func fake(_ script: String, work: URL, destination: URL? = nil) throws -> InstallerModel {
        let source = work.appendingPathComponent("source/installer/engine")
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: true)
        try script.write(to: source.appendingPathComponent("sv.py"), atomically: true, encoding: .utf8)
        let m = InstallerModel(work: work, restoreAutomatically: false, diagnosticsDestination: destination)
        m.source = work.appendingPathComponent("source").path
        return m
    }

    @MainActor func testActualCancellation() async throws {
        let m = try fake("import time\nprint('ready',flush=True)\ntime.sleep(60)\n", work: temp())
        m.run("build")
        for _ in 0..<100 where !m.log.contains("ready") { try await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertTrue(m.running); m.cancel()
        for _ in 0..<100 where m.running { try await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertFalse(m.running)
    }

    @MainActor func testFailedCollectStillExportsDoctor() async throws {
        let work = try temp(), dest = work.appendingPathComponent("export")
        let m = try fake("import sys,json\nc=sys.argv[1]\nprint(json.dumps({'event':'done','ok':c=='doctor'}),flush=True)\nsys.exit(0 if c=='doctor' else 1)\n", work: work, destination: dest)
        m.exportDiagnostics()
        for _ in 0..<200 where !FileManager.default.fileExists(atPath: dest.path) { try await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertFalse(m.running)
        XCTAssertTrue(FileManager.default.fileExists(atPath: dest.appendingPathComponent("doctor.jsonl").path))
    }
    @MainActor func testCancellationStopsDiagnosticQueue() async throws {
        let work = try temp(), dest = work.appendingPathComponent("export")
        let m = try fake("import time\nprint('collect-ready',flush=True)\ntime.sleep(60)\n", work: work, destination: dest)
        m.exportDiagnostics()
        for _ in 0..<100 where !m.log.contains("collect-ready") { try await Task.sleep(nanoseconds: 20_000_000) }
        m.cancel()
        for _ in 0..<100 where m.running { try await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertFalse(m.running)
        XCTAssertFalse(FileManager.default.fileExists(atPath: dest.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: work.appendingPathComponent("diagnostics/wizard.log").path))
    }

}
