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
    /// partial download, so a check cut off by the app's end is found again and offered to resume.
    public static func partURL(_ name: String, in dir: URL) -> URL { dir.appendingPathComponent(name + ".part") }

    /// A file that came whole and was not checked yet: the next start checks it instead of fetching it again.
    static func uncheckedPart(_ f: CatalogFile, in dir: URL) -> Bool {
        f.bytes > 0 && size(of: partURL(f.name, in: dir)) == f.bytes
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
    private var checking: Set<String> = [] // file names read back against their digest
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
    public func start(_ entry: CatalogEntry) {
        let todo = DownloadPlan.remaining(entry.files, in: modelsDir)
        guard !todo.isEmpty else { return }
        lock.lock()
        entries[entry.id] = entry.files
        failures[entry.id] = nil
        cancelling.remove(entry.id)
        // In flight from now: the first progress report can take a while to come.
        for file in todo where received[file.name] == nil && !checking.contains(file.name) { received[file.name] = 0 }
        lock.unlock()
        for file in todo {
            guard let url = URL(string: file.url), url.scheme == "https" else {
                fail(entry.id, "the catalog gives no https URL for \(file.name)")
                return
            }
            if isChecking(file.name) { continue }
            // Came whole before and was never checked (the app ended during the check): checked
            // now, not fetched again.
            if DownloadPlan.uncheckedPart(file, in: modelsDir) {
                lock.lock()
                received[file.name] = nil
                lock.unlock()
                check(entry: entry.id, file: file, part: DownloadPlan.partURL(file.name, in: modelsDir))
                continue
            }
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
        publish()
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
        entries[entryID]?.contains { received[$0.name] != nil || checking.contains($0.name) } ?? false
    }

    private func isChecking(_ file: String) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return checking.contains(file)
    }

    /// Reads a whole file back against its digest, then renames it into place; a mismatch deletes
    /// it and the entry reports why. The row reads Checking meanwhile, and a cancel stops the check
    /// with the file kept, so the next start checks it again rather than fetching it.
    private func check(entry entryID: String, file: CatalogFile, part: URL) {
        lock.lock()
        checking.insert(file.name)
        lock.unlock()
        publish()
        let dest = modelsDir.appendingPathComponent(file.name)
        checkQueue.async {
            do {
                try Checksum.land(part, at: dest, file: file.name, sha256: file.sha256) { [weak self] in
                    guard let self else { return true }
                    self.lock.lock()
                    defer { self.lock.unlock() }
                    return self.cancelling.contains(entryID)
                }
            } catch is CancellationError {
                // Kept: checked on the next start.
            } catch {
                self.fail(entryID, error.localizedDescription)
            }
            self.lock.lock()
            self.checking.remove(file.name)
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
                } else if checking.contains(f.name) || DownloadPlan.isWhole(f, in: modelsDir) {
                    got += f.bytes
                }
            }
            let checks = files.contains { checking.contains($0.name) }
            if let why = failures[id] {
                out[id] = DownloadProgress(entryID: id, received: got, total: total, state: .failed(why))
            } else if hasTasks(id) {
                out[id] = DownloadProgress(entryID: id, received: got, total: total, state: .running, checking: checks)
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
        // moves under its partial name, and becomes the model only once checked.
        let file = tag.files.first { $0.name == tag.file } ?? CatalogFile(name: tag.file, bytes: 0, url: "")
        let part = DownloadPlan.partURL(tag.file, in: modelsDir)
        do {
            try FileManager.default.createDirectory(at: modelsDir, withIntermediateDirectories: true)
            if FileManager.default.fileExists(atPath: part.path) { try FileManager.default.removeItem(at: part) }
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
