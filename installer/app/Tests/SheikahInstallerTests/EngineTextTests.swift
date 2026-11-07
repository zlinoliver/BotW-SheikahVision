import XCTest
@testable import SheikahInstaller

final class EngineTextTests: XCTestCase {
    func testEngineMessagesHaveChinese() {
        XCTAssertEqual(EngineText.chinese("attempt 1/5"), "正在连接 Vision Pro…")
        XCTAssertEqual(EngineText.chinese("attempt 3/5"), "第 3/5 次尝试")
        XCTAssertEqual(EngineText.chinese("failed (disconnected); retrying in 10 s"), "失败（disconnected），10 秒后重试")
        XCTAssertEqual(EngineText.chinese("the Vision Pro dropped the connection; trying again"), "Vision Pro 连接断开，正在重试")
        XCTAssertEqual(EngineText.chinese("need 13.0 GB free in /w, have 2.5 GB"), "需要 13.0 GB 可用空间，现在只有 2.5 GB")
        XCTAssertEqual(EngineText.chinese("title_version is 0, not 208 (v208 = update 1.5.0, the last Wii U update): select the v208 update folder together with the base game"),
                       "游戏版本是 0，不是 208（v208 即 1.5.0 更新）：请同时选择 v208 更新文件夹和本体")
    }
    func testUnknownMessagesStayAsTheyAre() {
        XCTAssertNil(EngineText.chinese("something new from the engine"))
    }
}
