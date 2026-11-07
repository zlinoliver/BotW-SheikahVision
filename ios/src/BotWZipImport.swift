// Streaming zip import for the game files. visionOS has no public unzip API, so this is a small reader of its own:
// the central directory (with ZIP64, the archive is 12-15 GB), stored and deflate entries, and zlib's raw inflate in
// fixed-size chunks. Nothing is held in memory beyond the central directory and two 1 MB buffers.
import Foundation
import CoreFoundation
import zlib

struct BotWZipError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

/// Set from the main thread (Cancel button), polled by the extraction thread
final class BotWCancelToken {
    private let lock = NSLock()
    private var flag = false
    var cancelled: Bool { lock.lock(); defer { lock.unlock() }; return flag }
    func cancel() { lock.lock(); flag = true; lock.unlock() }
}

struct BotWZipEntry: Codable {
    var originalName: String = ""
    var name: String
    var method: UInt16
    var crc: UInt32
    var compressedSize: UInt64
    var size: UInt64
    var headerOffset: UInt64
    var dataOffset: UInt64?
    var isDirectory: Bool { name.hasSuffix("/") }
}

final class BotWZipReader {
    static let chunk = 1 << 20
    private let fd: Int32
    private let fileSize: UInt64
    private let verifiedEntries: [UInt64: BotWZipEntry]
    private(set) var directoryOffset: UInt64 = 0
    private(set) var warnings: [String] = []
    private(set) var entries: [BotWZipEntry] = []

    init(url: URL, verifiedEntries: [BotWZipEntry] = []) throws {
        var verified: [UInt64: BotWZipEntry] = [:]
        for entry in verifiedEntries {
            guard verified.updateValue(entry, forKey: entry.headerOffset) == nil else { throw BotWZipError(message: tr("files.zipBad")) }
        }
        self.verifiedEntries = verified
        try Self.rejectLinks(url)
        fd = open(url.path, O_RDONLY | O_NOFOLLOW)
        guard fd >= 0 else { throw BotWZipError(message: tr("files.zipBad")) }
        var st = stat()
        guard fstat(fd, &st) == 0, st.st_size >= 0, st.st_mode & S_IFMT == S_IFREG else {
            close(fd); throw BotWZipError(message: tr("files.zipBad"))
        }
        fileSize = UInt64(st.st_size)
        do { try readDirectory() } catch { close(fd); throw error }
    }

    deinit { close(fd) }

    // MARK: central directory

    private func read(at off: UInt64, count: Int) throws -> [UInt8] {
        guard off <= fileSize, count >= 0, UInt64(count) <= fileSize - off else { throw BotWZipError(message: tr("files.zipBad")) }
        var buf = [UInt8](repeating: 0, count: count)
        var got = 0
        while got < count {
            let n = buf.withUnsafeMutableBytes { pread(fd, $0.baseAddress! + got, count - got, off_t(off) + off_t(got)) }
            if n <= 0 { throw BotWZipError(message: tr("files.zipBad")) }
            got += n
        }
        return buf
    }

    private static func u16(_ b: [UInt8], _ i: Int) -> UInt16 { UInt16(b[i]) | UInt16(b[i + 1]) << 8 }
    private static func u32(_ b: [UInt8], _ i: Int) -> UInt32 { UInt32(u16(b, i)) | UInt32(u16(b, i + 2)) << 16 }
    private static func u64(_ b: [UInt8], _ i: Int) -> UInt64 { UInt64(u32(b, i)) | UInt64(u32(b, i + 4)) << 32 }

    private func readDirectory() throws {
        let bad = BotWZipError(message: tr("files.zipBad"))
        // end of central directory: the last 22 bytes, or up to 64 KB back when there is a comment
        guard fileSize >= 22 else { throw bad }
        let tailLen = Int(min(fileSize, 22 + 65535))
        let tailStart = fileSize - UInt64(tailLen)
        let tail = try read(at: tailStart, count: tailLen)
        var e = tailLen - 22
        while e >= 0 {
            if Self.u32(tail, e) == 0x0605_4b50, e + 22 + Int(Self.u16(tail, e + 20)) == tail.count { break }
            e -= 1
        }
        guard e >= 0 else { throw bad }
        guard Self.u16(tail, e + 4) == 0, Self.u16(tail, e + 6) == 0 else { throw BotWZipError(message: tr("import.split")) }
        var cdSize = UInt64(Self.u32(tail, e + 12))
        var cdOffset = UInt64(Self.u32(tail, e + 16))
        let eocdPos = tailStart + UInt64(e)
        if cdSize == 0xFFFF_FFFF || cdOffset == 0xFFFF_FFFF || Self.u16(tail, e + 10) == 0xFFFF {
            // ZIP64: the locator sits right before the end record and points at the ZIP64 end record
            guard eocdPos >= 20 else { throw bad }
            let loc = try read(at: eocdPos - 20, count: 20)
            guard Self.u32(loc, 0) == 0x0706_4b50 else { throw bad }
            guard Self.u32(loc, 4) == 0, Self.u32(loc, 16) == 1 else { throw BotWZipError(message: tr("import.split")) }
            let z64 = try read(at: Self.u64(loc, 8), count: 56)
            guard Self.u32(z64, 0) == 0x0606_4b50 else { throw bad }
            guard Self.u32(z64, 16) == 0, Self.u32(z64, 20) == 0 else { throw BotWZipError(message: tr("import.split")) }
            guard Self.u64(z64, 32) <= 200_000 else { throw bad }
            cdSize = Self.u64(z64, 40)
            cdOffset = Self.u64(z64, 48)
        }
        guard cdOffset <= eocdPos, cdSize <= eocdPos - cdOffset, cdSize < (128 << 20) else { throw bad }
        directoryOffset = cdOffset
        let cd = try read(at: cdOffset, count: Int(cdSize))
        // walk the records by size, not by the entry count (large archives from some tools carry a wrong count)
        var p = 0
        var names = Set<String>(), folded = Set<String>()
        var total: UInt64 = 0, count = 0
        var ranges: [BotWZipEntry] = []
        while p + 46 <= cd.count, Self.u32(cd, p) == 0x0201_4b50 {
            let flags = Self.u16(cd, p + 8)
            let method = Self.u16(cd, p + 10)
            let crc = Self.u32(cd, p + 16)
            var csize = UInt64(Self.u32(cd, p + 20))
            var usize = UInt64(Self.u32(cd, p + 24))
            let nameLen = Int(Self.u16(cd, p + 28)), extraLen = Int(Self.u16(cd, p + 30)), commentLen = Int(Self.u16(cd, p + 32))
            var offset = UInt64(Self.u32(cd, p + 42))
            guard p + 46 + nameLen + extraLen + commentLen <= cd.count else { throw bad }
            let name = try Self.decodeName(Array(cd[(p + 46)..<(p + 46 + nameLen)]), flags: flags)
            guard Self.u16(cd, p + 34) == 0 else { throw BotWZipError(message: tr("import.split")) }
            guard (Self.u32(cd, p + 38) >> 16) & 0o170000 != 0o120000 else { throw BotWZipError(message: tr("files.zipUnsafe", name)) }
            // ZIP64 extra field (id 1): the 64-bit values for exactly the fields that read 0xFFFFFFFF, in this order
            let needsSize = usize == 0xFFFF_FFFF, needsCompressed = csize == 0xFFFF_FFFF, needsOffset = offset == 0xFFFF_FFFF
            var foundZIP64 = false
            var x = p + 46 + nameLen
            let xEnd = x + extraLen
            while x + 4 <= xEnd {
                let id = Self.u16(cd, x), len = Int(Self.u16(cd, x + 2))
                guard x + 4 + len <= xEnd else { throw bad }
                if id == 1 {
                    guard !foundZIP64 else { throw bad }
                    foundZIP64 = true
                    var q = x + 4
                    func next() throws -> UInt64 {
                        guard q + 8 <= x + 4 + len else { throw bad }
                        defer { q += 8 }
                        return Self.u64(cd, q)
                    }
                    if needsSize { usize = try next() }
                    if needsCompressed { csize = try next() }
                    if needsOffset { offset = try next() }
                }
                x += 4 + len
            }
            guard x == xEnd, foundZIP64 || !(needsSize || needsCompressed || needsOffset) else { throw bad }
            p += 46 + nameLen + extraLen + commentLen
            count += 1
            guard count <= 200_000, usize <= (4 << 30), usize <= (40 << 30) - total,
                  offset < cdOffset, csize <= cdOffset - offset else { throw bad }
            total += usize
            try Self.checkSafe(name)
            guard names.insert(name).inserted else { throw BotWZipError(message: tr("import.duplicate", name)) }
            if method != 0 && method != 8 { throw BotWZipError(message: tr("files.zipMethod", name, String(method))) }
            if flags & 0x41 != 0 { throw BotWZipError(message: tr("files.zipEncrypted", name)) }
            let entry = BotWZipEntry(originalName: name, name: name, method: method, crc: crc, compressedSize: csize, size: usize, headerOffset: offset)
            ranges.append(entry)
            if !folded.insert(name.lowercased()).inserted {
                warnings.append(tr("import.case", name)); continue
            }
            entries.append(entry)
        }
        guard p == cd.count, !entries.isEmpty else { throw bad }
        // Validate all local headers before any archive bytes can be punched. Overlapping entries could otherwise
        // free a later entry's data (or a DLC entry) even though each central record looks individually valid.
        var end: UInt64 = 0
        var offsets: [UInt64: UInt64] = [:]
        for var entry in ranges.sorted(by: { $0.headerOffset < $1.headerOffset }) {
            if let verified = verifiedEntries[entry.headerOffset] {
                // Only durable, completed entries may have punched local headers. Their immutable plan pins the
                // original name, sizes, CRC and data start; all untouched entries still get a local-header check.
                guard verified.originalName == entry.name, verified.method == entry.method,
                      verified.crc == entry.crc, verified.compressedSize == entry.compressedSize,
                      verified.size == entry.size, verified.dataOffset != nil else { throw bad }
                entry.dataOffset = verified.dataOffset
            }
            let span = try dataRange(entry)
            guard entry.headerOffset >= end else { throw BotWZipError(message: tr("import.overlap")) }
            offsets[entry.headerOffset] = span.lowerBound
            end = span.upperBound
        }
        for index in entries.indices { entries[index].dataOffset = offsets[entries[index].headerOffset] }
    }

    // MARK: game root

    /// Entries that are junk from the Finder
    static func isJunk(_ name: String) -> Bool {
        name.split(separator: "/").contains { $0 == "__MACOSX" || $0.hasPrefix(".") }
    }

    /// Rejects names that would leave the destination folder
    static func checkSafe(_ name: String) throws {
        let path = name.hasSuffix("/") ? String(name.dropLast()) : name
        let parts = path.split(separator: "/", omittingEmptySubsequences: false)
        if path.isEmpty || name.hasPrefix("/") || name.contains("\\") || name.contains(":") || name.contains("\0") ||
            parts.contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }) {
            throw BotWZipError(message: tr("files.zipUnsafe", name))
        }
    }

    /// The folder prefix (with trailing "/", possibly empty) that holds code/U-King.rpx, the shallowest if several.
    func gameRoot() -> String? {
        var best: String?
        for e in entries where !e.isDirectory && !Self.isJunk(e.name) {
            let n = e.name
            if n == "code/U-King.rpx" { return "" }
            if n.hasSuffix("/code/U-King.rpx") {
                let prefix = String(n.dropLast("code/U-King.rpx".count))
                if best == nil || prefix.split(separator: "/").count < best!.split(separator: "/").count { best = prefix }
            }
        }
        return best
    }

    /// The files to extract for a root: code/, content/ and meta/ below it, with U-King.rpx last so an interrupted
    /// extraction never looks like a game folder
    func gameFiles(root: String) throws -> [BotWZipEntry] {
        var files: [BotWZipEntry] = []
        for e in entries where e.name.hasPrefix(root) && !Self.isJunk(e.name) {
            let rel = String(e.name.dropFirst(root.count))
            guard rel.hasPrefix("code/") || rel.hasPrefix("content/") || rel.hasPrefix("meta/") else { continue }
            try Self.checkSafe(e.name)
            if e.isDirectory { continue }
            guard e.method == 0 || e.method == 8 else { throw BotWZipError(message: tr("files.zipMethod", e.name, String(e.method))) }
            var f = e
            f.name = rel
            files.append(f)
        }
        if let i = files.firstIndex(where: { $0.name == "code/U-King.rpx" }) { files.append(files.remove(at: i)) }
        return files
    }

    // MARK: extraction

    /// Writes one entry to dest. `advance` gets the uncompressed bytes written since its last call.
    func extract(_ e: BotWZipEntry, to dest: URL, token: BotWCancelToken, advance: (Int) -> Void) throws {
        let bad = BotWZipError(message: tr("files.zipCorrupt", e.name))
        let fm = FileManager.default
        try Self.rejectLinks(dest)
        try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Self.rejectLinks(dest)
        let out = open(dest.path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0o644)
        guard out >= 0 else { throw BotWZipError(message: String(cString: strerror(errno))) }
        defer { close(out) }

        // data starts after the local header, whose name/extra lengths can differ from the central directory's
        let span = try dataRange(e)
        // Finder's >4 GB archives can have 32-bit descriptors alongside ZIP64 offsets.
        // Never read descriptors: sizes and CRC always come from the central directory.
        var pos = span.lowerBound
        var remaining = e.compressedSize

        let inBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: Self.chunk)
        let outBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: Self.chunk)
        defer { inBuf.deallocate(); outBuf.deallocate() }
        var crc = crc32(0, nil, 0)
        var total: UInt64 = 0

        func readChunk() throws -> Int {
            let n = Int(min(UInt64(Self.chunk), remaining))
            var got = 0
            while got < n {
                let r = pread(fd, inBuf + got, n - got, off_t(pos) + off_t(got))
                if r <= 0 { throw bad }
                got += r
            }
            pos += UInt64(n)
            remaining -= UInt64(n)
            return n
        }
        func write(_ p: UnsafePointer<UInt8>, _ n: Int) throws {
            guard UInt64(n) <= e.size - total else { throw bad }
            var done = 0
            while done < n {
                let w = Darwin.write(out, p + done, n - done)
                if w <= 0 { throw BotWZipError(message: String(cString: strerror(errno))) }
                done += w
            }
            crc = crc32(crc, p, UInt32(n))
            total += UInt64(n)
            advance(n)
        }

        if e.method == 0 {
            guard e.compressedSize == e.size else { throw bad }
            while remaining > 0 {
                if token.cancelled { throw CancellationError() }
                let n = try readChunk()
                try write(inBuf, n)
            }
        } else {
            var strm = z_stream()
            guard inflateInit2_(&strm, -15, zlibVersion(), Int32(MemoryLayout<z_stream>.size)) == Z_OK else { throw bad }
            defer { inflateEnd(&strm) }
            var ended = false
            while !ended {
                if token.cancelled { throw CancellationError() }
                if strm.avail_in == 0 {
                    if remaining == 0 { throw bad }  // input ran out before the end of the stream
                    let n = try readChunk()
                    strm.next_in = inBuf
                    strm.avail_in = UInt32(n)
                }
                // drain this input chunk completely (inflate may need several output buffers per chunk)
                repeat {
                    strm.next_out = outBuf
                    strm.avail_out = UInt32(Self.chunk)
                    let r = inflate(&strm, Z_NO_FLUSH)
                    guard r == Z_OK || r == Z_STREAM_END || r == Z_BUF_ERROR else { throw bad }
                    let produced = Self.chunk - Int(strm.avail_out)
                    if produced > 0 { try write(outBuf, produced) }
                    if r == Z_STREAM_END { ended = true; break }
                    if r == Z_BUF_ERROR && produced == 0 { throw bad }
                } while strm.avail_out == 0
            }
            guard strm.avail_in == 0 else { throw bad }
        }
        guard total == e.size, crc == e.crc, remaining == 0 else { throw bad }
        guard fsync(out) == 0 else { throw bad }
    }
}


// MARK: - paths and sparse archives

extension BotWZipReader {
    static func rejectLinks(_ url: URL) throws {
        var current = url.absoluteURL
        while current.path != "/" {
            var st = stat()
            if lstat(current.path, &st) == 0, st.st_mode & S_IFMT == S_IFLNK {
                // Apple's document/provider URLs may start with /var although the physical root is /private/var.
                // Allow only these OS-owned aliases, with their exact system targets; every app/source link is rejected.
                let target = try? FileManager.default.destinationOfSymbolicLink(atPath: current.path)
                guard ["/var", "/tmp"].contains(current.path), ["/private" + current.path, "private" + current.path].contains(target ?? "") else {
                    throw BotWZipError(message: tr("files.zipUnsafe", current.path))
                }
            }
            current.deleteLastPathComponent()
        }
    }

    static func normalizedPath(_ url: URL) -> String {
        var parts: [Substring] = []
        for part in url.path.split(separator: "/") {
            if part == "." { continue }
            if part == ".." { if !parts.isEmpty { parts.removeLast() } }
            else { parts.append(part) }
        }
        let path = "/" + parts.joined(separator: "/")
        for alias in ["/var", "/tmp"] where path == alias || path.hasPrefix(alias + "/") {
            let target = try? FileManager.default.destinationOfSymbolicLink(atPath: alias)
            if ["/private" + alias, "private" + alias].contains(target ?? "") { return "/private" + path }
        }
        return path
    }

    private static func decodeName(_ bytes: [UInt8], flags: UInt16) throws -> String {
        if let name = String(bytes: bytes, encoding: .utf8) { return name }
        guard flags & 0x800 == 0 else { throw BotWZipError(message: tr("files.zipBad")) }
        let encoding = CFStringConvertEncodingToNSStringEncoding(CFStringEncoding(CFStringEncodings.dosLatinUS.rawValue))
        guard let name = String(bytes: bytes, encoding: String.Encoding(rawValue: encoding)) else {
            throw BotWZipError(message: tr("files.zipBad"))
        }
        return name
    }

    private func dataRange(_ entry: BotWZipEntry) throws -> Range<UInt64> {
        let bad = BotWZipError(message: tr("files.zipCorrupt", entry.name))
        if let start = entry.dataOffset {
            guard entry.headerOffset <= start, start - entry.headerOffset >= 30, start <= directoryOffset,
                  entry.compressedSize <= directoryOffset - start else { throw bad }
            return start..<(start + entry.compressedSize)
        }
        let header = try read(at: entry.headerOffset, count: 30)
        guard Self.u32(header, 0) == 0x0403_4b50, Self.u16(header, 8) == entry.method,
              Self.u16(header, 6) & 0x41 == 0 else { throw bad }
        let name = try read(at: entry.headerOffset + 30, count: Int(Self.u16(header, 26)))
        guard try Self.decodeName(name, flags: Self.u16(header, 6)) == (entry.originalName.isEmpty ? entry.name : entry.originalName) else { throw bad }
        let start = entry.headerOffset + 30 + UInt64(Self.u16(header, 26)) + UInt64(Self.u16(header, 28))
        guard start <= directoryOffset, entry.compressedSize <= directoryOffset - start else { throw bad }
        return start..<(start + entry.compressedSize)
    }

    func releasableBytes(_ entry: BotWZipEntry) throws -> UInt64 {
        let span = try dataRange(entry)
        let start = (entry.headerOffset + 4095) / 4096 * 4096
        let end = span.upperBound / 4096 * 4096
        return end > start ? end - start : 0
    }

    func release(_ entry: BotWZipEntry, fd: Int32) throws {
        let span = try dataRange(entry)
        guard BotWZipRelease.range(fd: fd, offset: entry.headerOffset, length: span.upperBound - entry.headerOffset) else {
            throw BotWZipError(message: tr("import.releaseFailed"))
        }
    }

}

enum BotWZipRelease {
    static func range(fd: Int32, offset: UInt64, length: UInt64) -> Bool {
        let start = (offset + 4095) / 4096 * 4096, end = (offset + length) / 4096 * 4096
        guard end > start else { return true }
        var hole = fpunchhole_t(fp_flags: 0, reserved: 0, fp_offset: off_t(start), fp_length: off_t(end - start))
        return fcntl(fd, F_PUNCHHOLE, &hole) == 0
    }

    static func probe(in folder: URL) -> Bool {
        let url = folder.appendingPathComponent(".release-" + UUID().uuidString)
        let fd = open(url.path, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0o600)
        guard fd >= 0 else { return false }
        defer { close(fd); try? FileManager.default.removeItem(at: url) }
        let data = [UInt8](repeating: 0xA5, count: 1 << 20)
        for _ in 0..<8 { if data.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress!, $0.count) }) != data.count { return false } }
        fsync(fd)
        var before = stat(), after = stat()
        guard fstat(fd, &before) == 0, range(fd: fd, offset: 2 << 20, length: 4 << 20) else { return false }
        fsync(fd)
        return fstat(fd, &after) == 0 && before.st_blocks - after.st_blocks >= (3 << 20) / 512
    }
}
