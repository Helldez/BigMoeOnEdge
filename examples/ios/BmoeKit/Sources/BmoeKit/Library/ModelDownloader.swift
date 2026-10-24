import Foundation

/// One catalog entry's transfer, summed over its files.
public struct DownloadProgress: Equatable, Sendable {
    public enum State: Equatable, Sendable {
        case running
        case failed(String)
    }

    public let entryID: String
    public let received: Int64
    public let total: Int64
    public let state: State
    /// A file of it has come whole and is being read back against its catalog digest.
    public let checking: Bool

    public init(entryID: String, received: Int64, total: Int64, state: State, checking: Bool = false) {
        self.entryID = entryID
        self.received = received
        self.total = total
        self.state = state
        self.checking = checking
    }

    public var fraction: Double { total > 0 ? min(1, Double(received) / Double(total)) : 0 }
}

/// Which files of an entry still have to come: a file is there when it has its exact size, so a
/// shard cut short by a crash is fetched again rather than handed to the engine. A file whose size
/// the catalog does not know (0: a link whose host said none) is there once it is there at all.
public enum DownloadPlan {
    public static func remaining(_ files: [CatalogFile], in dir: URL) -> [CatalogFile] {
        files.filter { !isWhole($0, in: dir) }
    }

    static func isWhole(_ f: CatalogFile, in dir: URL) -> Bool {
        guard let got = size(of: dir.appendingPathComponent(f.name)) else { return false }
        return f.bytes > 0 ? got == f.bytes : got > 0
    }

    /// Where a file waits, whole, for its digest to be checked: the name the server reports as a
    /// partial download (and deletes with Delete partial), so a check that did not finish is found
    /// again and offered to resume.
    public static func partURL(_ name: String, in dir: URL) -> URL { dir.appendingPathComponent(name + ".part") }

    /// The name one check reads: its own, so a second copy of the same file finishing meanwhile can
    /// never replace the bytes under a check, and only the file that was hashed is renamed into place.
    static func checkURL(_ name: String, in dir: URL) -> URL {
        dir.appendingPathComponent("\(name).part-\(UUID().uuidString)")
    }

    /// Whole files of `f` not checked yet: its partial name, and any check's own name a check cut
    /// off by the app's end left behind. Only a whole file is ever moved to these names (the system
    /// keeps a transfer's bytes elsewhere until it ends), so any of them with bytes in it is whole.
    static func uncheckedParts(_ f: CatalogFile, in dir: URL) -> [URL] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        return names.filter { $0 == f.name + ".part" || $0.hasPrefix(f.name + ".part-") }.sorted()
            .map { dir.appendingPathComponent($0) }
            .filter { url in size(of: url).map { f.bytes > 0 ? $0 == f.bytes : $0 > 0 } ?? false }
    }

    /// A file that came whole and was not checked yet: the next start checks it instead of fetching it again.
    static func uncheckedPart(_ f: CatalogFile, in dir: URL) -> Bool { !uncheckedParts(f, in: dir).isEmpty }

    /// The bytes a start still has to bring for these files: their whole size, less what is here
    /// whole and what stopped transfers kept. 0 for files whose size nobody said.
    public static func bytesNeeded(_ files: [CatalogFile], in dir: URL, resumeDir: URL) -> Int64 {
        let total = files.reduce(Int64(0)) { $0 + max(0, $1.bytes) }
        let here = partial(files, in: dir, resumeDir: resumeDir)
            ?? files.filter { isWhole($0, in: dir) }.reduce(Int64(0)) { $0 + max(0, $1.bytes) }
        return max(0, total - here)
    }

    /// Why a start is refused for want of space, worded as the server words it, or nil when it fits.
    public static func spaceRefusal(needed: Int64, free: Int64?) -> String? {
        guard let free, needed > free else { return nil }
        return "needs \(ByteText.gb(needed)) more, only \(ByteText.gb(free)) free"
    }

    static func size(of url: URL) -> Int64? {
        (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber)?.int64Value
    }

    /// What a stopped download of these files already has on the device: the files that landed
    /// whole, plus what the resume data of the others says came. Nil when no file has resume data:
    /// nothing is there to resume, and the row offers Download.
    public static func partial(_ files: [CatalogFile], in dir: URL, resumeDir: URL) -> Int64? {
        var got: Int64 = 0
        var resumable = false
        for f in files {
            if isWhole(f, in: dir) {
                got += f.bytes
            } else if uncheckedPart(f, in: dir) {
                resumable = true
                got += f.bytes
            } else if let data = try? Data(contentsOf: resumeURL(f.name, in: resumeDir)) {
                resumable = true
                got += min(f.bytes, ResumeData.bytesReceived(data) ?? 0)
            }
        }
        return resumable ? got : nil
    }

    static func resumeURL(_ name: String, in resumeDir: URL) -> URL { resumeDir.appendingPathComponent(name + ".resume") }
}

/// What the system's resume data says about a broken-off transfer. Its format is the system's (a
/// property list), read here only for the bytes it holds and the file it holds them in; a key it
/// no longer carries reads as unknown, never as an error.
public enum ResumeData {
    public static func bytesReceived(_ data: Data) -> Int64? {
        (plist(data)?["NSURLSessionResumeBytesReceived"] as? NSNumber)?.int64Value
    }

    /// Where the system keeps the bytes that came, among `candidates` (directories the temporary
    /// file is looked for in when the data names it without its directory).
    static func tempFile(_ data: Data, candidates: [URL]) -> URL? {
        guard let p = plist(data) else { return nil }
        if let path = p["NSURLSessionResumeInfoLocalPath"] as? String, FileManager.default.fileExists(atPath: path) {
            return URL(fileURLWithPath: path)
        }
        guard let name = p["NSURLSessionResumeInfoTempFileName"] as? String, !name.contains("/") else { return nil }
        return candidates.map { $0.appendingPathComponent(name) }.first { FileManager.default.fileExists(atPath: $0.path) }
    }

    private static func plist(_ data: Data) -> [String: Any]? {
        (try? PropertyListSerialization.propertyList(from: data, format: nil)) as? [String: Any]
    }
}

/// Downloads catalog entries into the models directory with the system's background transfer
/// service, the iOS counterpart of the Android app's worker: a transfer goes on while the app is
/// suspended or after the system ended it, and one that broke off resumes from where it stopped.
/// The server lists what lands (`GET /api/models`); it is never asked to download, since it has
/// no `curl` here.
///
/// Every task carries its entry in its description, so a relaunched app rebuilds what is in flight
/// from the system's own list of tasks and nothing has to be kept beside it.
public final class ModelDownloader: NSObject, @unchecked Sendable {
    public static let sessionIdentifier = "io.bigmoeonedge.app.downloads"

    /// Every entry in flight, after each change. Called on an arbitrary queue.
    public var onChange: (([String: DownloadProgress]) -> Void)?
    /// A file landed in the models directory: the server's list is out of date.
    public var onLanded: (() -> Void)?
    /// The system woke the app for this session's events; called once they are all delivered.
    public var backgroundEventsFinished: (() -> Void)?

    private let modelsDir: URL
    private let resumeDir: URL
    private let identifier: String
    private let lock = NSLock()
    private var entries: [String: [CatalogFile]] = [:]
    private var received: [String: Int64] = [:] // per file name, for the files in flight
    private var failures: [String: String] = [:] // per entry
    private var cancelling: Set<String> = []
    private var checks: [String: Int] = [:] // per file name, the checks reading a copy of it back
    // Off the session's delegate queue: reading tens of gigabytes back must not hold up the
    // progress of the other shards still coming.
    private let checkQueue = DispatchQueue(label: "io.bigmoeonedge.app.downloads.check", qos: .utility)
    private lazy var session: URLSession = {
        let config = URLSessionConfiguration.background(withIdentifier: identifier)
        // A model is gigabytes: the system picks the moment, on Wi-Fi and power when it can.
        config.sessionSendsLaunchEvents = true
        config.isDiscretionary = false
        config.allowsCellularAccess = true
        return URLSession(configuration: config, delegate: self, delegateQueue: nil)
    }()

    public init(modelsDir: URL, resumeDir: URL, identifier: String = ModelDownloader.sessionIdentifier) {
        self.modelsDir = modelsDir
        self.resumeDir = resumeDir
        self.identifier = identifier
        super.init()
    }

    /// The transfers the system holds for this app, from before a relaunch. Call once at start.
    public func reattach() {
        session.getAllTasks { tasks in
            self.lock.lock()
            for task in tasks {
                guard let tag = Tag(task.taskDescription) else { continue }
                self.entries[tag.entry] = tag.files
                self.received[tag.file] = task.countOfBytesReceived
            }
            self.lock.unlock()
            self.publish()
        }
    }

    /// Starts, or resumes, every file of the entry that is not on the device yet.
    /// Why the entry cannot start here, for want of space, or nil when what it still needs fits in
    /// what the models folder's volume has free (as the server refuses with 507 before a byte moves).
    public func spaceRefusal(_ entry: CatalogEntry) -> String? {
        let needed = DownloadPlan.bytesNeeded(entry.files, in: modelsDir, resumeDir: resumeDir)
        let values = try? modelsDir.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return DownloadPlan.spaceRefusal(needed: needed, free: values?.volumeAvailableCapacityForImportantUsage)
    }

    /// A file already in flight (a transfer the system holds, or a check) is left to it: a second
    /// transfer of the same file would race the first to the same place.
    public func start(_ entry: CatalogEntry) {
        let todo = DownloadPlan.remaining(entry.files, in: modelsDir)
        guard !todo.isEmpty else { return }
        if let bad = todo.first(where: { URL(string: $0.url)?.scheme != "https" }) {
            fail(entry.id, "the catalog gives no https URL for \(bad.name)")
            return
        }
        lock.lock()
        entries[entry.id] = entry.files
        failures[entry.id] = nil
        cancelling.remove(entry.id)
        lock.unlock()
        // The system's own list of tasks, not only this object's: after a relaunch the transfers
        // it still holds may not have been reattached yet.
        session.getAllTasks { tasks in
            let live = Set(tasks.filter { $0.state == .running || $0.state == .suspended }
                .compactMap { Tag($0.taskDescription)?.file })
            self.lock.lock()
            let fresh = todo.filter { !live.contains($0.name) && self.received[$0.name] == nil && !self.checkingFile($0.name) }
            // In flight from now: the first progress report can take a while to come.
            for file in fresh { self.received[file.name] = 0 }
            self.lock.unlock()
            for file in fresh { self.begin(entry, file) }
            self.publish()
        }
    }

    private func begin(_ entry: CatalogEntry, _ file: CatalogFile) {
        // Came whole before and was never checked (the app ended during the check): checked now,
        // not fetched again.
        if let part = DownloadPlan.uncheckedParts(file, in: modelsDir).first {
            lock.lock()
            received[file.name] = nil
            lock.unlock()
            check(entry: entry.id, file: file, part: part)
            return
        }
        guard let url = URL(string: file.url) else { return }
        let resume = DownloadPlan.resumeURL(file.name, in: resumeDir)
        let task: URLSessionDownloadTask
        if let data = try? Data(contentsOf: resume) {
            try? FileManager.default.removeItem(at: resume)
            task = session.downloadTask(withResumeData: data)
        } else {
            task = session.downloadTask(with: url)
        }
        task.taskDescription = Tag(entry: entry.id, file: file.name, files: entry.files).text
        task.resume()
    }

    /// Cancels the entry's transfers. What came so far is kept, so a later start resumes it and
    /// the row offers Resume; throwing it away is its own action, `discard`.
    public func cancel(_ entryID: String) {
        lock.lock()
        cancelling.insert(entryID)
        lock.unlock()
        session.getAllTasks { tasks in
            for task in tasks {
                guard let tag = Tag(task.taskDescription), tag.entry == entryID else { continue }
                if let d = task as? URLSessionDownloadTask {
                    d.cancel(byProducingResumeData: { data in
                        if let data { self.keep(data, for: tag.file) }
                        self.onLanded?()
                    })
                } else {
                    task.cancel()
                }
            }
        }
    }

    /// What a stopped download of the entry already has on the device, or nil when nothing of it
    /// can be resumed (`DownloadPlan.partial`).
    public func partial(_ entry: CatalogEntry) -> Int64? {
        DownloadPlan.partial(entry.files, in: modelsDir, resumeDir: resumeDir)
    }

    /// Throws away what stopped downloads of the entry kept: the resume data and, where it can be
    /// found, the bytes the system holds for it. Files that landed whole stay.
    public func discard(_ entry: CatalogEntry) {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        // Where the system has kept a background transfer's bytes: the app's temporary folder, or
        // the transfer daemon's folder in the app's caches.
        var candidates = [FileManager.default.temporaryDirectory]
        if let bundle = Bundle.main.bundleIdentifier {
            candidates.append(caches.appendingPathComponent("com.apple.nsurlsessiond/Downloads/\(bundle)"))
        }
        for f in entry.files {
            // A whole copy a check did not finish, under a check's own name the server does not know.
            for part in DownloadPlan.uncheckedParts(f, in: modelsDir) where part.lastPathComponent != f.name + ".part" {
                try? FileManager.default.removeItem(at: part)
            }
            let resume = DownloadPlan.resumeURL(f.name, in: resumeDir)
            guard let data = try? Data(contentsOf: resume) else { continue }
            if let tmp = ResumeData.tempFile(data, candidates: candidates) { try? FileManager.default.removeItem(at: tmp) }
            try? FileManager.default.removeItem(at: resume)
        }
        dismiss(entry.id)
    }

    /// Forgets a failed entry's error, once the person has read it.
    public func dismiss(_ entryID: String) {
        lock.lock()
        failures[entryID] = nil
        if !hasTasks(entryID) { entries[entryID] = nil }
        lock.unlock()
        publish()
    }

    // MARK: state

    private func hasTasks(_ entryID: String) -> Bool {
        entries[entryID]?.contains { received[$0.name] != nil || checkingFile($0.name) } ?? false
    }

    // Under the lock.
    private func checkingFile(_ name: String) -> Bool { (checks[name] ?? 0) > 0 }

    /// Reads a whole file back against its digest, then renames it into place; a mismatch deletes
    /// it and the entry reports why. The file is first moved to a name of this check's own, so the
    /// bytes hashed are the bytes renamed whatever else lands meanwhile. The row reads Checking,
    /// and a cancel stops the check with the file kept under its partial name, so the next start
    /// checks it again rather than fetching it.
    private func check(entry entryID: String, file: CatalogFile, part: URL) {
        let own = DownloadPlan.checkURL(file.name, in: modelsDir)
        do {
            try FileManager.default.moveItem(at: part, to: own)
        } catch {
            fail(entryID, "\(file.name): \(error.localizedDescription)")
            return
        }
        lock.lock()
        checks[file.name, default: 0] += 1
        lock.unlock()
        publish()
        let dest = modelsDir.appendingPathComponent(file.name)
        checkQueue.async {
            do {
                try Checksum.land(own, at: dest, file: file.name, sha256: file.sha256) { [weak self] in
                    guard let self else { return true }
                    self.lock.lock()
                    defer { self.lock.unlock() }
                    return self.cancelling.contains(entryID)
                }
            } catch is CancellationError {
                // Kept for the next start, under the name the server's Delete partial removes.
                let part = DownloadPlan.partURL(file.name, in: self.modelsDir)
                try? FileManager.default.removeItem(at: part)
                try? FileManager.default.moveItem(at: own, to: part)
            } catch {
                self.fail(entryID, error.localizedDescription)
            }
            self.lock.lock()
            self.checks[file.name] = (self.checks[file.name] ?? 1) > 1 ? self.checks[file.name]! - 1 : nil
            self.lock.unlock()
            self.publish()
            self.onLanded?()
        }
    }

    private func keep(_ resumeData: Data, for file: String) {
        try? FileManager.default.createDirectory(at: resumeDir, withIntermediateDirectories: true)
        try? resumeData.write(to: DownloadPlan.resumeURL(file, in: resumeDir))
    }

    private func fail(_ entryID: String, _ why: String) {
        lock.lock()
        failures[entryID] = why
        lock.unlock()
        publish()
    }

    private func publish() {
        lock.lock()
        var out: [String: DownloadProgress] = [:]
        for (id, files) in entries {
            let total = files.reduce(Int64(0)) { $0 + $1.bytes }
            var got: Int64 = 0
            for f in files {
                if let r = received[f.name] {
                    got += r
                } else if checkingFile(f.name) || DownloadPlan.isWhole(f, in: modelsDir) {
                    got += f.bytes
                }
            }
            let inCheck = files.contains { checkingFile($0.name) }
            // Downloading while any file of it is still in flight, even after another failed: the
            // row keeps Cancel, and offers no second start of files already coming. The failure
            // is said once nothing of it runs.
            if hasTasks(id) {
                out[id] = DownloadProgress(entryID: id, received: got, total: total, state: .running, checking: inCheck)
            } else if let why = failures[id] {
                out[id] = DownloadProgress(entryID: id, received: got, total: total, state: .failed(why))
            }
        }
        // An entry with nothing in flight and nothing to report is done: it leaves the list.
        entries = entries.filter { out[$0.key] != nil }
        lock.unlock()
        onChange?(out)
    }
}

extension ModelDownloader: URLSessionDownloadDelegate {
    public func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                           totalBytesWritten: Int64, totalBytesExpectedToWrite _: Int64)
    {
        guard let tag = Tag(downloadTask.taskDescription) else { return }
        lock.lock()
        received[tag.file] = totalBytesWritten
        lock.unlock()
        publish()
    }

    public func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let tag = Tag(downloadTask.taskDescription) else { return }
        let status = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(status) else {
            fail(tag.entry, "\(tag.file): the server answered HTTP \(status)")
            return
        }
        // The file has to move before this returns: the system deletes `location` right after. It
        // moves under a name of its own (never one another copy could be checked under), and
        // becomes the model only once checked.
        let file = tag.files.first { $0.name == tag.file } ?? CatalogFile(name: tag.file, bytes: 0, url: "")
        let part = DownloadPlan.checkURL(tag.file, in: modelsDir)
        do {
            try FileManager.default.createDirectory(at: modelsDir, withIntermediateDirectories: true)
            try FileManager.default.moveItem(at: location, to: part)
        } catch {
            fail(tag.entry, "\(tag.file): \(error.localizedDescription)")
            return
        }
        check(entry: tag.entry, file: file, part: part)
    }

    public func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let tag = Tag(task.taskDescription) else { return }
        lock.lock()
        received[tag.file] = nil
        let cancelled = cancelling.contains(tag.entry)
        lock.unlock()
        if let error = error as NSError? {
            // Whatever came is kept for the next start, whether the person stopped it or the network did.
            if let data = error.userInfo[NSURLSessionDownloadTaskResumeData] as? Data { keep(data, for: tag.file) }
            if !cancelled { fail(tag.entry, "\(tag.file): \(error.localizedDescription)") }
        }
        publish()
        onLanded?()
    }

    public func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        let done = backgroundEventsFinished
        backgroundEventsFinished = nil
        DispatchQueue.main.async { done?() }
    }
}

/// What a task carries about itself: its entry, its file, and every file of the entry.
private struct Tag: Codable {
    let entry: String
    let file: String
    let files: [CatalogFile]

    init(entry: String, file: String, files: [CatalogFile]) {
        self.entry = entry
        self.file = file
        self.files = files
    }

    init?(_ text: String?) {
        guard let text, let t = try? JSONDecoder().decode(Tag.self, from: Data(text.utf8)) else { return nil }
        self = t
    }

    var text: String { String(decoding: (try? JSONEncoder().encode(self)) ?? Data(), as: UTF8.self) }
}
