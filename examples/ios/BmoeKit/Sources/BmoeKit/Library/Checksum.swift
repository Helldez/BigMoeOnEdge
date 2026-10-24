import CryptoKit
import Foundation

/// The check a finished download passes before it becomes a model (docs/server-api.md, "Models"):
/// where its catalog row names a `sha256`, the file is read back and its digest compared, as the
/// server and the Android app do. A file is tens of gigabytes, so it is read in chunks and never
/// held whole.
public enum Checksum {
    /// What is read at a time: large enough that the digest, not the reads, sets the pace.
    public static let chunkBytes = 4 << 20

    /// The file's SHA-256 as 64 lowercase hex digits. Throws when it cannot be read, or with
    /// `CancellationError` once `cancelled` says so between two chunks.
    public static func sha256Hex(of url: URL, chunkBytes: Int = Checksum.chunkBytes,
                                 cancelled: () -> Bool = { false }) throws -> String
    {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var hasher = SHA256()
        while true {
            if cancelled() { throw CancellationError() }
            // Each chunk's buffer goes before the next is read, so memory stays at one chunk.
            let more: Bool = try autoreleasepool {
                guard let data = try handle.read(upToCount: chunkBytes), !data.isEmpty else { return false }
                hasher.update(data: data)
                return true
            }
            if !more { break }
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    /// A downloaded file whose digest is not the catalog's.
    public struct Mismatch: LocalizedError, Equatable {
        public let file: String
        public let actual: String
        public let expected: String

        public var errorDescription: String? {
            "\(file): its SHA-256 is \(actual.prefix(12))..., the catalog names \(expected.prefix(12))...: it is not "
                + "the file the catalog lists, and the download is deleted"
        }
    }

    /// Puts a finished download in place: `part` is checked against `sha256` when there is one and
    /// then renamed to `dest`, replacing what was there. A digest that differs deletes `part`, since
    /// downloading it again from where it stopped could only bring back the same bytes, and throws
    /// `Mismatch`; `dest` is then left as it was.
    public static func land(_ part: URL, at dest: URL, file: String, sha256: String,
                            cancelled: () -> Bool = { false }) throws
    {
        let fm = FileManager.default
        if !sha256.isEmpty {
            let actual = try sha256Hex(of: part, cancelled: cancelled)
            guard actual == sha256 else {
                try? fm.removeItem(at: part)
                throw Mismatch(file: file, actual: actual, expected: sha256)
            }
        }
        if fm.fileExists(atPath: dest.path) { try fm.removeItem(at: dest) }
        try fm.moveItem(at: part, to: dest)
    }
}
