// One import transaction for picker and Import-folder sources. Only game, its receipt and our work directory
// are ever replaced. A durable checkpoint precedes releasing zip blocks; the last checkpointed file stays readable.
import Foundation
import CryptoKit

struct BotWImportFile: Codable {
    var path: String
    var size: UInt64
    var zip: BotWZipEntry?
}

struct BotWImportSource: Codable, Identifiable {
    var url: URL
    var prefix: String
    var files: [BotWImportFile]
    var titleID: String
    var version: Int
    var languages: [String]
    var hasDLC: Bool
    var archive: Bool
    var hasUnplannedEntries: Bool? = nil
    var id: String { url.path }
    var bytes: UInt64 { files.reduce(0) { $0 + $1.size } }
    var region: String { BotWGameImport.region(titleID) ?? "" }
}

struct BotWGameReceipt: Codable {
    struct Fingerprint: Codable, Equatable {
        var size: UInt64
        var inode: UInt64
        var modified: Date
        static func read(_ url: URL) throws -> Self {
            try BotWZipReader.rejectLinks(url)
            return try attributes(url)
        }
        static func read(_ url: URL, checkedParents: inout Set<String>) throws -> Self {
            let parent = url.deletingLastPathComponent()
            if !checkedParents.contains(parent.path) {
                try BotWZipReader.rejectLinks(parent)
                checkedParents.insert(parent.path)
            }
            return try attributes(url)
        }
        private static func attributes(_ url: URL) throws -> Self {
            var st = stat()
            guard lstat(url.path, &st) == 0, st.st_size >= 0,
                  st.st_mode & S_IFMT == S_IFREG || st.st_mode & S_IFMT == S_IFDIR else {
                throw BotWZipError(message: tr("files.zipUnsafe", url.path))
            }
            return Self(size: UInt64(st.st_size), inode: UInt64(st.st_ino),
                        modified: Date(timeIntervalSince1970: Double(st.st_mtimespec.tv_sec) + Double(st.st_mtimespec.tv_nsec) / 1_000_000_000))
        }
    }
    var titleID: String
    var titleVersion: Int
    var rpxSHA256: String
    var languages: [String]
    var fileCount: Int
    var totalBytes: UInt64
    var directory: Fingerprint
    var checkedAt: Date
    var files: [String: Fingerprint]
    var region: String { BotWGameImport.region(titleID) ?? "" }
}

final class BotWGameImport {
    static let rpxSHA256 = "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30"
    static let languages = ["JP": ["JPja"], "US": ["USen", "USfr", "USes"],
                            "EU": ["EUen", "EUde", "EUfr", "EUes", "EUit", "EUnl", "EUru"]]
    static func region(_ id: String) -> String? {
        ["00050000101C9300": "JP", "00050000101C9400": "US", "00050000101C9500": "EU"][id.uppercased()]
    }
    static func defaultLanguage(region: String, available: [String], preferred: [String] = Locale.preferredLanguages) -> String {
        let codes = region == "JP" ? ["ja"] : region == "US" ? ["en"] : preferred.map { String($0.prefix(2)) }
        for code in codes { if let match = available.first(where: { $0.hasSuffix(code) }) { return match } }
        return available.first(where: { $0.hasSuffix("en") }) ?? available.first ?? ""
    }
    struct Progress {
        var stage: String
        var done: UInt64
        var total: UInt64
        var bytesPerSecond: Double
        var secondsLeft: Double
    }
    private struct Journal: Codable {
        var phase: String
        var source: BotWImportSource
        var completed: Int
        var receipt: BotWGameReceipt?
        var sourceInode: UInt64
        var sourceSize: UInt64
        var releaseUnavailable: Bool?
        var hasReleasedData: Bool?
    }
    struct Manifest: Codable {
        struct File: Codable { var path: String; var size: UInt64; var sha256: String }
        var titleID: String
        var files: [File]
    }
    let root: URL
    var pending: URL { root.appendingPathComponent("Import") }
    var game: URL { root.appendingPathComponent("game") }
    private var work: URL { root.appendingPathComponent(".game-import") }
    private var staging: URL { work.appendingPathComponent("staging") }
    private var old: URL { work.appendingPathComponent("old") }
    private var journalURL: URL { work.appendingPathComponent("journal.json") }
    private var planURL: URL { work.appendingPathComponent("source.json") }
    private var releasedURL: URL { root.appendingPathComponent(".game-import-released.json") }
    private var receiptURL: URL { root.appendingPathComponent("game-receipt.json") }
    private let fm = FileManager.default
    private let freeSpace: () throws -> Int64
    private let fault: (String) throws -> Void
    private let expectedRPX: String
    private let manifestFolder: URL?
    private let releaseProbe: ((URL) -> Bool)?
    private let releaseWindowBytes: UInt64
    private let knownDLC: [String: Set<String>]
    private(set) var notices: [String] = []
    private(set) var releaseSupported = false
    private(set) var lastNeeded: UInt64 = 0
    private(set) var lastFree: Int64 = 0
    private(set) var statisticsRuns = 0
    private(set) var selectionFailed = false
    private(set) var recoveryNeedsAttention = false

    init(root: URL, manifestFolder: URL? = Bundle.main.resourceURL?.appendingPathComponent("game-manifest"),
         freeSpace: (() throws -> Int64)? = nil, fault: @escaping (String) throws -> Void = { _ in },
         expectedRPX: String = BotWGameImport.rpxSHA256, releaseProbe: ((URL) -> Bool)? = nil,
         knownDLC: [String: Set<String>] = [:], releaseWindowBytes: UInt64 = 256 << 20) {
        self.root = URL(fileURLWithPath: BotWZipReader.normalizedPath(root))
        self.manifestFolder = manifestFolder
        self.fault = fault
        self.expectedRPX = expectedRPX
        self.releaseProbe = releaseProbe
        self.knownDLC = knownDLC
        self.releaseWindowBytes = max(1 << 20, releaseWindowBytes)
        self.freeSpace = freeSpace ?? {
            var volume = URL(fileURLWithPath: root.path)
            volume.removeAllCachedResourceValues()
            guard let bytes = try volume.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
                .volumeAvailableCapacityForImportantUsage else { throw BotWZipError(message: tr("import.spaceUnknown")) }
            return bytes
        }
    }

    private func directory(_ url: URL) throws {
        try BotWZipReader.rejectLinks(url)
        try fm.createDirectory(at: url, withIntermediateDirectories: true)
    }
    private func remove(_ url: URL) throws {
        try BotWZipReader.rejectLinks(url)
        if fm.fileExists(atPath: url.path) { try fm.removeItem(at: url) }
    }
    private func move(_ from: URL, _ to: URL) throws {
        try BotWZipReader.rejectLinks(from); try BotWZipReader.rejectLinks(to)
        try fm.moveItem(at: from, to: to)
        try syncDirectory(from.deletingLastPathComponent()); try syncDirectory(to.deletingLastPathComponent())
    }
    private func syncDirectory(_ url: URL) throws {
        let fd = open(url.path, O_RDONLY | O_NOFOLLOW)
        guard fd >= 0 else { throw BotWZipError(message: tr("import.sync")) }
        defer { close(fd) }
        guard fsync(fd) == 0 else { throw BotWZipError(message: tr("import.sync")) }
    }
    private func write<T: Encodable>(_ value: T, to url: URL) throws {
        try BotWZipReader.rejectLinks(url)
        try JSONEncoder().encode(value).write(to: url, options: .atomic)
        let fd = open(url.path, O_RDONLY | O_NOFOLLOW)
        guard fd >= 0 else { throw BotWZipError(message: tr("import.sync")) }
        defer { close(fd) }
        guard fsync(fd) == 0 else { throw BotWZipError(message: tr("import.sync")) }
        try syncDirectory(url.deletingLastPathComponent())
    }
    private func journal() throws -> Journal? {
        try BotWZipReader.rejectLinks(journalURL)
        guard fm.fileExists(atPath: journalURL.path) else { return nil }
        try BotWZipReader.rejectLinks(planURL)
        let decoded: (Journal, BotWImportSource)
        do {
            decoded = (try JSONDecoder().decode(Journal.self, from: Data(contentsOf: journalURL)),
                       try JSONDecoder().decode(BotWImportSource.self, from: Data(contentsOf: planURL)))
        } catch { throw BotWZipError(message: tr("import.journalBad")) }
        var j = decoded.0
        let source = decoded.1
        guard source.url == j.source.url else { throw BotWZipError(message: tr("import.journalBad")) }
        j.source = source
        guard ["extracting", "committing", "committed"].contains(j.phase), j.completed >= 0,
              j.completed <= j.source.files.count else { throw BotWZipError(message: tr("import.journalBad")) }
        guard Self.region(source.titleID) != nil, source.files.count <= 200_000,
              source.files.allSatisfy({ $0.size <= 4 << 30 }),
              source.bytes <= 40 << 30 else { throw BotWZipError(message: tr("import.journalBad")) }
        var compressed: UInt64 = 0
        for f in j.source.files {
            if let entry = f.zip {
                guard source.archive, entry.name == f.path, entry.size == f.size, entry.headerOffset < j.sourceSize,
                      entry.compressedSize <= j.sourceSize - entry.headerOffset,
                      compressed <= j.sourceSize - entry.compressedSize else { throw BotWZipError(message: tr("import.journalBad")) }
                compressed += entry.compressedSize
            } else if source.archive { throw BotWZipError(message: tr("import.journalBad")) }
            try BotWZipReader.checkSafe(f.path)
            guard f.path.hasPrefix("code/") || f.path.hasPrefix("content/") || f.path.hasPrefix("meta/") else {
                throw BotWZipError(message: tr("import.journalBad"))
            }
        }
        return j
    }
    private func saveJournal(_ journal: Journal) throws {
        // The immutable file plan is large (~37,000 paths). Do not rewrite it at each checkpoint.
        if !fm.fileExists(atPath: planURL.path) { try write(journal.source, to: planURL) }
        var checkpoint = journal
        checkpoint.source.files = []
        try write(checkpoint, to: journalURL)
    }
    var unfinished: Bool { fm.fileExists(atPath: journalURL.path) }
    /// The unfinished import's source, read from the small checkpoint file only (not the ~5 MB plan).
    var unfinishedSource: URL? {
        guard let data = try? Data(contentsOf: journalURL), let j = try? JSONDecoder().decode(Journal.self, from: data) else { return nil }
        return j.source.url
    }
    var hasPending: Bool {
        !((try? fm.contentsOfDirectory(at: pending, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])) ?? []).isEmpty
    }

    private struct ReleasedArchive: Codable {
        var url: URL
        var inode: UInt64
        var size: UInt64
    }
    private func releasedArchives() throws -> [ReleasedArchive] {
        try BotWZipReader.rejectLinks(releasedURL)
        guard fm.fileExists(atPath: releasedURL.path) else { return [] }
        return try JSONDecoder().decode([ReleasedArchive].self, from: Data(contentsOf: releasedURL))
    }
    private func rememberRelease(_ j: Journal) throws {
        guard j.source.archive, insidePending(j.source.url) else { return }
        var records = try releasedArchives().filter { $0.url != j.source.url }
        records.append(ReleasedArchive(url: j.source.url, inode: j.sourceInode, size: j.sourceSize))
        try write(records, to: releasedURL)
    }
    private func forgetRelease(_ url: URL) throws {
        let records = try releasedArchives()
        let kept = records.filter { BotWZipReader.normalizedPath($0.url) != BotWZipReader.normalizedPath(url) }
        if kept.isEmpty { try remove(releasedURL); try syncDirectory(root) }
        else if kept.count != records.count { try write(kept, to: releasedURL) }
    }
    private func rejectReleasedArchive(_ url: URL) throws {
        if let fingerprint = try? BotWGameReceipt.Fingerprint.read(url),
           try releasedArchives().contains(where: { BotWZipReader.normalizedPath($0.url) == BotWZipReader.normalizedPath(url) && $0.inode == fingerprint.inode && $0.size == fingerprint.size }) {
            throw BotWZipError(message: tr("import.partialArchive"))
        }
    }
    private func preserveOld(in folder: URL) throws -> Bool {
        let backup = folder.appendingPathComponent("old")
        try BotWZipReader.rejectLinks(backup)
        guard fm.fileExists(atPath: backup.path) else { return true }
        if !fm.fileExists(atPath: game.path) { try move(backup, game); return true }
        recoveryNeedsAttention = true
        notices.append(tr("import.oldPreserved"))
        return false
    }
    private func discardWork() throws {
        guard fm.fileExists(atPath: work.path), try preserveOld(in: work) else { return }
        let trash = root.appendingPathComponent(".game-import-trash-" + UUID().uuidString)
        try move(work, trash)
        try fault("trashRenamed")
        try remove(trash)
        try syncDirectory(root)
    }
    private func trashFolders() throws -> [URL] {
        let prefix = ".game-import-trash-"
        return try fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil).filter {
            $0.lastPathComponent.hasPrefix(prefix) && UUID(uuidString: String($0.lastPathComponent.dropFirst(prefix.count))) != nil
        }
    }
    private func clearTrash() throws {
        for url in try trashFolders() {
            try BotWZipReader.rejectLinks(url)
            if try preserveOld(in: url) { try remove(url); try syncDirectory(root) }
        }
    }

    func recover() throws {
        try directory(root); try directory(pending)
        recoveryNeedsAttention = false
        try clearTrash()
        for u in [work, staging, old, game, receiptURL] { try BotWZipReader.rejectLinks(u) }
        guard var j = try journal() else {
            try discardWork()
            return
        }
        if j.phase == "committing" {
            // If the new directory already landed, put it back in staging before restoring the old one.
            if !fm.fileExists(atPath: staging.path), fm.fileExists(atPath: game.path) { try move(game, staging) }
            if fm.fileExists(atPath: old.path) { try move(old, game) }
            j.phase = "extracting"
            try saveJournal(j)
        } else if j.phase == "committed" {
            try finish(j)
            return
        }
        // Moved folders may now be empty, so existence of the original RPX is not a recovery prerequisite.
        if !fm.fileExists(atPath: j.source.url.path) {
            if insidePending(j.source.url) { try discardWork() }
            else { notices.append(tr("import.externalResume")) }
        }
    }

    private func insidePending(_ url: URL) -> Bool {
        // Unlike standardizedFileURL, this stays stable after a source is deleted and rejects lexical escapes.
        BotWZipReader.normalizedPath(url).hasPrefix(BotWZipReader.normalizedPath(pending) + "/")
    }
    private func movingSource(_ source: BotWImportSource) -> Bool {
        !source.archive && (insidePending(source.url) || BotWZipReader.normalizedPath(source.url) == BotWZipReader.normalizedPath(pending))
    }

    private static func excludedContentPath(_ path: String) -> Bool {
        path.split(separator: "/").contains { $0.lowercased() == "aoc" || $0.uppercased().hasPrefix("0005000C") }
    }
    private func relatedDLC(_ fields: [String: String], baseID: String) -> Bool {
        let id = (fields["title_id"] ?? "").uppercased()
        guard id.count == 16, id.hasPrefix("0005000C"), Self.region(baseID) != nil else { return false }
        return String(id.suffix(8).prefix(6)) == String(baseID.suffix(8).prefix(6))
            || ["parent_title_id", "base_title_id"].contains(where: { fields[$0]?.uppercased() == baseID })
            || knownDLC[baseID]?.contains(id) == true
    }
    private func pathHasDLC(_ path: String, baseID: String) -> Bool {
        path.split(separator: "/").contains {
            $0.lowercased() == "aoc" || relatedDLC(["title_id": String($0)], baseID: baseID)
        }
    }

    private func folderFiles(_ picked: URL, token: BotWCancelToken? = nil) throws -> ([BotWImportFile], Bool) {
        try BotWZipReader.rejectLinks(picked)
        let folder = URL(fileURLWithPath: BotWZipReader.normalizedPath(picked))
        var files: [BotWImportFile] = [], hasDLC = false
        let meta = folder.appendingPathComponent("meta/meta.xml")
        let baseID: String
        if (try? BotWGameReceipt.Fingerprint.read(meta).size) ?? UInt64.max < 1 << 20,
           let data = try? Data(contentsOf: meta), let fields = try? Self.metaFields(data) { baseID = (fields["title_id"] ?? "").uppercased() }
        else { baseID = "" }
        guard let walker = fm.enumerator(at: folder, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles]) else {
            throw BotWZipError(message: tr("files.noRpx"))
        }
        var folded = Set<String>()
        for case let url as URL in walker {
            if token?.cancelled == true { throw CancellationError() }
            let rel = String(url.path.dropFirst(folder.path.count + 1))
            try BotWZipReader.rejectLinks(url)
            if Self.excludedContentPath(rel) {
                hasDLC = hasDLC || pathHasDLC(rel, baseID: baseID)
                walker.skipDescendants(); continue
            }
            let a = try fm.attributesOfItem(atPath: url.path)
            let nestedMeta = url.appendingPathComponent("meta/meta.xml")
            if a[.type] as? FileAttributeType == .typeDirectory,
               (try? BotWGameReceipt.Fingerprint.read(nestedMeta).size) ?? UInt64.max < 1 << 20,
               let data = try? Data(contentsOf: nestedMeta),
               let fields = try? Self.metaFields(data), fields["title_id"]?.uppercased().hasPrefix("0005000C") == true {
                hasDLC = hasDLC || relatedDLC(fields, baseID: baseID)
                walker.skipDescendants(); continue
            }
            if BotWZipReader.isJunk(rel) { walker.skipDescendants(); continue }
            try BotWZipReader.checkSafe(rel)
            guard a[.type] as? FileAttributeType == .typeRegular else { continue }
            guard rel.hasPrefix("code/") || rel.hasPrefix("content/") || rel.hasPrefix("meta/") else { continue }
            if !folded.insert(rel.lowercased()).inserted { notices.append(tr("import.case", rel)); continue }
            files.append(BotWImportFile(path: rel, size: (a[.size] as! NSNumber).uint64Value))
        }
        guard files.count <= 200_000, files.allSatisfy({ $0.size <= 4 << 30 }),
              files.reduce(UInt64(0), { $0 + $1.size }) <= 40 << 30 else { throw BotWZipError(message: tr("files.zipBad")) }
        files.sort { $0.path < $1.path }
        if let i = files.firstIndex(where: { $0.path == "code/U-King.rpx" }) { files.append(files.remove(at: i)) }
        return (files, hasDLC)
    }

    private final class MetaParser: NSObject, XMLParserDelegate {
        var fields: [String: String] = [:]
        var text = ""
        func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName: String?, attributes: [String: String]) {
            text = ""
        }
        func parser(_ parser: XMLParser, foundCharacters string: String) { text += string }
        func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName: String?) {
            if ["title_id", "title_version", "parent_title_id", "base_title_id"].contains(elementName) { fields[elementName] = text.trimmingCharacters(in: .whitespacesAndNewlines) }
        }
    }
    private static func metaFields(_ data: Data) throws -> [String: String] {
        let parser = XMLParser(data: data), delegate = MetaParser()
        parser.delegate = delegate
        parser.shouldResolveExternalEntities = false
        guard parser.parse() else { throw BotWZipError(message: tr("import.structure")) }
        return delegate.fields
    }

    private func metadata(_ data: Data, paths: [String]) throws -> (String, Int, [String]) {
        let fields = try Self.metaFields(data)
        let id = (fields["title_id"] ?? "").uppercased()
        guard let region = Self.region(id) else { throw BotWZipError(message: tr("import.title")) }
        guard fields["title_version"] == "208" else { throw BotWZipError(message: tr("import.version")) }
        let available = (Self.languages[region] ?? []).filter { paths.contains("content/Pack/Bootup_" + $0 + ".pack") }
        guard !available.isEmpty else { throw BotWZipError(message: tr("import.languagesBad")) }
        return (id, 208, available)
    }

    func inspect(_ picked: URL) throws -> BotWImportSource {
        try BotWZipReader.rejectLinks(picked)
        let url = URL(fileURLWithPath: BotWZipReader.normalizedPath(picked))
        let archive = url.pathExtension.lowercased() == "zip"
        if archive { try rejectReleasedArchive(url) }
        var files: [BotWImportFile], hasDLC: Bool, prefix = "", meta: Data
        var archiveReader: BotWZipReader?
        if archive {
            let reader: BotWZipReader
            do { reader = try BotWZipReader(url: url) }
            catch {
                let size = (try? BotWGameReceipt.Fingerprint.read(url).size) ?? 0
                if size > 1 << 20, (error as? BotWZipError)?.message == tr("files.zipBad") {
                    throw BotWZipError(message: tr("import.incomplete", Self.size(size)))
                }
                throw error
            }
            archiveReader = reader
            guard let found = reader.gameRoot() else { throw BotWZipError(message: tr("files.zipNoRpx")) }
            prefix = found
            guard let mainMeta = reader.entries.first(where: { $0.name == prefix + "meta/meta.xml" }), mainMeta.size < 1 << 20 else {
                throw BotWZipError(message: tr("import.structure"))
            }
            meta = try previewData(mainMeta, reader: reader)
            let baseID = (try Self.metaFields(meta)["title_id"] ?? "").uppercased()
            hasDLC = reader.entries.contains { pathHasDLC($0.name, baseID: baseID) }
            var dlcPrefixes: [String] = []
            for entry in reader.entries where entry.name.hasSuffix("/meta/meta.xml") && entry.name != prefix + "meta/meta.xml" && entry.size < 1 << 20 {
                try directory(work)
                let preview = work.appendingPathComponent("dlc-preview-" + UUID().uuidString)
                defer { try? remove(preview) }
                try reader.extract(entry, to: preview, token: BotWCancelToken()) { _ in }
                if let fields = try? Self.metaFields(Data(contentsOf: preview)), fields["title_id"]?.uppercased().hasPrefix("0005000C") == true {
                    hasDLC = hasDLC || relatedDLC(fields, baseID: baseID)
                    dlcPrefixes.append(String(entry.name.dropLast("meta/meta.xml".count)))
                }
            }
            files = try reader.gameFiles(root: prefix).filter { entry in
                !Self.excludedContentPath(entry.name) && !dlcPrefixes.contains(where: { (prefix + entry.name).hasPrefix($0) })
            }.map { BotWImportFile(path: $0.name, size: $0.size, zip: $0) }
            notices += reader.warnings
            for warning in reader.warnings { log("warning=\(warning)") }
        } else {
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue else {
                throw BotWZipError(message: tr(["wud", "wux"].contains(url.pathExtension.lowercased()) ? "import.image" : "import.format"))
            }
            (files, hasDLC) = try folderFiles(url)
            guard files.contains(where: { $0.path == "code/U-King.rpx" }) else { throw BotWZipError(message: tr("files.noRpx")) }
            guard files.contains(where: { $0.path == "meta/meta.xml" && $0.size < 1 << 20 }) else { throw BotWZipError(message: tr("import.structure")) }
            meta = try Data(contentsOf: url.appendingPathComponent("meta/meta.xml"))
        }
        let (id, version, available) = try metadata(meta, paths: files.map(\.path))
        var source = BotWImportSource(url: url, prefix: prefix, files: files, titleID: id, version: version, languages: available, hasDLC: hasDLC, archive: archive)
        let reader = archiveReader
        if let reader {
            let planned = Set(files.compactMap { $0.zip?.originalName })
            source.hasUnplannedEntries = !reader.warnings.isEmpty || reader.entries.contains { entry in
                guard !BotWZipReader.isJunk(entry.name) else { return false }
                return entry.isDirectory ? !planned.contains(where: { $0.hasPrefix(entry.name) }) : !planned.contains(entry.name)
            }
        }
        try preflight(source, reader: reader)
        return source
    }

    private func previewData(_ entry: BotWZipEntry, reader: BotWZipReader) throws -> Data {
        try directory(work)
        let preview = work.appendingPathComponent("preview-" + UUID().uuidString)
        defer { try? remove(preview) }
        try reader.extract(entry, to: preview, token: BotWCancelToken()) { _ in }
        return try Data(contentsOf: preview)
    }

    private func preflight(_ source: BotWImportSource, reader: BotWZipReader?, token: BotWCancelToken = BotWCancelToken(), resuming: Bool = false) throws {
        if let manifest = try manifest(source.titleID) {
            let sizes = Dictionary(uniqueKeysWithValues: source.files.map { ($0.path, $0.size) })
            for entry in manifest.files {
                guard sizes[entry.path] == entry.size else { throw BotWZipError(message: tr("files.zipCorrupt", entry.path)) }
            }
        }
        guard let rpx = source.files.first(where: { $0.path == "code/U-King.rpx" }) else { throw BotWZipError(message: tr("files.noRpx")) }
        let url: URL
        if let reader, let entry = rpx.zip {
            try directory(work)
            let preview = work.appendingPathComponent("rpx-preview-" + UUID().uuidString)
            defer { try? remove(preview) }
            try reader.extract(entry, to: preview, token: token) { _ in }
            guard try Self.sha256(preview, token: token) == expectedRPX else { throw BotWZipError(message: tr("import.rpx")) }
            return
        } else {
            let original = source.url.appendingPathComponent(rpx.path)
            url = resuming && !fm.fileExists(atPath: original.path) ? staging.appendingPathComponent(rpx.path) : original
        }
        guard try Self.sha256(url, token: token) == expectedRPX else { throw BotWZipError(message: tr("import.rpx")) }
    }

    func discover(selected: URL? = nil) throws -> [BotWImportSource] {
        notices = []
        selectionFailed = false
        try recover()
        if recoveryNeedsAttention { return [] }
        if let j = try journal() {
            if let selected, BotWZipReader.normalizedPath(selected) != BotWZipReader.normalizedPath(j.source.url) {
                selectionFailed = true; notices.append(tr("import.unfinished"))
            }
            return [j.source]
        }
        var results: [BotWImportSource] = []
        func scan(_ folder: URL, depth: Int) throws {
            try BotWZipReader.rejectLinks(folder)
            guard depth <= 3 else { return }
            if fm.fileExists(atPath: folder.appendingPathComponent("code/U-King.rpx").path) {
                do { results.append(try inspect(folder)) } catch { notices.append(error.localizedDescription) }
                return
            }
            for url in try fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey], options: [.skipsHiddenFiles]) {
                if url.lastPathComponent == "__MACOSX" { continue }
                try BotWZipReader.rejectLinks(url)
                if (try url.resourceValues(forKeys: [.isDirectoryKey])).isDirectory == true {
                    try scan(url, depth: depth + 1)
                } else if url.pathExtension.lowercased() == "zip" {
                    do { results.append(try inspect(url)) } catch { notices.append(error.localizedDescription) }
                } else if ["wud", "wux", "rar", "7z", "wua", "z01"].contains(url.pathExtension.lowercased()) {
                    notices.append(tr(["wud", "wux"].contains(url.pathExtension.lowercased()) ? "import.image" : "import.format"))
                }
            }
        }
        try scan(pending, depth: 0)
        results.sort { $0.files.count == $1.files.count ? $0.bytes > $1.bytes : $0.files.count > $1.files.count }
        if let selected {
            do {
                let chosen: BotWImportSource
                if let index = results.firstIndex(where: { BotWZipReader.normalizedPath($0.url) == BotWZipReader.normalizedPath(selected) }) {
                    chosen = results.remove(at: index)
                } else { chosen = try inspect(selected) }
                results.insert(chosen, at: 0)
            } catch {
                selectionFailed = true
                notices.append(error.localizedDescription)
            }
        }
        return results
    }

    static func size(_ bytes: UInt64) -> String { ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file) }
    static func needed(uncompressed: UInt64, compressed: UInt64, largest: UInt64, releasing: Bool, moving: Bool, releaseReserve: UInt64 = 0, expansionPeak: UInt64 = 0) -> UInt64 {
        if moving { return 1 << 30 }
        if releasing { return max(expansionPeak, uncompressed > compressed ? uncompressed - compressed : 0) + largest + max(1 << 30, releaseReserve) }
        return uncompressed + (1 << 30)
    }
    private func checkSpace(_ j: Journal) throws {
        let source = j.source
        let remaining = source.files.dropFirst(max(0, j.completed - 1))
        releaseSupported = j.releaseUnavailable != true && source.archive && insidePending(source.url) && (releaseProbe?(pending) ?? BotWZipRelease.probe(in: pending))
        // The normal game's small entries fit inside the 1 GiB margin. For unusually large entries / many tiny
        // files, also reserve the retained checkpoint entry, a batch, and the unpunchable edges of every entry.
        var balance: Int64 = 0, peak: UInt64 = 0
        for file in remaining {
            let compressed = file.zip?.compressedSize ?? 0
            guard compressed <= UInt64(Int64.max) else { throw BotWZipError(message: tr("files.zipBad")) }
            let (next, overflow) = balance.addingReportingOverflow(Int64(file.size) - Int64(compressed))
            guard !overflow else { throw BotWZipError(message: tr("files.zipBad")) }
            balance = next
            if balance > 0 { peak = max(peak, UInt64(balance)) }
        }
        let reserve = (remaining.compactMap { $0.zip?.compressedSize }.max() ?? 0) + (256 << 20)
            + remaining.reduce(UInt64(0)) { $0 + min($1.zip?.compressedSize ?? 0, 8192) }
        lastNeeded = Self.needed(uncompressed: remaining.reduce(0) { $0 + $1.size },
                                compressed: remaining.reduce(0) { $0 + ($1.zip?.compressedSize ?? 0) },
                                largest: remaining.map(\.size).max() ?? 0, releasing: releaseSupported,
                                moving: movingSource(source), releaseReserve: reserve, expansionPeak: peak)
        do { lastFree = try freeSpace() } catch { throw BotWZipError(message: tr("import.spaceUnknown")) }
        guard lastFree >= 0 else { throw BotWZipError(message: tr("import.spaceUnknown")) }
        log("plan source=\(source.url.path) bytes=\(source.bytes) releaseSupported=\(releaseSupported) needs=\(lastNeeded) free=\(lastFree)")
        guard UInt64(lastFree) >= lastNeeded else { throw BotWZipError(message: tr("files.noSpace", Self.size(lastNeeded), Self.size(UInt64(lastFree)))) }
    }

    func previewSpace(_ source: BotWImportSource) throws {
        let identity = try BotWGameReceipt.Fingerprint.read(source.url)
        let j = try journal() ?? Journal(phase: "extracting", source: source, completed: 0,
                                        sourceInode: identity.inode, sourceSize: identity.size)
        try checkSpace(j)
    }

    func run(_ source: BotWImportSource, token: BotWCancelToken = BotWCancelToken(), journalReady: () -> Void = {}, progress: (Progress) -> Void = { _ in }) throws {
        let started = Date()
        var outcome = "failure", logSource = source
        defer { log("result=\(outcome) source=\(logSource.url.path) bytes=\(logSource.bytes) elapsed=\(Date().timeIntervalSince(started)) releaseSupported=\(releaseSupported) free=\((try? freeSpace()) ?? -1)") }
        do {
            try recover()
            guard !recoveryNeedsAttention else { throw BotWZipError(message: tr("import.journalBad")) }
            let saved = try journal()
            let plan = try saved?.source ?? inspect(source.url)
            logSource = plan
            let identity = try BotWGameReceipt.Fingerprint.read(plan.url)
            var j = saved ?? Journal(phase: "extracting", source: plan, completed: 0, sourceInode: identity.inode, sourceSize: identity.size)
            guard BotWZipReader.normalizedPath(j.source.url) == BotWZipReader.normalizedPath(source.url), j.sourceInode == identity.inode,
                  !j.source.archive || j.sourceSize == identity.size else { throw BotWZipError(message: tr("import.sourceChanged")) }
            let resume = max(0, j.completed - 1)
            let verified = j.source.files.prefix(resume).compactMap(\.zip)
            let reader = j.source.archive ? try BotWZipReader(url: j.source.url, verifiedEntries: verified) : nil
            if saved != nil { try preflight(j.source, reader: reader, token: token, resuming: true) }
            try checkSpace(j)
            try directory(work); try directory(staging)
            try saveJournal(j)
            journalReady()
            try fault("journal")
            let fd = releaseSupported ? open(j.source.url.path, O_RDWR | O_NOFOLLOW) : -1
            defer { if fd >= 0 { close(fd) } }
            if releaseSupported && fd < 0 { throw BotWZipError(message: tr("import.releaseFailed")) }
            let volumeBaseline = releaseSupported ? try freeSpace() : 0
            guard volumeBaseline >= 0 else { throw BotWZipError(message: tr("import.spaceUnknown")) }
            var allocatedSinceStart: Int64 = 0, reclaimed: UInt64 = 0, releaseCheckpoints = 0
            func occupied(_ url: URL) -> Int64 {
                var st = stat()
                return lstat(url.path, &st) == 0 ? Int64(st.st_blocks) * 512 : 0
            }
            var done: UInt64 = 0, lastCheckpoint = j.completed
            var releaseQueue: [BotWZipEntry] = []
            func report(_ stage: String) {
                let speed = Double(done) / max(0.001, Date().timeIntervalSince(started))
                progress(Progress(stage: stage, done: done, total: j.source.bytes, bytesPerSecond: speed,
                                  secondsLeft: Double(j.source.bytes - min(done, j.source.bytes)) / max(1, speed)))
            }
            for (index, file) in j.source.files.enumerated() {
                if token.cancelled { throw CancellationError() }
                let dest = staging.appendingPathComponent(file.path)
                try BotWZipReader.checkSafe(file.path); try BotWZipReader.rejectLinks(dest)
                if index < resume {
                    guard (try? BotWGameReceipt.Fingerprint.read(dest).size) == file.size else {
                        throw BotWZipError(message: tr("import.stagingChanged"))
                    }
                    done += file.size; continue
                }
                try fault("extract:\(index)")
                try directory(dest.deletingLastPathComponent())
                if let entry = file.zip, let reader {
                    let part = work.appendingPathComponent("entry.part")
                    let previousBlocks = occupied(dest) + occupied(part)
                    try remove(part)
                    try reader.extract(entry, to: part, token: token) { count in done += UInt64(count); report("extracting") }
                    try remove(dest); try move(part, dest)
                    allocatedSinceStart += occupied(dest) - previousBlocks
                    releaseQueue.append(entry)
                } else {
                    let original = j.source.url.appendingPathComponent(file.path)
                    try BotWZipReader.rejectLinks(original)
                    if fm.fileExists(atPath: original.path) {
                        try remove(dest)
                        if movingSource(j.source) { try move(original, dest) }
                        else { try copy(original, to: dest, token: token) }
                    }
                    guard (try? BotWGameReceipt.Fingerprint.read(dest).size) == file.size else { throw BotWZipError(message: tr("files.zipCorrupt", file.path)) }
                    done += file.size; report("extracting")
                }
                j.completed = index + 1
                if j.completed - lastCheckpoint >= 64 || index == j.source.files.count - 1 || releaseQueue.reduce(UInt64(0), { $0 + $1.size }) >= 256 << 20 {
                    try syncDirectory(dest.deletingLastPathComponent())
                    try saveJournal(j); try fault("checkpoint")
                    // The last completed entry is always rewritten on resume. Never punch its local header or data.
                    if releaseSupported, let reader {
                        var before = stat(), after = stat()
                        guard fstat(fd, &before) == 0 else { throw BotWZipError(message: tr("import.releaseFailed")) }
                        var attempted = false
                        do {
                            try fault("release")
                            let releasing = try releaseQueue.dropLast().filter { try reader.releasableBytes($0) > 0 }
                            if j.hasReleasedData != true, !releasing.isEmpty {
                                // Persist intent before punching: a crash between punch and marker must not leave
                                // an unrecognised, partially released archive after abandonment or orphan cleanup.
                                j.hasReleasedData = true
                                try rememberRelease(j); try saveJournal(j)
                            }
                            for entry in releasing {
                                attempted = true
                                try reader.release(entry, fd: fd)
                            }
                            guard fsync(fd) == 0, fstat(fd, &after) == 0 else { throw BotWZipError(message: tr("import.releaseFailed")) }
                            if attempted && after.st_blocks >= before.st_blocks { throw BotWZipError(message: tr("import.releaseFailed")) }
                            reclaimed += UInt64(max(0, before.st_blocks - after.st_blocks)) * 512
                            releaseCheckpoints += 1
                            // st_blocks can shrink on APFS clones without freeing volume capacity. Over multiple
                            // batches, add our staged allocation back to the volume delta to estimate real reclaim.
                            let available = try freeSpace()
                            guard available >= 0 else { throw BotWZipError(message: tr("import.spaceUnknown")) }
                            let observed = max(0, Double(available) - Double(volumeBaseline) + Double(allocatedSinceStart))
                            let slack = Double(min(64 << 20, releaseWindowBytes / 4))
                            log("release checkpoints=\(releaseCheckpoints) blocks=\(reclaimed) observedVolumeGain=\(observed)")
                            if releaseCheckpoints >= 2, reclaimed >= releaseWindowBytes,
                               observed < Double(reclaimed) / 8, Double(reclaimed) - observed > slack {
                                throw BotWZipError(message: tr("import.releaseFailed"))
                            }
                        } catch {
                            j.releaseUnavailable = true
                            try saveJournal(j)
                            notices.append(tr("import.releaseFallback"))
                            try checkSpace(j)
                        }
                    }
                    try fault("released")
                    releaseQueue = releaseQueue.last.map { [$0] } ?? []
                    lastCheckpoint = j.completed
                }
            }
            report("validating")
            j.receipt = try validate(staging, token: token)
            if token.cancelled { throw CancellationError() }
            report("committing")
            j.phase = "committing"; try saveJournal(j); try fault("committing")
            if fm.fileExists(atPath: game.path) { try move(game, old) }
            try fault("old")
            try move(staging, game); try fault("installed")
            j.phase = "committed"; try saveJournal(j); try fault("committed")
            try finish(j)
            outcome = "success"
        } catch {
            if error is CancellationError { outcome = "cancelled" }
            log("error=\(error.localizedDescription)")
            throw error
        }
    }

    private func copy(_ from: URL, to: URL, token: BotWCancelToken) throws {
        let input = try FileHandle(forReadingFrom: from)
        defer { try? input.close() }
        let fd = open(to.path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0o644)
        guard fd >= 0 else { throw BotWZipError(message: tr("import.sync")) }
        defer { close(fd) }
        while let data = try input.read(upToCount: 1 << 20), !data.isEmpty {
            if token.cancelled { throw CancellationError() }
            try data.withUnsafeBytes { bytes in
                var n = 0
                while n < bytes.count {
                    let written = Darwin.write(fd, bytes.baseAddress! + n, bytes.count - n)
                    guard written > 0 else { throw BotWZipError(message: tr("import.sync")) }
                    n += written
                }
            }
        }
        guard fsync(fd) == 0 else { throw BotWZipError(message: tr("import.sync")) }
    }

    static func sha256(_ url: URL, token: BotWCancelToken? = nil) throws -> String {
        try BotWZipReader.rejectLinks(url)
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var hash = SHA256()
        while let data = try handle.read(upToCount: 1 << 20), !data.isEmpty {
            if token?.cancelled == true { throw CancellationError() }
            hash.update(data: data)
        }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
    func manifest(_ id: String) throws -> Manifest? {
        guard let url = manifestFolder?.appendingPathComponent(id + ".json"), fm.fileExists(atPath: url.path) else { return nil }
        let manifest = try JSONDecoder().decode(Manifest.self, from: Data(contentsOf: url))
        guard manifest.titleID == id else { throw BotWZipError(message: tr("import.manifestBad")) }
        for file in manifest.files { try BotWZipReader.checkSafe(file.path) }
        return manifest
    }
    func validate(_ folder: URL, deep: Bool = false, token: BotWCancelToken? = nil) throws -> BotWGameReceipt {
        statisticsRuns += 1
        let (files, _) = try folderFiles(folder, token: token)
        let paths = files.map(\.path)
        guard ["code/U-King.rpx", "code/app.xml", "code/cos.xml", "meta/meta.xml"].allSatisfy(paths.contains),
              paths.contains(where: { $0.hasPrefix("content/") }) else { throw BotWZipError(message: tr("import.structure")) }
        let meta = folder.appendingPathComponent("meta/meta.xml")
        guard (try BotWGameReceipt.Fingerprint.read(meta)).size < 1 << 20 else { throw BotWZipError(message: tr("import.structure")) }
        let (id, version, available) = try metadata(Data(contentsOf: meta), paths: paths)
        let hash = try Self.sha256(folder.appendingPathComponent("code/U-King.rpx"), token: token)
        guard hash == expectedRPX else { throw BotWZipError(message: tr("import.rpx")) }
        if let manifest = try manifest(id) {
            let byPath = Dictionary(uniqueKeysWithValues: files.map { ($0.path, $0.size) })
            for entry in manifest.files {
                if token?.cancelled == true { throw CancellationError() }
                guard byPath[entry.path] == entry.size else { throw BotWZipError(message: tr("files.zipCorrupt", entry.path)) }
                if deep, try Self.sha256(folder.appendingPathComponent(entry.path), token: token) != entry.sha256 {
                    throw BotWZipError(message: tr("files.zipCorrupt", entry.path))
                }
            }
        } else if deep { throw BotWZipError(message: tr("import.noManifest")) }
        var fingerprints: [String: BotWGameReceipt.Fingerprint] = [:]
        var checkedParents = Set<String>()
        for file in files {
            if token?.cancelled == true { throw CancellationError() }
            fingerprints[file.path] = try .read(folder.appendingPathComponent(file.path), checkedParents: &checkedParents)
        }
        return BotWGameReceipt(titleID: id, titleVersion: version, rpxSHA256: hash, languages: available,
                               fileCount: files.count, totalBytes: files.reduce(0) { $0 + $1.size },
                               directory: try .read(folder), checkedAt: Date(), files: fingerprints)
    }

    func receipt() throws -> BotWGameReceipt? {
        try BotWZipReader.rejectLinks(receiptURL)
        guard fm.fileExists(atPath: game.path) else { return nil }
        if let data = try? Data(contentsOf: receiptURL), let saved = try? JSONDecoder().decode(BotWGameReceipt.self, from: data),
           Self.region(saved.titleID) != nil, saved.titleVersion == 208, saved.rpxSHA256 == expectedRPX,
           saved.fileCount == saved.files.count, saved.fileCount > 0, saved.fileCount <= 200_000,
           saved.files.values.allSatisfy({ $0.size <= 4 << 30 }),
           saved.totalBytes == saved.files.values.reduce(UInt64(0), { $0 + $1.size }), saved.totalBytes <= 40 << 30,
           ["code/U-King.rpx", "code/app.xml", "code/cos.xml", "meta/meta.xml"].allSatisfy({ saved.files[$0] != nil }),
           !saved.languages.isEmpty, saved.languages.allSatisfy({ Self.languages[saved.region]?.contains($0) == true && saved.files["content/Pack/Bootup_" + $0 + ".pack"] != nil }),
           saved.directory == (try? .read(game)) {
            // Parent-directory mtime does not change when a child file is edited. Stat recorded paths (no enumeration
            // or hashing) as well, so an edited child's mtime invalidates the receipt on both APFS and the simulator.
            var valid = true
            var checkedParents = Set<String>()
            for (path, fingerprint) in saved.files {
                try BotWZipReader.checkSafe(path)
                if (try? BotWGameReceipt.Fingerprint.read(game.appendingPathComponent(path), checkedParents: &checkedParents)) != fingerprint { valid = false; break }
            }
            if valid { return saved }
        }
        let fresh = try validate(game)
        try write(fresh, to: receiptURL)
        return fresh
    }

    private func finish(_ j: Journal) throws {
        guard var receipt = j.receipt else { throw BotWZipError(message: tr("import.journalBad")) }
        try remove(old); try fault("backupRemoved")
        receipt.directory = try .read(game)
        try write(receipt, to: receiptURL); try fault("receipt")
        var excluded = game
        var values = URLResourceValues(); values.isExcludedFromBackup = true
        try excluded.setResourceValues(values)
        if j.source.archive, j.hasReleasedData == true, j.source.hasDLC || j.source.hasUnplannedEntries != false {
            notices.append(tr("import.partialArchive"))
        }
        if j.source.hasDLC { notices.append(tr("import.dlc")) }
        else if insidePending(j.source.url) || movingSource(j.source) {
            if let identity = try? BotWGameReceipt.Fingerprint.read(j.source.url),
               identity.inode != j.sourceInode || (j.source.archive && identity.size != j.sourceSize) {
                notices.append(tr("import.sourceChanged"))
                try discardWork()
                return
            }
            if j.source.archive {
                if j.source.hasUnplannedEntries == false { try remove(j.source.url); try forgetRelease(j.source.url) }
                else { notices.append(tr("import.archiveKept")) }
            } else {
                if BotWZipReader.normalizedPath(j.source.url) == BotWZipReader.normalizedPath(pending) {
                    for name in ["code", "content", "meta"] { try pruneEmpty(pending.appendingPathComponent(name)) }
                } else { try pruneEmpty(j.source.url) }
                if fm.fileExists(atPath: j.source.url.path) { notices.append(tr("import.sourceKept")) }
            }
            try fault("sourceRemoved")
            var parent = j.source.url.deletingLastPathComponent()
            while insidePending(parent) {
                if fm.fileExists(atPath: parent.path) {
                    guard try fm.contentsOfDirectory(atPath: parent.path).isEmpty else { break }
                    try remove(parent); try fault("parentRemoved")
                }
                parent.deleteLastPathComponent()
            }
        }
        try discardWork()
    }
    private func pruneEmpty(_ folder: URL) throws {
        guard fm.fileExists(atPath: folder.path) else { return }
        try BotWZipReader.rejectLinks(folder)
        for url in try fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil) {
            try BotWZipReader.rejectLinks(url)
            let type = try fm.attributesOfItem(atPath: url.path)[.type] as? FileAttributeType
            if type == .typeDirectory { try pruneEmpty(url) }
            else if BotWZipReader.isJunk(url.lastPathComponent) { try remove(url) }
        }
        if try fm.contentsOfDirectory(atPath: folder.path).isEmpty { try remove(folder) }
    }

    func abandon() throws {
        try recover()
        guard !recoveryNeedsAttention else { return }
        if let j = try journal(), j.source.archive, insidePending(j.source.url), j.hasReleasedData == true {
            try rememberRelease(j)
            notices.append(tr("import.partialArchive"))
        }
        // The archive stays intact outside the ranges already released. Never delete unknown files or DLC with it.
        try discardWork()
    }
    func deleteGame() throws {
        try recover()
        let conflict = recoveryNeedsAttention
        if conflict {
            // Deleting the game was confirmed, so the preserved backup goes with it; otherwise the next recover()
            // would move it back as game. This is the only way out of that state inside the app. Saves are elsewhere.
            for folder in [work] + (try trashFolders()) { try remove(folder.appendingPathComponent("old")) }
        }
        try remove(game); try remove(receiptURL)
        if conflict { try recover() }
    }
    func deepValidate() throws {
        let fresh = try validate(game, deep: true)
        try write(fresh, to: receiptURL)
    }
    private func log(_ line: String) {
        let url = root.appendingPathComponent("import-log.txt")
        do {
            try BotWZipReader.rejectLinks(url)
            var data = (try? Data(contentsOf: url)) ?? Data()
            if data.count > 256 << 10 { data = data.suffix(128 << 10) }
            data.append(Data((ISO8601DateFormatter().string(from: Date()) + " " + line.replacingOccurrences(of: "\n", with: " ") + "\n").utf8))
            try data.write(to: url, options: .atomic)
        } catch { NSLog("[import] log: %@", error.localizedDescription) }
    }
}
