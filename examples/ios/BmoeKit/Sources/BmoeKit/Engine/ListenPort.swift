import Foundation

/// Where the app's server listens (server/listen_port.h). The server takes the port the person chose,
/// or its own default, and when that one is in use the next free one; then it writes the port it took
/// to a file the app names. The app never assumes a port: it speaks, key and all, only to the port its
/// own server wrote, so a port another program holds is never sent the key nor shown as the engine's.
public enum ListenPort {
    /// The ports a person may choose. Below 1024 an app may not listen; 0 leaves the server's default.
    public static let min = 1024
    public static let max = 65535

    /// The port in a port file's text: digits and a newline, as the server writes it. Nil for anything
    /// else, a file caught half written included (no newline yet).
    public static func parse(_ text: String) -> Int? {
        guard text.hasSuffix("\n") else { return nil }
        let digits = text.dropLast()
        guard !digits.isEmpty, digits.count <= 5, digits.allSatisfy({ ("0"..."9").contains($0) }),
              let port = Int(digits), (1...max).contains(port) else { return nil }
        return port
    }

    /// The port in the file at `url`; nil when there is no such file yet or it does not hold one.
    public static func read(_ url: URL) -> Int? {
        guard let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .utf8) else { return nil }
        return parse(text)
    }

    /// A port as a person typed it: blank is the server's default (0), a number must be one an app may
    /// listen on. Nil when it is neither, so the field says so instead of saving it.
    public static func chosen(_ text: String) -> Int? {
        let t = text.trimmingCharacters(in: .whitespaces)
        if t.isEmpty { return 0 }
        guard let p = Int(t), (min...max).contains(p) else { return nil }
        return p
    }
}
