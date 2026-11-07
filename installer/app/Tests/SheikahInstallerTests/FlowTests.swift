import XCTest
@testable import SheikahInstaller

@MainActor
final class FlowTests: XCTestCase {
    func make() throws -> WizardFlow {
        let work = FileManager.default.temporaryDirectory.appendingPathComponent("sv-flow-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: work) }
        let flow = WizardFlow(engine: InstallerModel(work: work, restoreAutomatically: false))
        flow.advanceDelay = 5_000_000; flow.pollDelay = 5_000_000; flow.documentDelay = 5_000_000
        return flow
    }
    func event(_ object: [String: Any], _ name: String, _ m: InstallerModel) {
        m.receive(String(data: try! JSONSerialization.data(withJSONObject: object), encoding: .utf8)!, name: name)
    }
    func fixture(_ flow: WizardFlow, calls: @escaping (String) -> Void = { _ in }, fail: String? = nil) {
        flow.engine.simulate = { [self] name, extra, m in
            calls(name)
            if name == "doctor" { event(["event":"check", "check":"disk", "status":"pass"], name, m) }
            if name == "account" {
                event(["event":"teams", "teams":[["id":"TEAMA00001", "name":"Test"]]], name, m)
            }
            if name == "devices" { event(["event":"devices", "devices":[["udid":"TEST", "name":"Test Vision", "paired":true, "reachable":true]]], name, m) }
            var done: [String: Any] = ["event":"done", "ok":name != fail]
            if name == "account" { done["warmed"] = !extra.contains("--list-only"); done["team"] = m.team }
            if name == "import" { done["game"] = ["region":"JPN", "title_version":208] }
            if name == "documents" { done["ready"] = true }
            if name == fail { event(["event":"error", "message":"fixture failure: " + name], name, m) }
            event(done, name, m); m.running = false
        }
    }
    func wait(_ condition: @escaping () -> Bool) async {
        for _ in 0..<300 {
            if condition() { return }
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
        XCTFail("Timed out waiting for flow")
    }
    func ready(_ f: WizardFlow) {
        f.mode = .setup; f.step = .install
        f.engine.checks = [["check":"disk", "status":"pass"]]
        f.engine.team = "TEAMA00001"; f.engine.device = "TEST"
        f.engine.succeeded = ["account", "import"]
        f.verifiedDevice = "TEST"
    }
    func testMacChecksOnEntryAndAutoAdvancesToAccount() async throws {
        let f = try make(); var commands: [String] = []; fixture(f, calls: { commands.append($0) })
        // the account step's team list starts a moment after the step is entered
        f.start(); await wait { f.step == .account && !f.busy && commands.count == 2 }
        XCTAssertEqual(commands, ["doctor", "account"]); XCTAssertEqual(f.state(.mac), .done)
    }
    func testFirstFailureBlocksAdvanceAndFutureNavigation() async throws {
        let f = try make()
        f.engine.simulate = { [self] name, _, m in
            event(["event":"check", "check":"disk", "status":"fail", "detail":"18 GB available"], name, m)
            event(["event":"done", "ok":false], name, m); m.running = false
        }
        f.start(); await wait { !f.busy && f.firstFailure != nil }
        f.select(.install)
        XCTAssertEqual(f.step, .mac); XCTAssertEqual(f.state(.mac), .attention)
        XCTAssertEqual(f.firstFailure?["check"] as? String, "disk")
    }
    func testMultipleTeamsNeverAutoPickAndNoIdentityShowsGuidance() async throws {
        let f = try make(); f.mode = .setup; f.step = .account
        f.engine.receive(#"{"event":"teams","teams":[{"id":"TEAMA00001"},{"id":"DEMO000001"}]}"#, name:"account")
        XCTAssertTrue(f.engine.team.isEmpty)
        f.engine.team = "TEAMA00001"
        f.engine.simulate = { [self] name, _, m in
            event(["event":"done", "ok":true, "team":m.team, "warmed":false], name, m); m.running = false
        }
        f.warmSigning(); await wait { f.engine.error != nil }
        XCTAssertEqual(f.step, .account); XCTAssertFalse(f.engine.canContinue(2))
        XCTAssertTrue(f.engine.error!.contains("Apple Development"))
    }
    func testFullSimulatedFlowAndOneButtonPipeline() async throws {
        let f = try make(); var calls: [String] = []; fixture(f, calls: { calls.append($0) })
        f.start(); await wait { f.step == .account && !f.busy && calls.contains("account") }
        f.warmSigning(); await wait { f.step == .game }
        f.acceptGameURLs([URL(fileURLWithPath:"/synthetic/game")])
        await wait { f.step == .install }
        f.install(); await wait { f.mode == .finished }
        XCTAssertEqual(Array(calls.suffix(5)), ["build", "install", "launch", "documents", "push-game"])
        XCTAssertEqual(f.totalProgress, 1)
        f.finish(); XCTAssertEqual(f.mode, .home)
    }
    func testUpdateSkipsTheTransferWhenTheHeadsetHasTheGame() async throws {
        let f = try make(); ready(f); var calls: [String] = []; fixture(f, calls: { calls.append($0) })
        let base = f.engine.simulate!
        f.engine.simulate = { [self] name, extra, m in
            if name == "documents" { calls.append(name); event(["event":"done", "ok":true, "ready":true, "has_game":true], name, m); m.running = false }
            else { base(name, extra, m) }
        }
        f.updating = true
        f.install(); await wait { f.mode == .finished }
        XCTAssertEqual(calls, ["build", "install", "launch", "documents"])
        XCTAssertTrue(f.gameKept)
        f.updating = false; calls = []; f.mode = .setup; f.step = .install; f.engine.succeeded = ["account", "import"]
        f.install(); await wait { f.mode == .finished }
        XCTAssertEqual(calls.last, "push-game")  // a first install always sends the game
        XCTAssertFalse(f.gameKept)
    }
    func testFinishedAccountStepCanBeReopenedToChangeTheTeam() async throws {
        let f = try make(); ready(f); var calls: [String] = []; fixture(f, calls: { calls.append($0) })
        f.engine.succeeded.insert("build"); f.engine.hasBuild = true
        f.step = .headset; f.select(.account); XCTAssertTrue(f.reviewing)
        f.redo(.account)
        await wait { f.step == .account && !f.busy && calls.contains("account") }
        XCTAssertFalse(f.reviewing)
        XCTAssertFalse(f.engine.succeeded.contains("account"))
        f.engine.team = "DEMO000001"                      // another team: the build must be redone
        XCTAssertFalse(f.engine.succeeded.contains("build"))
        f.warmSigning(); await wait { f.step != .account }
        XCTAssertTrue(f.engine.succeeded.contains("account"))
    }
    func testPackageAlreadyOnTheHeadsetIsNotSentAgain() async throws {
        let f = try make(); ready(f); var calls: [String] = []; fixture(f, calls: { calls.append($0) })
        let base = f.engine.simulate!
        f.engine.simulate = { [self] name, extra, m in
            if name == "documents" { calls.append(name); event(["event":"done", "ok":true, "ready":true, "has_game":false, "game_pending":true], name, m); m.running = false }
            else { base(name, extra, m) }
        }
        f.install(); await wait { f.mode == .finished }
        XCTAssertFalse(calls.contains("push-game"))
        XCTAssertTrue(f.gameWaiting)
    }
    func testFailedStageRetriesWithoutRepeatingBuild() async throws {
        let f = try make(); ready(f); var calls: [String] = []
        fixture(f, calls: { calls.append($0) }, fail: "install")
        f.install(); await wait { f.installError != nil && !f.busy }
        XCTAssertEqual(calls, ["build", "install"])
        XCTAssertTrue(f.installError!.contains("fixture failure: install"))
        fixture(f, calls: { calls.append($0) })
        f.install(); await wait { f.mode == .finished }
        XCTAssertEqual(calls.filter { $0 == "build" }.count, 1)
        XCTAssertEqual(calls.filter { $0 == "install" }.count, 2)
    }
    func testDocumentsWaitAndLaunchFailureDoNotRequireButton() async throws {
        let f = try make(); ready(f); var probes = 0; var calls: [String] = []
        fixture(f, calls: { calls.append($0) }, fail: "launch")
        let base = f.engine.simulate!
        f.engine.simulate = { [self] name, extra, m in
            if name == "documents" {
                probes += 1
                event(["event":"done", "ok":true, "ready":probes >= 3, "reason":"not-opened"], name, m); m.running = false
            } else { base(name, extra, m) }
        }
        f.install(); await wait { f.mode == .finished }
        XCTAssertEqual(probes, 3); XCTAssertTrue(calls.contains("push-game"))
    }
    func testCancelDuringDocumentsDoesNotTransferAndRetryWorks() async throws {
        let f = try make(); ready(f); var calls: [String] = []; fixture(f, calls: { calls.append($0) })
        let base = f.engine.simulate!
        f.engine.simulate = { [self] name, extra, m in
            if name == "documents" { event(["event":"done", "ok":true, "ready":false, "reason":"not-opened"], name, m); m.running = false }
            else { base(name, extra, m) }
        }
        f.install(); await wait { f.waitingForApp }; f.cancel()
        try? await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertFalse(calls.contains("push-game")); XCTAssertFalse(f.busy)
        fixture(f, calls: { calls.append($0) }); f.install(); await wait { f.mode == .finished }
        XCTAssertEqual(calls.filter { $0 == "build" }.count, 1)
    }
    func testWeightedProgressAndTransferEstimateAreCapped() {
        XCTAssertEqual(InstallProgress.overall(stage:"build", fraction:0.5, elapsed:0, transferSeconds:480), 0.35, accuracy:0.001)
        XCTAssertEqual(InstallProgress.overall(stage:"sign", fraction:0, elapsed:0, transferSeconds:480), 0.65)
        XCTAssertEqual(InstallProgress.overall(stage:"install", fraction:0, elapsed:0, transferSeconds:480), 0.68)
        let estimated = InstallProgress.overall(stage:"push", fraction:nil, elapsed:240, transferSeconds:480)
        XCTAssertEqual(estimated, 0.875)
        XCTAssertLessThan(InstallProgress.overall(stage:"push", fraction:nil, elapsed:99999, transferSeconds:480), 1)
        XCTAssertLessThanOrEqual(InstallProgress.overall(stage:"push", fraction:1, elapsed:99999, transferSeconds:480), 0.99)
    }
    func testHomeRestoresOnlyAfterEngineConfirmsCompletedInstallation() async throws {
        let initial = try make()
        let state: [String: Any] = ["completed":true, "started":true, "team":"TEAMA00001", "device":"TEST"]
        try JSONSerialization.data(withJSONObject:state).write(to:initial.engine.work.appendingPathComponent("wizard-state.json"))
        let m = InstallerModel(work:initial.engine.work, restoreAutomatically:false)
        let f = WizardFlow(engine:m)
        f.engine.simulate = { [self] name, _, m in
            event(["event":"done", "ok":true, "team":"TEAMA00001", "built":true, "installed_devices":["TEST":1]], name, m); m.running = false
        }
        f.boot(); await wait { f.mode == .home }
        XCTAssertTrue(f.engine.hasBuild)
    }
    func testAppInstalledButTransferNotFinishedDoesNotRestoreHome() async throws {
        let f = try make(); f.engine.team = "TEAMA00001"; f.engine.device = "TEST"
        f.engine.simulate = { [self] name, _, m in
            event(["event":"done", "ok":true, "team":"TEAMA00001", "built":true, "installed_devices":["TEST":1]], name, m); m.running = false
        }
        f.boot(); await wait { !f.busy && f.engine.installed }
        XCTAssertEqual(f.mode, .welcome)
    }
    func testReviewDoneStepDoesNotAutoAdvanceAndCannotSwitchWhileBusy() async throws {
        let f = try make(); ready(f)
        f.select(.account); XCTAssertTrue(f.reviewing)
        f.busy = true; f.select(.game); XCTAssertEqual(f.step, .account)
        f.busy = false
        try? await Task.sleep(nanoseconds: 20_000_000)
        XCTAssertEqual(f.step, .account)
    }
    func testRenewOnlyRunsRenewAndKeepsHome() async throws {
        let f = try make(); f.mode = .home; f.engine.hasBuild = true
        var calls: [String] = []; fixture(f, calls: { calls.append($0) })
        f.renew(); await wait { calls == ["renew"] && !f.busy }
        XCTAssertEqual(f.mode, .home); XCTAssertEqual(calls, ["renew"])
    }
    func testPreviewFixturesNeverStartAnEngineAndBothExpiryWarnings() throws {
        let f = try make(); var calls = 0; fixture(f, calls: { _ in calls += 1 })
        for name in PreviewScenes.names { PreviewScenes.apply(name, to:f); f.boot() }
        XCTAssertEqual(calls, 0)
        PreviewScenes.apply("home-1", to:f); XCTAssertEqual(f.daysRemaining, 1)
        XCTAssertFalse(f.busy)
        PreviewScenes.apply("install-error", to:f)
        XCTAssertFalse(f.busy); XCTAssertNotNil(f.installError)
        PreviewScenes.apply("home-5", to:f); XCTAssertEqual(f.daysRemaining, 5)
    }
    func testEarlySigningWarmupIsNotFinalSigningProgress() throws {
        let f = try make()
        f.engine.receive(#"{"event":"command","step":"sign"}"#, name: "build")
        XCTAssertEqual(f.engine.activeStage, "provision")
        f.engine.receive(#"{"event":"command","step":"build"}"#, name: "build")
        f.engine.receive(#"{"event":"command","step":"sign"}"#, name: "build")
        XCTAssertEqual(f.engine.activeStage, "sign")
    }
    func testSessionPersistsTeamAndWarmup() throws {
        let f = try make(); ready(f)
        f.persist()
        let restoredEngine = InstallerModel(work:f.engine.work, restoreAutomatically:false)
        let restored = WizardFlow(engine:restoredEngine)
        XCTAssertEqual(restored.engine.team, "TEAMA00001")
        XCTAssertEqual(restored.engine.device, "TEST")
        XCTAssertTrue(restored.engine.canContinue(2))
    }
    func testMultipleHeadsetsRequireAnExplicitChoice() async throws {
        let f = try make(); f.mode = .setup
        f.engine.simulate = { [self] name, _, m in
            event(["event":"devices", "devices":[["udid":"ONE", "paired":true, "reachable":true], ["udid":"TWO", "paired":true, "reachable":true]]], name, m)
            event(["event":"done", "ok":true], name, m); m.running = false
        }
        f.enter(.headset)
        await wait { f.engine.devices.count == 2 }
        XCTAssertEqual(f.step, .headset); XCTAssertTrue(f.engine.device.isEmpty)
        f.chooseDevice("TWO"); await wait { f.step == .install }
        XCTAssertEqual(f.engine.device, "TWO")
    }
    func testChangingGameAndDeviceInvalidateInstallationStages() throws {
        let f = try make(); ready(f)
        f.engine.succeeded.formUnion(["build", "install", "push-game"])
        f.engine.device = "OTHER"
        XCTAssertFalse(f.engine.succeeded.contains("install")); XCTAssertFalse(f.engine.succeeded.contains("push-game"))
        f.engine.succeeded.formUnion(["install", "push-game"])
        f.engine.receive(#"{"event":"done","ok":true}"#, name:"build")
        XCTAssertFalse(f.engine.succeeded.contains("install")); XCTAssertFalse(f.engine.succeeded.contains("push-game"))
    }

    func testRememberedDeviceRequiresFreshOnlineConfirmation() async throws {
        let initial = try make(); ready(initial); initial.persist()
        let m = InstallerModel(work:initial.engine.work, restoreAutomatically:false)
        let f = WizardFlow(engine:m); f.pollDelay = 5_000_000
        m.checks = [["check":"disk", "status":"pass"]]; m.succeeded.insert("import")
        var polls = 0
        m.simulate = { [self] name, _, engine in
            polls += 1
            event(["event":"devices", "devices":[]], name, engine)
            event(["event":"done", "ok":true], name, engine); engine.running = false
        }
        XCTAssertEqual(m.device, "TEST"); XCTAssertFalse(f.deviceVerified)
        f.start(); await wait { polls > 0 }
        XCTAssertEqual(f.step, .headset); XCTAssertTrue(f.rememberedDeviceUnavailable)
        XCTAssertFalse(f.deviceVerified); XCTAssertEqual(f.state(.headset), .attention)
        f.cancel()
    }
    func testRememberedOnlineDeviceAdvancesAfterPolling() async throws {
        let f = try make(); ready(f); f.verifiedDevice = ""
        fixture(f); f.start(); await wait { f.step == .install }
        XCTAssertEqual(f.verifiedDevice, "TEST")
    }
    func testRememberedChoiceAmongMultipleDevicesStillNeedsConfirmation() async throws {
        let f = try make(); f.mode = .setup; f.engine.device = "ONE"
        f.engine.simulate = { [self] name, _, m in
            event(["event":"devices", "devices":[["udid":"ONE", "paired":true, "reachable":true], ["udid":"TWO", "paired":true, "reachable":true]]], name, m)
            event(["event":"done", "ok":true], name, m); m.running = false
        }
        f.enter(.headset); await wait { f.onlineDevices.count == 2 }
        XCTAssertEqual(f.engine.device, "ONE"); XCTAssertFalse(f.deviceVerified); XCTAssertEqual(f.step, .headset)
        f.chooseDevice("ONE"); await wait { f.step == .install }
        XCTAssertTrue(f.deviceVerified)
    }
    func testUnpairedOrOfflineSelectionAndAdvancedBuildAreRejected() throws {
        let f = try make(); ready(f); f.verifiedDevice = ""
        f.engine.devices = [["udid":"TEST", "paired":true, "reachable":false], ["udid":"OTHER", "paired":false, "reachable":true]]
        f.chooseDevice("TEST"); XCTAssertFalse(f.deviceVerified)
        f.chooseDevice("OTHER"); XCTAssertFalse(f.deviceVerified)
        var calls = 0; fixture(f, calls: { _ in calls += 1 })
        f.runAdvanced("build"); XCTAssertEqual(calls, 0); XCTAssertNotNil(f.engine.error)
        f.install(); XCTAssertFalse(f.deviceVerified)
        f.cancel()
    }
    func testUnreachableDocumentsPauseAndRetryStartsAtLaunch() async throws {
        let f = try make(); ready(f); f.unreachableTimeout = 0.02
        var calls: [String] = []; fixture(f, calls: { calls.append($0) })
        let base = f.engine.simulate!
        f.engine.simulate = { [self] name, extra, m in
            if name == "documents" {
                calls.append(name)
                event(["event":"done", "ok":true, "ready":false, "reason":"unreachable"], name, m); m.running = false
            } else { base(name, extra, m) }
        }
        f.install(); await wait { f.installError != nil && !f.busy }
        XCTAssertEqual(f.installError, WizardFlow.disconnectedMessage)
        XCTAssertFalse(f.needsManualLaunch); XCTAssertFalse(calls.contains("push-game"))
        fixture(f, calls: { calls.append($0) }); f.install(); await wait { f.mode == .finished }
        XCTAssertEqual(calls.filter { $0 == "build" }.count, 1)
        XCTAssertEqual(calls.filter { $0 == "install" }.count, 1)
        XCTAssertEqual(calls.filter { $0 == "launch" }.count, 2)
    }
    func testNotOpenedDocumentsShowManualLaunchWithoutDisconnectPause() async throws {
        let f = try make(); ready(f); f.manualLaunchDelay = 0.02; f.unreachableTimeout = 0.005
        fixture(f); let base = f.engine.simulate!
        f.engine.simulate = { [self] name, extra, m in
            if name == "documents" {
                event(["event":"done", "ok":true, "ready":false, "reason":"not-opened"], name, m); m.running = false
            } else { base(name, extra, m) }
        }
        f.install(); await wait { f.needsManualLaunch }
        XCTAssertNil(f.installError); XCTAssertTrue(f.busy); f.cancel()
    }
    func testUnreachableLaunchPausesBeforeDocuments() async throws {
        let f = try make(); ready(f); var calls: [String] = []
        fixture(f, calls: { calls.append($0) }, fail:"launch"); let base = f.engine.simulate!
        f.engine.simulate = { [self] name, extra, m in
            base(name, extra, m)
            if name == "launch" { event(["event":"error", "code":"E_DEVICE_UNREACHABLE", "message":"disconnected"], name, m) }
        }
        f.install(); await wait { f.installError != nil && !f.busy }
        XCTAssertEqual(f.installError, WizardFlow.disconnectedMessage); XCTAssertFalse(calls.contains("documents"))
    }
    func testTransferEstimatesForWiredWifiAndUnknown() {
        let wired = TransferEstimate(bytes:12_000_000_000, connection:.wired)
        let wifi = TransferEstimate(bytes:12_000_000_000, connection:.wifi)
        let unknown = TransferEstimate(bytes:nil, connection:.unknown)
        XCTAssertEqual(wired.seconds, 12_000_000_000 / 26_000_000, accuracy:0.001)
        XCTAssertEqual(wifi.seconds, 12_000_000_000 / 7_000_000, accuracy:0.001)
        XCTAssertEqual(unknown.seconds, wifi.seconds)
        XCTAssertEqual(wired.totalMinutes(build:17), 26...26)
        XCTAssertEqual(wifi.totalMinutes(build:17), 47...47)
        XCTAssertEqual(unknown.totalMinutes(build:17), 26...47)
        XCTAssertEqual(TransferConnection(transport:"wired"), .wired)
        XCTAssertEqual(TransferConnection(transport:"localNetwork"), .wifi)
        XCTAssertEqual(TransferConnection(transport:"new-transport"), .unknown)
    }
    func testTransferSubtitleOverrunAndProgressDoNotRegress() throws {
        let f = try make(); ready(f)
        f.engine.activeStage = "push"; f.engine.transferring = true
        f.engine.gameInfo = ["size_bytes":Int64(12_000_000_000)]
        f.engine.devices = [["udid":"TEST", "transport":"wired"]]
        f.engine.stepStarted = Date().addingTimeInterval(-120); f.busy = true; f.tick()
        XCTAssertTrue(f.transferSubtitle.contains(minuteCount(8)))
        let before = f.totalProgress
        f.engine.stepStarted = Date().addingTimeInterval(-2000); f.tick()
        XCTAssertTrue(f.transferSubtitle.contains(text("仍在传输", "still transferring")))
        XCTAssertGreaterThanOrEqual(f.totalProgress, before); XCTAssertLessThan(f.totalProgress, 1)
        let capped = f.totalProgress; f.engine.devices[0]["transport"] = "localNetwork"; f.tick()
        XCTAssertGreaterThanOrEqual(f.totalProgress, capped)
    }
    func testCancellingAdvancedDoctorDoesNotPauseInstallation() async throws {
        let f = try make()
        f.engine.simulate = { _, _, _ in }
        f.runAdvanced("doctor"); await wait { f.engine.running }
        f.cancel(); await wait { !f.busy }
        f.enter(.install)
        XCTAssertNil(f.installError); XCTAssertEqual(f.engine.status, text("已取消。", "Cancelled."))
    }
    func testWizardOwnsStartupStatusExactlyOnce() async throws {
        let f = try make()
        let source = f.engine.work.appendingPathComponent("source/installer/engine")
        try FileManager.default.createDirectory(at:source, withIntermediateDirectories:true)
        try Data().write(to:source.appendingPathComponent("sv.py"))
        let saved = ["source":f.engine.work.appendingPathComponent("source").path]
        try JSONSerialization.data(withJSONObject:saved).write(to:f.engine.work.appendingPathComponent("wizard-state.json"))
        let m = InstallerModel(work:f.engine.work)
        let flow = WizardFlow(engine:m); var calls: [String] = []
        fixture(flow, calls: { calls.append($0) })
        flow.boot(); flow.boot(); await wait { calls.contains("status") && !flow.busy }
        try? await Task.sleep(nanoseconds:20_000_000)
        XCTAssertEqual(calls, ["status"])
    }
    func testSingularPluralAndTodayCopy() {
        XCTAssertEqual(signingTitle(0), text("签名今天到期", "Signing expires today"))
        XCTAssertEqual(signingTitle(1), text("签名还剩 1 天", "Signing expires in 1 day"))
        XCTAssertEqual(signingTitle(2), text("签名还剩 2 天", "Signing expires in 2 days"))
        XCTAssertEqual(minuteCount(1), text("1 分钟", "1 minute"))
        XCTAssertEqual(minuteCount(2), text("2 分钟", "2 minutes"))
        XCTAssertEqual(fileCount(1), text("1 个文件", "1 file"))
        XCTAssertEqual(fileCount(2), text("2 个文件", "2 files"))
    }

    func testAvailableDeviceCompletesOnlyAfterProbe() async throws {
        let f = try make(); f.mode = .setup; var requests: [[String]] = []
        f.engine.simulate = { [self] name, extra, m in
            requests.append(extra)
            event(["event":"devices", "devices":[["udid":"IDLE", "name":"Idle Vision", "paired":true, "availability":"available", "reachable":extra.contains("--probe")]]], name, m)
            event(["event":"done", "ok":true], name, m); m.running = false
        }
        f.enter(.headset); await wait { f.step == .install }
        XCTAssertEqual(requests, [[], ["--probe"]]); XCTAssertEqual(f.verifiedDevice, "IDLE")
    }
    func testFailedProbeShowsHintAndIsThrottledForTenSeconds() async throws {
        let f = try make(); f.mode = .setup
        var time = Date(); f.clock = { time }; var probes = 0; var lists = 0
        f.engine.simulate = { [self] name, extra, m in
            var device: [String:Any] = ["udid":"IDLE", "name":"Idle Vision", "paired":true, "availability":"available", "reachable":false]
            if extra.contains("--probe") { probes += 1; device["probe_error"] = "unavailable" }
            else { lists += 1 }
            event(["event":"devices", "devices":[device]], name, m)
            event(["event":"done", "ok":true], name, m); m.running = false
        }
        f.enter(.headset); await wait { lists >= 3 }
        XCTAssertEqual(probes, 1); XCTAssertFalse(f.deviceVerified)
        XCTAssertTrue(f.connectionStatus?.contains(text("暂时连不上", "can’t be reached yet")) == true)
        time.addTimeInterval(9); let before = lists; await wait { lists > before }
        XCTAssertEqual(probes, 1)
        time.addTimeInterval(2); await wait { probes == 2 }
        f.cancel()
    }
    func testCandidateChangeTriggersProbeBeforeTenSeconds() async throws {
        let f = try make(); f.mode = .setup
        let time = Date(); f.clock = { time }; var id = "FIRST"; var probes = 0
        f.engine.simulate = { [self] name, extra, m in
            if extra.contains("--probe") { probes += 1 }
            event(["event":"devices", "devices":[["udid":id, "paired":true, "availability":"available", "reachable":false, "probe_error":"unavailable"]]], name, m)
            event(["event":"done", "ok":true], name, m); m.running = false
        }
        f.enter(.headset); await wait { probes == 1 }; id = "SECOND"
        await wait { probes == 2 }; f.cancel()
    }
    func testAdvancedCollectRunsBeforeSetup() async throws {
        let f = try make(); var calls: [String] = []; var flags: [String] = []
        f.engine.simulate = { [self] name, extra, m in
            calls.append(name); flags = extra
            event(["event":"done", "ok":true], name, m); m.running = false
        }
        f.runAdvanced("collect"); await wait { !calls.isEmpty && !f.busy }
        XCTAssertEqual(calls, ["collect"]); XCTAssertEqual(flags, ["--local-only"]); XCTAssertNil(f.engine.error)
    }
    func testCachedBuildUsesTwoMinutePreparationEstimate() throws {
        let f = try make(); ready(f)
        f.engine.gameInfo = ["size_bytes":Int64(12_000_000_000)]; f.engine.estimatedBuildMinutes = 17
        XCTAssertEqual(f.installDuration, text("约 26–47 分钟", "About 26–47 minutes"))
        f.engine.hasBuild = true; f.engine.succeeded.insert("build")
        XCTAssertEqual(f.installDuration, text("约 11–32 分钟", "About 11–32 minutes"))
        f.engine.hasBuild = false
        XCTAssertEqual(f.installDuration, text("约 26–47 分钟", "About 26–47 minutes"))
    }
    func testLanguageResolutionAndPersistence() {
        XCTAssertEqual(InstallerLanguageChoice.resolve(saved:"zh", environment:nil, preferred:"en"), "zh")
        XCTAssertEqual(InstallerLanguageChoice.resolve(saved:"en", environment:nil, preferred:"zh-Hans"), "en")
        XCTAssertEqual(InstallerLanguageChoice.resolve(saved:"system", environment:nil, preferred:"zh-Hans"), "zh")
        XCTAssertEqual(InstallerLanguageChoice.resolve(saved:"en", environment:"zh", preferred:"en"), "zh")
        let name = "sv-language-" + UUID().uuidString
        let defaults = UserDefaults(suiteName:name)!
        defer { defaults.removePersistentDomain(forName:name) }
        let language = InstallerLanguage(defaults:defaults); language.select(.zh)
        XCTAssertEqual(InstallerLanguage(defaults:defaults).choice, .zh)
        language.select(.en); XCTAssertEqual(defaults.string(forKey:"installerLanguage"), "en")
    }
    func testLanguageSwitchDoesNotRestartRunningOperation() throws {
        let f = try make(); ready(f); f.busy = true
        f.engine.gameInfo = ["region":"JPN", "title_version":208]
        let previous = InstallerLanguage.shared.choice
        defer { InstallerLanguage.shared.select(previous) }
        var calls = 0; fixture(f, calls: { _ in calls += 1 })
        f.changeLanguage(.zh); XCTAssertTrue(f.engine.imported.contains("日版")); XCTAssertTrue(f.busy)
        f.changeLanguage(.en); XCTAssertTrue(f.engine.imported.contains("Japan")); XCTAssertTrue(f.busy)
        XCTAssertEqual(calls, 0); XCTAssertEqual(f.step, .install)
    }

}
