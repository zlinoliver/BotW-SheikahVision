import XCTest
@testable import SheikahInstaller

final class EngineTests: XCTestCase {
    @MainActor func testJSONEventsDriveWizardState() {
        let model = InstallerModel(work: FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString))
        model.receive(#"{"event":"teams","teams":[{"id":"TEAMA00001","name":"Test Team","paid":true}]}"#, name: "account")
        XCTAssertEqual(model.team, "TEAMA00001")
        model.receive(#"{"event":"progress","percent":40,"eta_seconds":120}"#, name: "build")
        XCTAssertEqual(model.fraction, 0.4)
        XCTAssertEqual(model.eta, 120)
        model.receive(#"{"event":"done","ok":false}"#, name: "build")
        XCTAssertFalse(model.succeeded.contains("build"))
        model.receive(#"{"event":"done","ok":true,"expires":"2026-11-11"}"#, name: "build")
        XCTAssertTrue(model.succeeded.contains("build"))
        XCTAssertEqual(model.expiry, "2026-11-11")
    }

    @MainActor func testImportResultSummary() {
        let model = InstallerModel(work: FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString))
        model.receive(#"{"event":"done","ok":true,"game":{"region":"JPN","title_version":208,"dlc_ignored":["DLC"]}}"#, name: "import")
        XCTAssertTrue(model.imported.contains("208"))
        XCTAssertTrue(model.succeeded.contains("import"))
    }

    @MainActor func testChinesePatchAndVersionsFromEvents() {
        let model = InstallerModel(work: FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString))
        model.receive(#"{"event":"done","ok":true,"game":{"region":"JPN","title_version":208,"text_patch":"zh-Hans"}}"#, name: "import")
        XCTAssertTrue(model.imported.contains("汉化") || model.imported.contains("Chinese patch"))
        model.receive(#"{"event":"done","ok":true,"installer_version":"0.2.0","built_version":"0.1.0","built":false}"#, name: "status")
        XCTAssertEqual(model.installerVersion, "0.2.0")
        XCTAssertEqual(model.builtVersion, "0.1.0")
    }

    @MainActor func testMalformedLinesDoNotAdvanceAndErrorsSurface() {
        let model = InstallerModel(work: FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString))
        model.receive("not-json", name: "install")
        model.receive(#"{"event":"error","message":"Open the app once"}"#, name: "install")
        XCTAssertFalse(model.installed)
        XCTAssertFalse(model.succeeded.contains("install"))
        XCTAssertTrue(model.error?.contains("Open the app once") == true)
    }
}
