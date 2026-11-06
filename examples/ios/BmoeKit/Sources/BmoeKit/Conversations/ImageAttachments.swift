import Foundation

/// The images a message carries (docs/server-api.md, "Images"): the rules every composer applies
/// before a picked image is attached, and the data URL the server takes, as the web UI's composer
/// has them. Pure, so the rules are tested without a screen; the engine decodes the bytes as they
/// are and the projector downsizes, so nothing here resizes or re-encodes.
public enum ImageAttachments {
    /// At most this many images in one message, the server's own limit.
    public static let maxImages = 8
    /// The size of one image's bytes the composer refuses above, before any base64.
    public static let maxBytes = 16 * 1024 * 1024
    /// What an image is sent as when its type is not known.
    public static let fallbackMIME = "image/jpeg"

    /// Why a picked file of `mime` and `bytes` does not go in beside `count` already attached, or
    /// nil when it does. The wording is the web composer's.
    public static func accept(mime: String?, bytes: Int, count: Int) -> String? {
        guard let mime, mime.lowercased().hasPrefix("image/") else { return "Only images can be attached" }
        if bytes > maxBytes { return "An image can be at most \(maxBytes / (1024 * 1024)) MB" }
        if count >= maxImages { return "At most \(maxImages) images go in one message" }
        return nil
    }

    /// The data URL the API carries an image as: the MIME type (the fallback when there is none)
    /// and the bytes in base64, on one line.
    public static func dataURL(mime: String?, data: Data) -> String {
        let type = (mime?.isEmpty == false ? mime! : fallbackMIME).lowercased()
        return "data:\(type);base64," + data.base64EncodedString()
    }

    /// A data URL back to its type and bytes, for a thumbnail of what a message carries; nil when
    /// it is not one.
    public static func decode(dataURL s: String) -> (mime: String, data: Data)? {
        guard s.hasPrefix("data:"), let comma = s.firstIndex(of: ",") else { return nil }
        let header = s[s.index(s.startIndex, offsetBy: 5)..<comma]
        let parts = header.split(separator: ";", omittingEmptySubsequences: false).map(String.init)
        guard parts.contains("base64"), let data = Data(base64Encoded: String(s[s.index(after: comma)...])) else { return nil }
        let mime = parts.first ?? ""
        return (mime.isEmpty ? fallbackMIME : mime, data)
    }
}
