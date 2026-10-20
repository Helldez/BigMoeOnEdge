import Foundation

/// A JSON value as the server sends it. The API carries values whose type a schema decides at run
/// time (a parameter's value is a bool, an int, a float or a string, as `GET /api/params` says), so
/// the client reads a tree and asks it, the way the other clients do, instead of a type per answer.
/// Integers and floats stay apart: an `int` parameter goes back as `8`, never `8.0`.
public enum JSON: Equatable, Sendable {
    case null
    case bool(Bool)
    case int(Int64)
    case double(Double)
    case string(String)
    case array([JSON])
    case object([String: JSON])

    public init(parsing data: Data) throws {
        self = try JSONDecoder().decode(JSON.self, from: data)
    }

    public init(parsing text: String) throws {
        try self.init(parsing: Data(text.utf8))
    }

    public func data() -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return (try? encoder.encode(self)) ?? Data("null".utf8)
    }

    public var text: String { String(decoding: data(), as: UTF8.self) }

    // MARK: reading

    public subscript(key: String) -> JSON {
        if case let .object(o) = self { return o[key] ?? .null }
        return .null
    }

    public subscript(index: Int) -> JSON {
        if case let .array(a) = self, a.indices.contains(index) { return a[index] }
        return .null
    }

    public var isNull: Bool { self == .null }

    public var string: String? {
        if case let .string(s) = self { return s }
        return nil
    }

    public var bool: Bool? {
        if case let .bool(b) = self { return b }
        return nil
    }

    public var double: Double? {
        switch self {
        case let .double(d): return d
        case let .int(i): return Double(i)
        default: return nil
        }
    }

    public var int: Int? {
        switch self {
        case let .int(i): return Int(exactly: i)
        case let .double(d): return d.isFinite ? Int(d) : nil
        default: return nil
        }
    }

    public var int64: Int64? {
        switch self {
        case let .int(i): return i
        case let .double(d): return d.isFinite ? Int64(d) : nil
        default: return nil
        }
    }

    public var array: [JSON]? {
        if case let .array(a) = self { return a }
        return nil
    }

    public var object: [String: JSON]? {
        if case let .object(o) = self { return o }
        return nil
    }

    public func string(or fallback: String) -> String { string ?? fallback }
    public func bool(or fallback: Bool) -> Bool { bool ?? fallback }
    public func double(or fallback: Double) -> Double { double ?? fallback }
    public func int(or fallback: Int) -> Int { int ?? fallback }
    public func int64(or fallback: Int64) -> Int64 { int64 ?? fallback }
    public var strings: [String] { array?.compactMap(\.string) ?? [] }

    /// How a value reads in a field or a file: numbers plain, a whole float without its `.0`.
    public var display: String {
        switch self {
        case .null: return ""
        case let .bool(b): return b ? "true" : "false"
        case let .int(i): return String(i)
        case let .double(d):
            if d.isFinite, d == d.rounded(), abs(d) < 1e15 { return String(Int64(d)) }
            return String(d)
        case let .string(s): return s
        case .array, .object: return text
        }
    }
}

extension JSON: Codable {
    public init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() {
            self = .null
        } else if let b = try? c.decode(Bool.self) {
            self = .bool(b)
        } else if let i = try? c.decode(Int64.self) {
            self = .int(i)
        } else if let d = try? c.decode(Double.self) {
            self = .double(d)
        } else if let s = try? c.decode(String.self) {
            self = .string(s)
        } else if let a = try? c.decode([JSON].self) {
            self = .array(a)
        } else {
            self = .object(try c.decode([String: JSON].self))
        }
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .null: try c.encodeNil()
        case let .bool(b): try c.encode(b)
        case let .int(i): try c.encode(i)
        case let .double(d): try c.encode(d)
        case let .string(s): try c.encode(s)
        case let .array(a): try c.encode(a)
        case let .object(o): try c.encode(o)
        }
    }
}

extension JSON: ExpressibleByStringLiteral, ExpressibleByBooleanLiteral, ExpressibleByIntegerLiteral,
    ExpressibleByFloatLiteral, ExpressibleByArrayLiteral, ExpressibleByDictionaryLiteral, ExpressibleByNilLiteral
{
    public init(stringLiteral value: String) { self = .string(value) }
    public init(booleanLiteral value: Bool) { self = .bool(value) }
    public init(integerLiteral value: Int64) { self = .int(value) }
    public init(floatLiteral value: Double) { self = .double(value) }
    public init(arrayLiteral elements: JSON...) { self = .array(elements) }
    public init(dictionaryLiteral elements: (String, JSON)...) {
        self = .object(Dictionary(elements, uniquingKeysWith: { _, last in last }))
    }
    public init(nilLiteral: ()) { self = .null }
}
