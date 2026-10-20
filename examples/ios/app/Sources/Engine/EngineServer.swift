import Foundation

/// The server inside this app: bmoe-server's `run()` on a thread of its own, through the bridge.
/// It listens on loopback with a key only the app holds, since on a phone every installed app can
/// reach loopback; on the network, when the person asks, with the key they are shown.
final class EngineServer {
    /// The server's own default port (server/server.h), so an OpenAI client on the network finds a
    /// phone where it finds a desktop. The launch argument `-BmoePort N` moves it, for the simulator:
    /// it shares the Mac's loopback, where a desktop bmoe-server may hold that port already.
    static let port: Int = {
        let p = UserDefaults.standard.integer(forKey: "BmoePort")
        return (1...65535).contains(p) ? p : 8765
    }()

    struct Launch {
        let share: Bool
        let key: String
    }

    private(set) var running = false

    var baseURL: URL { URL(string: "http://127.0.0.1:\(Self.port)/")! }

    /// Starts the server. `exited` is called on the main queue once it stops, with its exit code.
    func start(_ launch: Launch, paths: AppPaths, exited: @escaping (Int32) -> Void) {
        precondition(!running, "one server per process")
        var args = [
            "bmoe-server",
            "--host", launch.share ? "0.0.0.0" : "127.0.0.1",
            "--port", String(Self.port),
            "--data-dir", paths.data.path,
            "--models-dir", paths.documents.path,
            // The key on the command line: inside one process there is no process list to read it from.
            "--api-key", launch.key,
            // A device probe in a child process needs a process to start; an app here has none.
            "--no-isolated-probes",
        ]
        if let catalog = paths.catalog { args += ["--catalog", catalog.path] }
        running = true
        let argv = args.map { strdup($0) }
        defer { argv.forEach { free($0) } }
        argv.map { UnsafePointer($0) }.withUnsafeBufferPointer { buf in
            bmoe_server_start(Int32(buf.count), buf.baseAddress) { [weak self] code in
                DispatchQueue.main.async {
                    self?.running = false
                    exited(code)
                }
            }
        }
    }

    /// Asks the server to stop; `exited` follows once it has.
    @discardableResult
    func stop() -> Bool { bmoe_server_stop() }
}
