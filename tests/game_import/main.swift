import Foundation

let fm = FileManager.default
let fixtures = URL(fileURLWithPath: CommandLine.arguments[1])
let runs = URL(fileURLWithPath: CommandLine.arguments[2])
let expected = try String(contentsOf: fixtures.appendingPathComponent("rpx.sha256"), encoding: .utf8)
var passed = 0
struct Failure: Error { var text: String }
func require(_ value: @autoclosure () throws -> Bool, _ text: String) throws {
    if try !value() { throw Failure(text: text) }
}
enum Rejection {
    case importError, message(String), contains(String), cocoa(CocoaError.Code), cancelled
}
func rejected(_ expected: Rejection = .importError, _ body: () throws -> Void) throws {
    do { try body() } catch {
        if error is Failure { throw error }
        let matches: Bool
        switch expected {
        case .importError: matches = error is BotWZipError
        case .message(let text): matches = (error as? BotWZipError)?.message == text
        case .contains(let text): matches = (error as? BotWZipError)?.message.contains(text) == true
        case .cocoa(let code): matches = (error as? CocoaError)?.code == code
        case .cancelled: matches = error is CancellationError
        }
        try require(matches, "wrong rejection: \(error)")
        return
    }
    throw Failure(text: "unexpected acceptance")
}
func snapshot(_ folder: URL) throws -> [String: Data] {
    var result: [String: Data] = [:]
    if let e = fm.enumerator(at: folder, includingPropertiesForKeys: [.isRegularFileKey]) {
        for case let u as URL in e where (try u.resourceValues(forKeys: [.isRegularFileKey])).isRegularFile == true {
            result[String(u.path.dropFirst(folder.path.count))] = try Data(contentsOf: u)
        }
    }
    return result
}
func test(_ name: String, _ body: (URL) throws -> Void) throws {
    let tree = runs.appendingPathComponent(String(passed))
    let root = tree.appendingPathComponent("Documents")
    let appSave = tree.appendingPathComponent("Application Support/SheikahVision/save/2147483649/1")
    try fm.createDirectory(at: appSave, withIntermediateDirectories: true)
    try Data("application support save sentinel".utf8).write(to: appSave.appendingPathComponent("game_data.sav"))
    let appBefore = try snapshot(tree.appendingPathComponent("Application Support/SheikahVision/save"))
    let save = root.appendingPathComponent("save/account/1")
    try fm.createDirectory(at: save, withIntermediateDirectories: true)
    try Data((0..<1024).map { UInt8($0 % 256) }).write(to: save.appendingPathComponent("game_data.sav"))
    try Data("unchanged option".utf8).write(to: root.appendingPathComponent("save/option.sav"))
    let before = try snapshot(root.appendingPathComponent("save"))
    try body(root)
    try require(try snapshot(root.appendingPathComponent("save")) == before, "save changed in \(name)")
    try require(try snapshot(tree.appendingPathComponent("Application Support/SheikahVision/save")) == appBefore, "Application Support save changed in \(name)")
    passed += 1; print("PASS \(passed): \(name) [Documents + Application Support saves byte-identical]")
}
func engine(_ root: URL, release: Bool = false, fault: @escaping (String) throws -> Void = { _ in },
            space: @escaping () throws -> Int64 = { 100 << 30 }, manifest: Bool = false) -> BotWGameImport {
    BotWGameImport(root: root, manifestFolder: manifest ? fixtures.appendingPathComponent("manifest") : nil,
                   freeSpace: space, fault: fault, expectedRPX: expected, releaseProbe: release ? nil : { _ in false })
}
func putZip(_ name: String, at url: URL) throws {
    try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    // Real byte copy, never FileManager's APFS clone (except the >4 GiB sparse fixture, tested directly).
    try Data(contentsOf: fixtures.appendingPathComponent(name + ".zip")).write(to: url)
}
func folder(_ root: URL, fixture: String = "stored") throws {
    let r = try BotWZipReader(url: fixtures.appendingPathComponent(fixture + ".zip"))
    for e in try r.gameFiles(root: r.gameRoot()!) {
        try r.extract(e, to: root.appendingPathComponent(e.name), token: BotWCancelToken()) { _ in }
    }
}
func blocks(_ url: URL) -> Int64 { var st = stat(); stat(url.path, &st); return st.st_blocks }

do {
    for name in ["stored", "deflate", "prefix", "unicode", "junk", "case", "comment"] {
        try test("zip \(name)") { root in
            let e = engine(root); try e.recover()
            let zip = e.pending.appendingPathComponent("input.zip"); try putZip(name, at: zip)
            let source = try e.discover().first!; try e.run(source)
            try require(try e.receipt()?.titleVersion == 208, "receipt")
            try require(fm.fileExists(atPath: zip.path) == (name == "case"), "source cleanup")
        }
    }
    try test("forced ZIP64 (zip -fz)") { root in
        let reader = try BotWZipReader(url: fixtures.appendingPathComponent("forced64.zip"))
        try reader.extract(reader.entries.first!, to: root.appendingPathComponent("out"), token: BotWCancelToken()) { _ in }
        try require(try Data(contentsOf: root.appendingPathComponent("out")) == Data("zip64".utf8), "zip64")
    }
    try test("sparse offsets >4 GiB, 32-bit descriptors, central sizes") { root in
        let reader = try BotWZipReader(url: fixtures.appendingPathComponent("sparse.zip"))
        try require(reader.entries.allSatisfy { $0.headerOffset > 1 << 32 }, "offsets")
        for entry in try reader.gameFiles(root: "") {
            try reader.extract(entry, to: root.appendingPathComponent(entry.name), token: BotWCancelToken()) { _ in }
        }
        try require(try BotWGameImport.sha256(root.appendingPathComponent("code/U-King.rpx")) == expected, "descriptor regression")
    }
    try test("CP437 local/central filename decoding") { root in
        let reader = try BotWZipReader(url: fixtures.appendingPathComponent("cp437.zip"))
        let entry = reader.entries.first!
        try require(entry.name.contains("é"), "CP437")
        try reader.extract(entry, to: root.appendingPathComponent("out"), token: BotWCancelToken()) { _ in }
    }
    for name in ["encrypted", "bzip2", "split", "truncated", "bad-central", "dotdot", "absolute", "backslash", "colon", "empty-segment", "dot-segment", "symlink", "duplicate", "entry-limit", "single-limit", "local-name", "bomb", "crc", "overlap", "local-encrypted"] {
        try test("reject \(name)") { root in
            let expectedError: Rejection = name == "overlap" ? .message(tr("import.overlap")) :
                ["dotdot", "absolute", "backslash", "colon", "empty-segment", "dot-segment", "symlink"].contains(name) ? .contains("unsafe path") : .importError
            try rejected(expectedError) {
                let reader = try BotWZipReader(url: fixtures.appendingPathComponent(name + ".zip"))
                for entry in try reader.gameFiles(root: reader.gameRoot() ?? "") {
                    try reader.extract(entry, to: root.appendingPathComponent(entry.name), token: BotWCancelToken()) { _ in }
                }
            }
        }
    }
    for region in ["JP", "US", "EU"] {
        try test("\(region) identify, import and language receipt") { root in
            let e = engine(root); try e.recover()
            let zip = e.pending.appendingPathComponent("input.zip"); try putZip(region, at: zip)
            let source = try e.inspect(zip)
            try require(source.region == region && source.languages == BotWGameImport.languages[region]!, "identify region")
            try e.run(source)
            try require(try e.receipt()?.languages == BotWGameImport.languages[region]!, "receipt languages")
            if region != "JP" { try rejected { try e.deepValidate() } }
        }
    }
    for name in ["old-version", "unknown-title", "missing-meta", "wrong-rpx", "wrong-language"] {
        try test("reject version/region: \(name)") { root in
            let e = engine(root); try e.recover(); let zip = e.pending.appendingPathComponent("input.zip"); try putZip(name, at: zip)
            try rejected(name == "wrong-rpx" ? .message(tr("import.rpx")) : .importError) { try e.run(e.inspect(zip)) }
            try require(!fm.fileExists(atPath: e.game.path), "bad game installed")
        }
    }
    for depth in [0, 2, 3, 4] {
        try test("Import zip at depth \(depth)") { root in
            let e = engine(root); try e.recover()
            var target = e.pending
            for _ in 0..<depth { target.appendPathComponent("nested") }
            try putZip("stored", at: target.appendingPathComponent("game.zip"))
            try require(try e.discover().count == (depth == 4 ? 0 : 1), "depth")
        }
    }
    try test("Import folder moves, inode unchanged; empty parents removed") { root in
        let e = engine(root); try e.recover(); let source = e.pending.appendingPathComponent("one/two/game")
        try folder(source)
        let before = try BotWGameReceipt.Fingerprint.read(source.appendingPathComponent("content/test.bin")).inode
        try e.run(e.discover().first!)
        try require(try BotWGameReceipt.Fingerprint.read(e.game.appendingPathComponent("content/test.bin")).inode == before, "copied instead of moved")
        try require(!fm.fileExists(atPath: e.pending.appendingPathComponent("one").path), "empty parents")
    }
    try test("picker folder copy regression; source kept") { root in
        let e = engine(root); let source = root.appendingPathComponent("external"); try folder(source)
        let before = try BotWGameReceipt.Fingerprint.read(source.appendingPathComponent("content/test.bin")).inode
        try e.run(e.inspect(source))
        try require(try BotWGameReceipt.Fingerprint.read(source.appendingPathComponent("content/test.bin")).inode == before, "external mutated")
        try require(try BotWGameReceipt.Fingerprint.read(e.game.appendingPathComponent("content/test.bin")).inode != before, "not copied")
    }
    try test("multiple sources, completeness order") { root in
        let e = engine(root); try e.recover()
        try putZip("stored", at: e.pending.appendingPathComponent("one.zip")); try putZip("EU", at: e.pending.appendingPathComponent("two.zip"))
        let list = try e.discover(); try require(list.count == 2 && list.first?.region == "EU", "ranking")
    }
    try test("unsupported WUD, WUX, 7z, rar, wua and split extension notices") { root in
        let e = engine(root); try e.recover()
        for ext in ["wud", "wux", "7z", "rar", "wua", "z01"] { try Data([1]).write(to: e.pending.appendingPathComponent("game." + ext)) }
        try require(try e.discover().isEmpty && e.notices.count == 6, "format notices")
    }
    try test("incomplete >1 MiB zip explains copy may be unfinished") { root in
        let e = engine(root); try e.recover(); let url = e.pending.appendingPathComponent("large.zip")
        try Data(repeating: 1, count: 2 << 20).write(to: url)
        do { _ = try e.inspect(url); throw Failure(text: "accepted") }
        catch let error as BotWZipError { try require(error.message.contains("copying"), "specific notice") }
    }
    try test("DLC ignored, source retained and notice") { root in
        let e = engine(root); try e.recover(); let url = e.pending.appendingPathComponent("dlc.zip"); try putZip("dlc", at: url)
        let source = try e.inspect(url); try require(source.hasDLC, "DLC detection")
        try e.run(source)
        try require(fm.fileExists(atPath: url.path) && !e.notices.isEmpty && !fm.fileExists(atPath: e.game.appendingPathComponent("aoc").path), "DLC cleanup")
    }
    for archive in [true, false] {
        try test("DLC detected by meta with arbitrary folder name, archive=\(archive)") { root in
            let e = engine(root); try e.recover()
            let source: BotWImportSource
            if archive {
                let url = e.pending.appendingPathComponent("dlc.zip"); try putZip("dlc-meta", at: url)
                source = try e.inspect(url)
            } else {
                let url = e.pending.appendingPathComponent("dlc-folder"); try folder(url, fixture: "dlc-meta")
                source = try e.inspect(url)
            }
            try require(source.hasDLC && !source.files.contains(where: { $0.path.hasPrefix("content/Bonus/") }), "DLC meta ignored")
            try e.run(source)
            try require(fm.fileExists(atPath: source.url.path) && !fm.fileExists(atPath: e.game.appendingPathComponent("content/Bonus").path), "DLC source retention")
        }
    }
    try test("available space insufficient") { root in
        let e = engine(root, space: { 1 }); try e.recover(); let url = e.pending.appendingPathComponent("a.zip"); try putZip("stored", at: url)
        try rejected(.contains("Not enough free space")) { try e.run(e.inspect(url)) }; try require(e.lastNeeded > 1, "space formula")
    }
    try test("available space unreadable") { root in
        let e = engine(root, space: { throw CocoaError(.fileReadUnknown) }); try e.recover()
        let url = e.pending.appendingPathComponent("a.zip"); try putZip("stored", at: url)
        try rejected { try e.run(e.inspect(url)) }
    }
    try test("four space formulas, releasing and nonreleasing") { _ in
        try require(BotWGameImport.needed(uncompressed: 100, compressed: 80, largest: 10, releasing: true, moving: false) == (1 << 30) + 30, "releasing")
        try require(BotWGameImport.needed(uncompressed: 100, compressed: 80, largest: 10, releasing: false, moving: false) == (1 << 30) + 100, "copying")
        try require(BotWGameImport.needed(uncompressed: 100, compressed: 80, largest: 10, releasing: false, moving: true) == 1 << 30, "moving")
        try require(BotWGameImport.needed(uncompressed: 50, compressed: 80, largest: 10, releasing: true, moving: false) == (1 << 30) + 10, "nonnegative")
    }
    for point in ["extract:2", "journal", "checkpoint", "committing", "old", "installed", "committed"] {
        try test("fault at \(point), recover, resume, old/new always complete") { root in
            let clean = engine(root); try clean.recover()
            try folder(clean.game); try Data("old".utf8).write(to: clean.game.appendingPathComponent("old-marker"))
            let old = try snapshot(clean.game)
            let url = clean.pending.appendingPathComponent("new.zip"); try putZip("release", at: url)
            var fired = false
            let broken = engine(root, release: true, fault: { label in
                if label == point && !fired { fired = true; throw CocoaError(.fileWriteUnknown) }
            })
            let source = try broken.inspect(url)
            try rejected(.cocoa(.fileWriteUnknown)) { try broken.run(source) }
            try require(fired, "fault not hit")
            try clean.recover()
            let current = try snapshot(clean.game)
            try require(current == old || (current["/old-marker"] == nil && current.count == source.files.count), "partial installed")
            if point != "committed" { try clean.run(clean.discover().first!) }
            try require(try clean.receipt()?.fileCount == source.files.count, "resume completion")
            try require(!fm.fileExists(atPath: root.appendingPathComponent(".game-import").path), "work cleanup")
        }
    }
    try test("cancel and resume after real punching, st_blocks decreases and size unchanged") { root in
        let e = engine(root, release: true); try e.recover()
        try require(BotWZipRelease.probe(in: e.pending), "APFS hole release unavailable on test volume")
        let url = e.pending.appendingPathComponent("large.zip"); try putZip("release", at: url)
        let size = try BotWGameReceipt.Fingerprint.read(url).size, before = blocks(url)
        let token = BotWCancelToken(); var afterFirst: Int64?
        let broken = engine(root, release: true, fault: { point in
            if point == "extract:70" { afterFirst = blocks(url); token.cancel() }
        })
        let source = try broken.inspect(url)
        try rejected(.cancelled) { try broken.run(source, token: token) }
        try require(afterFirst! < before && (try BotWGameReceipt.Fingerprint.read(url).size) == size, "no incremental release")
        let beforeResume = blocks(url)
        var decreasedAgain = false
        let resumed = engine(root, release: true, fault: { point in if point == "extract:130" { decreasedAgain = blocks(url) < beforeResume } })
        try resumed.run(resumed.discover().first!)
        try require(decreasedAgain, "no further release")
    }
    try test("corrupt entry never releases its bytes") { root in
        let e = engine(root, release: true); try e.recover(); let url = e.pending.appendingPathComponent("crc.zip"); try putZip("crc", at: url)
        let before = blocks(url)
        try rejected { try e.run(e.inspect(url)) }
        try require(blocks(url) == before, "corrupt archive punched")
    }
    try test("folder interruption after moving a file resumes") { root in
        let clean = engine(root); try clean.recover(); let source = clean.pending.appendingPathComponent("folder"); try folder(source)
        let broken = engine(root, fault: { if $0 == "extract:2" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try broken.run(broken.inspect(source)) }
        try clean.recover(); try clean.run(clean.discover().first!)
        try require(try clean.receipt() != nil, "folder resume")
    }
    try test("receipt skips enumeration, child mtime change forces recount") { root in
        let e = engine(root, manifest: true); let source = root.appendingPathComponent("external"); try folder(source)
        try e.run(e.inspect(source)); let counts = e.statisticsRuns
        _ = try e.receipt(); _ = try e.receipt()
        try require(e.statisticsRuns == counts, "cold-start enumeration")
        try fm.setAttributes([.modificationDate: Date(timeIntervalSinceNow: 60)], ofItemAtPath: e.game.appendingPathComponent("content/test.bin").path)
        _ = try e.receipt(); try require(e.statisticsRuns == counts + 1, "mtime not detected")
        try e.deepValidate()
        var data = try Data(contentsOf: e.game.appendingPathComponent("content/test.bin")); data[0] ^= 1
        try data.write(to: e.game.appendingPathComponent("content/test.bin"))
        try rejected { try e.deepValidate() }
    }
    try test("destination symlink rejected") { root in
        let outside = root.appendingPathComponent("outside"); try fm.createDirectory(at: outside, withIntermediateDirectories: true)
        try fm.createSymbolicLink(at: root.appendingPathComponent("content"), withDestinationURL: outside)
        let reader = try BotWZipReader(url: fixtures.appendingPathComponent("stored.zip"))
        let entry = reader.entries.first(where: { $0.name == "content/test.bin" })!
        try rejected(.contains("unsafe path")) { try reader.extract(entry, to: root.appendingPathComponent(entry.name), token: BotWCancelToken()) { _ in } }
        try require(try fm.contentsOfDirectory(atPath: outside.path).isEmpty, "symlink followed")
    }
    try test("source symlink rejected, save never followed") { root in
        let e = engine(root); try e.recover()
        try fm.createSymbolicLink(at: e.pending.appendingPathComponent("source"), withDestinationURL: root.appendingPathComponent("save"))
        try rejected { _ = try e.discover() }
    }
    try test("abandon and delete game leave saves unchanged") { root in
        let e = engine(root); try e.recover(); let url = e.pending.appendingPathComponent("input.zip"); try putZip("stored", at: url)
        let token = BotWCancelToken(); token.cancel()
        try rejected(.cancelled) { try e.run(e.inspect(url), token: token) }
        try e.abandon(); try require(!e.unfinished, "abandon")
        try e.run(e.inspect(url)); try e.deleteGame(); try require(!fm.fileExists(atPath: e.game.path), "delete")
    }
    try test("missing source clears unfinished staging") { root in
        let e = engine(root); try e.recover()
        let url = e.pending.appendingPathComponent("input.zip"); try putZip("stored", at: url)
        let broken = engine(root, fault: { if $0 == "extract:2" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try broken.run(broken.inspect(url)) }
        try fm.removeItem(at: url); try e.recover()
        try require(!e.unfinished, "missing source not cleared")
    }
    try test("resume rejects replacement source without touching old game") { root in
        let e = engine(root); try e.recover(); try folder(e.game)
        let old = try snapshot(e.game)
        let url = e.pending.appendingPathComponent("input.zip"); try putZip("stored", at: url)
        let broken = engine(root, fault: { if $0 == "extract:2" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try broken.run(broken.inspect(url)) }
        try fm.removeItem(at: url); try putZip("deflate", at: url)
        try rejected { try e.run(e.discover().first!) }
        try require(try snapshot(e.game) == old, "old mutated")
    }
    try test("large entries, prefix expansion and edge retention reserve space") { root in
        let e = engine(root, release: true); try e.recover()
        let url = e.pending.appendingPathComponent("large.zip"); try putZip("stored", at: url)
        var source = try e.inspect(url)
        let index = source.files.firstIndex(where: { $0.path == "content/test.bin" })!
        source.files[index].size = 4 << 30
        source.files[index].zip!.size = 4 << 30
        source.files[index].zip!.compressedSize = 4 << 30
        try e.previewSpace(source)
        try require(e.releaseSupported && e.lastNeeded > 8 << 30, "large retained checkpoint undercounted")
        var grows = source.files[index], later = source.files[index]
        grows.size = 1 << 30; grows.zip!.size = 1 << 30; grows.zip!.compressedSize = 0
        later.size = 0; later.zip!.size = 0; later.zip!.compressedSize = 1 << 30
        source.files = [grows, later]
        try e.previewSpace(source)
        try require(e.lastNeeded > 3 << 30, "later compressed bytes hid earlier expansion peak")
    }
    for point in ["released", "backupRemoved", "receipt", "sourceRemoved", "parentRemoved"] {
        try test("recover interrupted release/cleanup at \(point)") { root in
            let e = engine(root); try e.recover(); try folder(e.game)
            let url = e.pending.appendingPathComponent("outer/inner/input.zip"); try putZip("release", at: url)
            var fired = false
            let broken = engine(root, release: true, fault: { if $0 == point && !fired { fired = true; throw CocoaError(.fileWriteUnknown) } })
            try rejected(.cocoa(.fileWriteUnknown)) { try broken.run(broken.inspect(url)) }
            try require(fired, "hook missed")
            try e.recover(); try e.recover()
            if point == "released" { try e.run(e.discover().first!) }
            try require(try e.receipt()?.fileCount == 136, "cleanup recovery receipt")
            try require(!e.unfinished, "cleanup did not finish")
        }
    }
    try test("temporarily unavailable external source preserves progress") { root in
        let e = engine(root); try e.recover(); let url = root.appendingPathComponent("external.zip"); try putZip("stored", at: url)
        let broken = engine(root, fault: { if $0 == "extract:2" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try broken.run(broken.inspect(url)) }
        let held = root.appendingPathComponent("unmounted.zip"); try fm.moveItem(at: url, to: held)
        try e.recover(); try require(e.unfinished && !e.notices.isEmpty, "external work discarded")
        try fm.moveItem(at: held, to: url); try e.run(e.discover().first!)
        try require(try e.receipt() != nil, "external resume")
    }
    try test("folder cleanup preserves unrecognized files") { root in
        let e = engine(root); try e.recover(); let source = e.pending.appendingPathComponent("folder"); try folder(source)
        try Data("keep".utf8).write(to: source.appendingPathComponent("notes.txt"))
        try e.run(e.inspect(source))
        try require(try String(contentsOf: source.appendingPathComponent("notes.txt"), encoding: .utf8) == "keep", "unknown source deleted")
    }
    try test("damaged receipt metadata forces validation") { root in
        let e = engine(root); try e.recover(); let source = root.appendingPathComponent("external"); try folder(source)
        try e.run(e.inspect(source)); let count = e.statisticsRuns
        let url = root.appendingPathComponent("game-receipt.json")
        var receipt = try JSONDecoder().decode(BotWGameReceipt.self, from: Data(contentsOf: url))
        receipt.titleID = "invalid"
        try JSONEncoder().encode(receipt).write(to: url)
        try require(try e.receipt()?.titleID == "00050000101C9300" && e.statisticsRuns == count + 1, "bad receipt trusted")
    }
    try test("failed block release persists full-copy fallback and space check") { root in
        let pending = root.appendingPathComponent("Import")
        let free: Int64 = (1 << 30) + (1 << 20)
        let broken = engine(root, release: true, fault: { if $0 == "release" { throw CocoaError(.fileWriteUnknown) } }, space: { free })
        try broken.recover(); let url = pending.appendingPathComponent("input.zip"); try putZip("release", at: url)
        try rejected(.contains("Not enough free space")) { try broken.run(broken.inspect(url)) }
        try require(!broken.releaseSupported && broken.lastNeeded > UInt64(free), "no fallback space check")
        let resumed = engine(root, release: true, space: { free })
        try rejected { try resumed.previewSpace(resumed.discover().first!) }
        try require(!resumed.releaseSupported, "fallback not persisted")
        let enough = engine(root, release: true); try enough.run(enough.discover().first!)
        try require(!enough.releaseSupported && (try enough.receipt()) != nil, "fallback failed to finish")
    }
    try test("cancellation during validation keeps old game and resumes") { root in
        let e = engine(root); try e.recover(); try folder(e.game); let old = try snapshot(e.game)
        let url = e.pending.appendingPathComponent("input.zip"); try putZip("stored", at: url)
        let token = BotWCancelToken()
        try rejected(.cancelled) { try e.run(e.inspect(url), token: token, progress: { if $0.stage == "validating" { token.cancel() } }) }
        try require(try snapshot(e.game) == old, "validation cancellation installed game")
        try e.run(e.discover().first!)
    }
    try test("OS /tmp alias remains owned after source deletion") { root in
        let alias = URL(fileURLWithPath: root.path.replacingOccurrences(of: "/private/tmp/", with: "/tmp/"))
        let e = engine(alias); try e.recover()
        let zip = root.appendingPathComponent("Import/input.zip"); try putZip("stored", at: zip)
        try e.run(e.inspect(zip))
        try require(!fm.fileExists(atPath: zip.path), "alias source not cleaned")
        try require(try e.receipt() != nil, "alias receipt")
    }
    for point in ["committing", "old", "installed", "committed"] {
        try test("first install without old game, fault at \(point)") { root in
            let e = engine(root); try e.recover(); let url = e.pending.appendingPathComponent("input.zip"); try putZip("stored", at: url)
            let broken = engine(root, fault: { if $0 == point { throw CocoaError(.fileWriteUnknown) } })
            try rejected(.cocoa(.fileWriteUnknown)) { try broken.run(broken.inspect(url)) }
            try e.recover(); try e.recover()
            if point != "committed" { try e.run(e.discover().first!) }
            try require(try e.receipt()?.fileCount == 6, "incomplete first installation")
        }
    }
    try test("DLC bytes remain readable after main archive entries are punched") { root in
        let e = engine(root, release: true); try e.recover()
        let url = e.pending.appendingPathComponent("dlc.zip"); try putZip("release-dlc", at: url)
        let source = try e.inspect(url), before = blocks(url)
        try e.run(source)
        try require(source.hasDLC && blocks(url) < before, "archive not retained/punched")
        let reader = try BotWZipReader(url: url, verifiedEntries: source.files.compactMap(\.zip))
        let dlc = reader.entries.first(where: { $0.name == "aoc/content/dlc.bin" })!
        let output = root.appendingPathComponent("dlc-check")
        try reader.extract(dlc, to: output, token: BotWCancelToken()) { _ in }
        try require(try Data(contentsOf: output) == Data(repeating: 0xdc, count: 65536), "DLC was released")
    }
    try test("picker rejects folder without RPX before moving any files") { root in
        let e = engine(root); try e.recover(); let source = e.pending.appendingPathComponent("incomplete-folder")
        try folder(source); try fm.removeItem(at: source.appendingPathComponent("code/U-King.rpx"))
        let before = try snapshot(source)
        try rejected { _ = try e.inspect(source) }
        try require(try snapshot(source) == before && !e.unfinished, "invalid source was changed")
    }
    try test("regional default languages and system fallback") { _ in
        try require(BotWGameImport.defaultLanguage(region: "JP", available: ["JPja"]) == "JPja", "JP default")
        try require(BotWGameImport.defaultLanguage(region: "US", available: ["USfr", "USen"]) == "USen", "US default")
        try require(BotWGameImport.defaultLanguage(region: "EU", available: ["EUen", "EUde"], preferred: ["de-DE"]) == "EUde", "EU system")
        try require(BotWGameImport.defaultLanguage(region: "EU", available: ["EUen", "EUde"], preferred: ["pt-BR"]) == "EUen", "EU fallback")
    }
    for archive in [false, true] {
        try test("review A: wrong RPX rejected before consumption, archive=\(archive)") { root in
            let e = engine(root, release: archive); try e.recover()
            let url = e.pending.appendingPathComponent(archive ? "input.zip" : "folder")
            if archive { try putZip("wrong-rpx", at: url) } else { try folder(url, fixture: "wrong-rpx") }
            let bytes = archive ? try Data(contentsOf: url) : nil
            let tree = archive ? [:] : try snapshot(url)
            let allocated = archive ? blocks(url) : 0
            try rejected(.message(tr("import.rpx"))) { _ = try e.inspect(url) }
            if let bytes { try require(try Data(contentsOf: url) == bytes && blocks(url) == allocated, "rejected zip changed") }
            else { try require(try snapshot(url) == tree, "rejected folder changed") }
            try require(!e.unfinished && !fm.fileExists(atPath: root.appendingPathComponent(".game-import/staging").path), "preflight left transaction")
        }
        try test("RPX rechecked at start after a successful preview, archive=\(archive)") { root in
            let e = engine(root, release: archive); try e.recover()
            let url = e.pending.appendingPathComponent(archive ? "input.zip" : "folder")
            if archive { try putZip("stored", at: url) } else { try folder(url) }
            let source = try e.inspect(url)
            if archive { try putZip("wrong-rpx", at: url) }
            else { try Data("wrong".utf8).write(to: url.appendingPathComponent("code/U-King.rpx")) }
            let before = archive ? ["zip": try Data(contentsOf: url)] : try snapshot(url)
            let allocated = archive ? blocks(url) : 0
            try rejected(.message(tr("import.rpx"))) { try e.run(source) }
            try require(try (archive ? ["zip": Data(contentsOf: url)] : snapshot(url)) == before, "start consumed wrong source")
            if archive { try require(blocks(url) == allocated, "start punched invalid RPX") }
            try require(!e.unfinished && !fm.fileExists(atPath: root.appendingPathComponent(".game-import/staging").path), "start left transaction")
        }
        try test("manifest size preflight preserves source, archive=\(archive)") { root in
            let e = engine(root, release: archive, manifest: true); try e.recover()
            let url = e.pending.appendingPathComponent(archive ? "input.zip" : "folder")
            if archive { try putZip("manifest-size", at: url) } else { try folder(url, fixture: "manifest-size") }
            let before = archive ? ["zip": try Data(contentsOf: url)] : try snapshot(url)
            try rejected(.message(tr("files.zipCorrupt", "content/test.bin"))) { _ = try e.inspect(url) }
            try require(try (archive ? ["zip": Data(contentsOf: url)] : snapshot(url)) == before && !e.unfinished, "manifest preflight consumed source")
        }
    }
    for state in ["no-journal", "source-only", "half-trash"] {
        for operation in ["recover", "abandon", "deleteGame", "discover"] {
            try test("review B: \(state), \(operation) remains usable") { root in
                let e = engine(root); try e.recover(); try folder(e.game)
                let before = try snapshot(e.game)
                let work = root.appendingPathComponent(state == "half-trash" ? ".game-import-trash-" + UUID().uuidString : ".game-import")
                try fm.createDirectory(at: work.appendingPathComponent("staging/content"), withIntermediateDirectories: true)
                try Data("leftover".utf8).write(to: work.appendingPathComponent("staging/content/part"))
                try Data("{}".utf8).write(to: work.appendingPathComponent("source.json"))
                if state == "source-only" { try fm.removeItem(at: work.appendingPathComponent("staging")) }
                switch operation {
                case "recover": try e.recover()
                case "abandon": try e.abandon()
                case "deleteGame":
                    try e.recover(); try require(try snapshot(e.game) == before, "recovery damaged old game")
                    try e.deleteGame()
                default: _ = try e.discover()
                }
                try e.recover()
                if operation != "deleteGame" { try require(try snapshot(e.game) == before, "orphan cleanup damaged game") }
                else { try require(!fm.fileExists(atPath: e.game.path), "explicit delete failed") }
                try require(!fm.fileExists(atPath: work.path), "orphan or tombstone stuck")
            }
        }
    }
    for gameExists in [false, true] {
        try test("orphan old backup preserved, game exists=\(gameExists)") { root in
            let e = engine(root); try e.recover()
            if gameExists { try folder(e.game) }
            let backup = root.appendingPathComponent(".game-import/old")
            try folder(backup); try Data("backup".utf8).write(to: backup.appendingPathComponent("marker"))
            let before = try snapshot(backup)
            try e.recover(); try e.abandon(); _ = try e.discover()
            try require(try snapshot(gameExists ? backup : e.game) == before, "old backup was deleted")
            try require(!gameExists || (e.recoveryNeedsAttention && !e.notices.isEmpty), "conflicting old backup hidden")
        }
    }
    for cleanup in ["abandon", "source-disappeared"] {
        try test("tombstone rename survives interruption during \(cleanup)") { root in
            let e = engine(root); try e.recover(); try folder(e.game)
            let before = try snapshot(e.game), url = e.pending.appendingPathComponent("input.zip")
            try putZip("stored", at: url)
            let start = engine(root, fault: { if $0 == "extract:2" { throw CocoaError(.fileWriteUnknown) } })
            try rejected(.cocoa(.fileWriteUnknown)) { try start.run(start.inspect(url)) }
            if cleanup == "source-disappeared" { try fm.removeItem(at: url) }
            let interrupted = engine(root, fault: { if $0 == "trashRenamed" { throw CocoaError(.fileWriteUnknown) } })
            try rejected(.cocoa(.fileWriteUnknown)) {
                if cleanup == "abandon" { try interrupted.abandon() } else { try interrupted.recover() }
            }
            let trash = try fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil).first { $0.lastPathComponent.hasPrefix(".game-import-trash-") }!
            try? fm.removeItem(at: trash.appendingPathComponent("journal.json"))
            try e.recover(); try e.abandon(); _ = try e.discover()
            try require(!e.unfinished && (try snapshot(e.game)) == before, "tombstone recovery damaged game")
        }
    }
    try test("invalid journal reports journalBad specifically") { root in
        let e = engine(root); try e.recover(); let url = e.pending.appendingPathComponent("input.zip"); try putZip("stored", at: url)
        let interrupted = engine(root, fault: { if $0 == "journal" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try interrupted.run(interrupted.inspect(url)) }
        let journal = root.appendingPathComponent(".game-import/journal.json")
        var json = try JSONSerialization.jsonObject(with: Data(contentsOf: journal)) as! [String: Any]
        json["phase"] = "invalid"
        try JSONSerialization.data(withJSONObject: json).write(to: journal)
        try rejected(.message(tr("import.journalBad"))) { try e.recover() }
    }
    for missing in [true, false] {
        try test("review C: failed selected source does not hide Import, missing=\(missing)") { root in
            let e = engine(root); try e.recover(); try putZip("stored", at: e.pending.appendingPathComponent("valid.zip"))
            let selected = root.appendingPathComponent("bad-selection")
            if !missing { try fm.createDirectory(at: selected, withIntermediateDirectories: true) }
            let list = try e.discover(selected: selected)
            try require(list.count == 1 && list.first?.url.lastPathComponent == "valid.zip" && e.selectionFailed, "Import scan blocked")
            try require(e.notices.contains(tr(missing ? "import.format" : "files.noRpx")), "failed selection notice absent")
        }
    }
    try test("active external-resume notice survives discover") { root in
        let e = engine(root); try e.recover(); let url = root.appendingPathComponent("external.zip"); try putZip("stored", at: url)
        let interrupted = engine(root, fault: { if $0 == "journal" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try interrupted.run(interrupted.inspect(url)) }
        try fm.removeItem(at: url)
        _ = try e.discover()
        try require(e.notices.contains(tr("import.externalResume")), "recover notice cleared")
    }
    try test("valid user selection has priority over more complete Import") { root in
        let e = engine(root); try e.recover(); try putZip("EU", at: e.pending.appendingPathComponent("larger.zip"))
        let selected = root.appendingPathComponent("selected.zip"); try putZip("stored", at: selected)
        let sources = try e.discover(selected: selected)
        try require(sources.count == 2 && sources.first?.url == selected && !e.selectionFailed, "user selection silently replaced")
    }
    try test("review D: archive containing notes is retained") { root in
        let e = engine(root); try e.recover(); let url = e.pending.appendingPathComponent("notes.zip"); try putZip("notes", at: url)
        try e.run(e.inspect(url))
        try require(try e.receipt() != nil && fm.fileExists(atPath: url.path) && e.notices.contains(tr("import.archiveKept")), "archive notes deleted")
        let reader = try BotWZipReader(url: url), entry = reader.entries.first { $0.name == "my-notes/readme.txt" }!
        let output = root.appendingPathComponent("readme")
        try reader.extract(entry, to: output, token: BotWCancelToken()) { _ in }
        try require(try String(contentsOf: output, encoding: .utf8) == "keep me", "notes bytes changed")
    }
    for mode in ["unknown", "parent", "known"] {
        try test("add-on metadata uses relationship or configured table, \(mode)") { root in
            let e = BotWGameImport(root: root, manifestFolder: nil, freeSpace: { 100 << 30 }, expectedRPX: expected,
                                   releaseProbe: { _ in false }, knownDLC: mode == "known" ? ["00050000101C9300": ["0005000C101D6000"]] : [:])
            try e.recover(); let url = e.pending.appendingPathComponent("addon.zip"); try putZip(mode == "parent" ? "parent-addon" : "unknown-addon", at: url)
            let source = try e.inspect(url)
            try require(source.hasDLC == (mode != "unknown") && source.hasUnplannedEntries == true, "DLC guessed or relationship missed")
            try e.run(source)
            try require(fm.fileExists(atPath: url.path) && !fm.fileExists(atPath: e.game.appendingPathComponent("content/Bonus").path), "unknown add-on consumed")
        }
    }
    try test("abandoned punched zip is retained with explicit warning and skipped") { root in
        let e = engine(root); try e.recover(); let url = e.pending.appendingPathComponent("input.zip"); try putZip("release", at: url)
        let interrupted = engine(root, release: true, fault: { if $0 == "released" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try interrupted.run(interrupted.inspect(url)) }
        try e.abandon()
        try require(e.notices.contains(tr("import.partialArchive")) && fm.fileExists(atPath: url.path) && !e.unfinished, "partial archive abandonment unclear")
        try require(try e.discover().isEmpty && e.notices.contains(tr("import.partialArchive")), "partial archive rescanned as corrupt")
    }
    try test("game directly in Import moves only game trees") { root in
        let e = engine(root); try e.recover(); try folder(e.pending)
        try Data("keep".utf8).write(to: e.pending.appendingPathComponent("notes.txt"))
        let original = try BotWGameReceipt.Fingerprint.read(e.pending.appendingPathComponent("content/test.bin")).inode
        let source = try e.discover().first!; try e.previewSpace(source)
        try require(e.lastNeeded == 1 << 30, "Import root used copy formula")
        try e.run(source)
        try require(try BotWGameReceipt.Fingerprint.read(e.game.appendingPathComponent("content/test.bin")).inode == original, "Import root copied")
        try require(try String(contentsOf: e.pending.appendingPathComponent("notes.txt"), encoding: .utf8) == "keep", "Import root unrelated file changed")
    }
    try test("resume always uses journal plan rather than stale caller fields") { root in
        let e = engine(root); try e.recover(); let url = e.pending.appendingPathComponent("input.zip"); try putZip("release", at: url)
        let interrupted = engine(root, fault: { if $0 == "extract:70" { throw CocoaError(.fileWriteUnknown) } })
        let original = try interrupted.inspect(url)
        try rejected(.cocoa(.fileWriteUnknown)) { try interrupted.run(original) }
        var stale = original; stale.files = []; stale.archive = false
        var total: UInt64 = 0
        try e.run(stale, progress: { total = $0.total })
        try require(total == original.bytes && (try e.receipt()?.fileCount) == original.files.count, "mixed caller and journal plans")
    }
    for effective in [false, true] {
        try test("volume reclaim window checks real capacity as well as blocks, effective=\(effective)") { root in
            let pending = root.appendingPathComponent("Import"), url = pending.appendingPathComponent("input.zip")
            try fm.createDirectory(at: pending, withIntermediateDirectories: true); try putZip("release", at: url)
            let initial = blocks(url) * 512
            func stagedBlocks() -> Int64 {
                let staging = root.appendingPathComponent(".game-import/staging")
                var sum: Int64 = 0
                if let walk = fm.enumerator(at: staging, includingPropertiesForKeys: [.isRegularFileKey]) {
                    for case let file as URL in walk where (try? file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true { sum += blocks(file) * 512 }
                }
                return sum
            }
            let e = BotWGameImport(root: root, manifestFolder: nil, freeSpace: {
                (100 << 30) - stagedBlocks() + (effective ? max(0, initial - blocks(url) * 512) : 0)
            }, expectedRPX: expected, releaseWindowBytes: 1 << 20)
            try e.run(e.inspect(url))
            try require(e.releaseSupported == effective, "volume reclaim fallback decision")
            try require(try e.receipt() != nil && (effective || e.notices.contains(tr("import.releaseFallback"))), "volume fallback did not complete")
        }
    }
    for external in [false, true] {
        try test("abandon does not mislabel an unpunched archive, external=\(external)") { root in
            let e = engine(root); try e.recover()
            let url = (external ? root : e.pending).appendingPathComponent("input.zip"); try putZip("release", at: url)
            let before = try Data(contentsOf: url)
            let interrupted = engine(root, fault: { if $0 == "extract:70" { throw CocoaError(.fileWriteUnknown) } })
            try rejected(.cocoa(.fileWriteUnknown)) { try interrupted.run(interrupted.inspect(url)) }
            try e.abandon()
            try require(!e.notices.contains(tr("import.partialArchive")) && (try Data(contentsOf: url)) == before, "unpunched archive mislabelled")
            _ = try e.inspect(url)
        }
    }
    try test("review B literal reproduction: remove journal after interrupted folder moves") { root in
        let e = engine(root); try e.recover(); try folder(e.game)
        let before = try snapshot(e.game), source = e.pending.appendingPathComponent("folder")
        try folder(source)
        let interrupted = engine(root, fault: { if $0 == "extract:2" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try interrupted.run(interrupted.inspect(source)) }
        try fm.removeItem(at: root.appendingPathComponent(".game-import/journal.json"))
        try e.recover(); try e.abandon(); _ = try e.discover()
        try require(try snapshot(e.game) == before, "journal loss damaged old game")
        try e.deleteGame()
        try require(!fm.fileExists(atPath: e.game.path) && !e.unfinished, "maintenance remains stuck after journal loss")
    }
    try test("changed archive is preserved at commit instead of deleting new notes") { root in
        let url = root.appendingPathComponent("Import/input.zip")
        let e = engine(root, fault: { if $0 == "committed" { try putZip("notes", at: url) } })
        try e.recover(); try putZip("stored", at: url)
        try e.run(e.inspect(url))
        try require(try Data(contentsOf: url) == Data(contentsOf: fixtures.appendingPathComponent("notes.zip")), "changed archive deleted or altered")
        try require(e.notices.contains(tr("import.sourceChanged")) && (try e.receipt()) != nil, "changed-source result unclear")
    }
    try test("unfinished source is readable from the checkpoint alone") { root in
        let e = engine(root); try e.recover()
        try require(e.unfinishedSource == nil, "no transaction yet")
        let url = e.pending.appendingPathComponent("input.zip"); try putZip("stored", at: url)
        let broken = engine(root, fault: { if $0 == "extract:2" { throw CocoaError(.fileWriteUnknown) } })
        try rejected(.cocoa(.fileWriteUnknown)) { try broken.run(broken.inspect(url)) }
        try require(e.unfinishedSource.map(BotWZipReader.normalizedPath) == BotWZipReader.normalizedPath(url), "unfinished source")
    }
    try test("delete game clears a preserved backup conflict and unblocks import") { root in
        let e = engine(root); try e.recover(); try folder(e.game)
        let backup = root.appendingPathComponent(".game-import/old"); try folder(backup)
        _ = try e.discover(); try require(e.recoveryNeedsAttention, "conflict state")
        try e.deleteGame()
        try require(!e.recoveryNeedsAttention && !fm.fileExists(atPath: e.game.path) && !fm.fileExists(atPath: backup.path), "delete left the conflict")
        try putZip("stored", at: e.pending.appendingPathComponent("input.zip"))
        try e.run(try e.discover().first!)
        try require(try e.receipt() != nil, "import after conflict")
    }
    try test("release record is dropped once a punched archive is consumed") { root in
        let record = root.appendingPathComponent(".game-import-released.json")
        var recorded = false
        let e = engine(root, release: true, fault: { if $0 == "released" && fm.fileExists(atPath: record.path) { recorded = true } }); try e.recover()
        let url = e.pending.appendingPathComponent("input.zip"); try putZip("release", at: url)
        try e.run(e.inspect(url))
        try require(recorded, "archive was never punched, so the record path was not exercised")
        try require(!fm.fileExists(atPath: url.path) && !fm.fileExists(atPath: record.path), "stale release record")
    }
    print("ALL OK: \(passed) scenarios; every scenario preserved Documents/save and sibling Application Support/SheikahVision/save byte-for-byte")
} catch {
    fputs("FAIL after \(passed): \(error)\n", stderr)
    exit(1)
}
