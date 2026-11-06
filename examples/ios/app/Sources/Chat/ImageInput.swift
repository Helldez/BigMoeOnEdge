import BmoeKit
import ImageIO
import PhotosUI
import SwiftUI
import UniformTypeIdentifiers

/// An image attached to the message being written: the data URL the server takes, as the file
/// was picked (the engine decodes it and the projector downsizes).
struct PendingImage: Identifiable, Equatable {
    let id = UUID()
    let dataURL: String
}

/// What a picked photo turns into: its bytes with its type's MIME, or why it is refused. The
/// photo library needs no permission for this: the picker runs out of process and hands over only
/// what the person chose.
enum PickedImage {
    /// The bytes as the library holds them and the MIME of the first image type the item offers
    /// (its own format, before any conversion the system could do), or nil when it loads nothing.
    static func load(_ item: PhotosPickerItem) async -> (mime: String?, data: Data)? {
        guard let data = try? await item.loadTransferable(type: Data.self) else { return nil }
        let type = item.supportedContentTypes.first { $0.conforms(to: .image) }
        return (type?.preferredMIMEType, data)
    }

    /// The composer's rules on a picked file, then its data URL; the reason when it is refused.
    static func attach(mime: String?, data: Data, to images: inout [PendingImage]) -> String? {
        if let why = ImageAttachments.accept(mime: mime, bytes: data.count, count: images.count) { return why }
        images.append(PendingImage(dataURL: ImageAttachments.dataURL(mime: mime, data: data)))
        return nil
    }
}

/// The camera, through the system's picker: a photo taken is handed over as one JPEG (the camera
/// gives a decoded image, not a file, so this is the one encode on the way to the server).
struct CameraPicker: UIViewControllerRepresentable {
    static var available: Bool { UIImagePickerController.isSourceTypeAvailable(.camera) }
    /// The one encode's quality: high, since the projector downsizes the photo to its own few
    /// hundred pixels a side and loses nothing to the file's size, so this is a platform fact of
    /// the camera's decoded image, not a knob of the model.
    static let jpegQuality: CGFloat = 0.92
    let onPhoto: (Data) -> Void
    @Environment(\.dismiss) private var dismiss

    func makeUIViewController(context: Context) -> UIImagePickerController {
        let picker = UIImagePickerController()
        picker.sourceType = .camera
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIImagePickerController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    final class Coordinator: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
        let parent: CameraPicker
        init(_ parent: CameraPicker) { self.parent = parent }

        func imagePickerController(_ picker: UIImagePickerController, didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]) {
            if let image = info[.originalImage] as? UIImage, let data = image.jpegData(compressionQuality: CameraPicker.jpegQuality) {
                parent.onPhoto(data)
            }
            parent.dismiss()
        }

        func imagePickerControllerDidCancel(_ picker: UIImagePickerController) { parent.dismiss() }
    }
}

/// A small picture of an image a message carries, decoded from its data URL off the main thread
/// and downsampled on the way in (an attached photo can be 16 MB; the thumbnail is a few pixels).
struct DataURLThumbnail: View {
    let dataURL: String
    let side: CGFloat
    @State private var image: UIImage?

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: Grid.radius, style: .continuous).fill(Palette.surface2)
            if let image {
                Image(uiImage: image).resizable().scaledToFill()
            } else {
                Image(systemName: "photo").foregroundStyle(Palette.muted)
            }
        }
        .frame(width: side, height: side)
        .clipShape(RoundedRectangle(cornerRadius: Grid.radius, style: .continuous))
        .task(id: dataURL) {
            let url = dataURL
            let pixels = side * UIScreen.main.scale
            image = await Task.detached(priority: .utility) { Self.thumbnail(url, maxPixels: pixels) }.value
        }
    }

    nonisolated private static func thumbnail(_ dataURL: String, maxPixels: CGFloat) -> UIImage? {
        guard let decoded = ImageAttachments.decode(dataURL: dataURL),
              let source = CGImageSourceCreateWithData(decoded.data as CFData, nil)
        else { return nil }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: Int(maxPixels),
        ]
        guard let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
        return UIImage(cgImage: cg)
    }
}

/// Views in a row that wraps onto the next line when they do not fit, leading-aligned.
struct WrapRow: Layout {
    var spacing: CGFloat = Grid.s

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? .infinity
        return place(in: width, subviews: subviews).size
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let frames = place(in: bounds.width, subviews: subviews).frames
        for (view, frame) in zip(subviews, frames) {
            view.place(at: CGPoint(x: bounds.minX + frame.minX, y: bounds.minY + frame.minY), proposal: ProposedViewSize(frame.size))
        }
    }

    private func place(in width: CGFloat, subviews: Subviews) -> (frames: [CGRect], size: CGSize) {
        var frames: [CGRect] = []
        var x: CGFloat = 0, y: CGFloat = 0, rowHeight: CGFloat = 0, widest: CGFloat = 0
        for view in subviews {
            let size = view.sizeThatFits(.unspecified)
            if x > 0, x + size.width > width {
                x = 0
                y += rowHeight + spacing
                rowHeight = 0
            }
            frames.append(CGRect(origin: CGPoint(x: x, y: y), size: size))
            x += size.width + spacing
            rowHeight = max(rowHeight, size.height)
            widest = max(widest, x - spacing)
        }
        return (frames, CGSize(width: widest, height: y + rowHeight))
    }
}
