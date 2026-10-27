import BmoeKit
import SwiftUI
import WebKit

/// A file the agent produced or changed, read through the server (`GET .../file?path=`): text and
/// code in the mono face, Markdown rendered, an image shown, and a web page drawn in a web view
/// that is walled off from the app.
struct FilePreviewSheet: View {
    @EnvironmentObject private var agent: AgentModel
    @Environment(\.dismiss) private var dismiss
    let path: String
    @State private var file: PreviewFile?
    @State private var failure: String?
    @State private var showSource = false

    var body: some View {
        NavigationStack {
            Group {
                if let failure {
                    Banner(text: failure, color: Palette.err).padding(Grid.m)
                    Spacer()
                } else if let file {
                    content(file)
                } else {
                    ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .background(Palette.bg)
            .navigationTitle((path as NSString).lastPathComponent)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } }
                if let file, file.kind == .html || file.kind == .markdown {
                    ToolbarItem(placement: .primaryAction) {
                        Button(showSource ? "Rendered" : "Source") { showSource.toggle() }
                    }
                }
            }
        }
        .task {
            do { file = try await agent.preview(path: path) } catch { failure = error.localizedDescription }
        }
    }

    @ViewBuilder
    private func content(_ f: PreviewFile) -> some View {
        switch showSource ? .text : f.kind {
        case .markdown:
            ScrollView { MarkdownText(source: f.text).foregroundStyle(Palette.text).padding(Grid.m) }
        case .html:
            SandboxedWebView(html: f.text).ignoresSafeArea(edges: .bottom)
        case .image:
            if let image = UIImage(data: f.data) {
                ScrollView([.horizontal, .vertical]) { Image(uiImage: image).resizable().scaledToFit().padding(Grid.m) }
            } else {
                Text("The image could not be read.").font(.ui(15)).foregroundStyle(Palette.muted).padding(Grid.m)
            }
        case .text:
            ScrollView([.vertical, .horizontal]) {
                Text(f.text).font(.mono(13)).foregroundStyle(Palette.text).textSelection(.enabled).padding(Grid.m)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        case .binary:
            VStack(alignment: .leading, spacing: Grid.s) {
                Text("No preview for this kind of file.").font(.ui(17, weight: .semibold)).foregroundStyle(Palette.text)
                Text("\(f.data.count) bytes, \(f.contentType.isEmpty ? "unknown type" : f.contentType). Open it from the workspace "
                    + "folder in the Files app.").font(.ui(14)).foregroundStyle(Palette.muted)
                Spacer()
            }
            .padding(Grid.m)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// A page the model wrote, shown as a page and nothing more. Its scripts run inside it (a chart,
/// a toggle), but there is no bridge to the app: no message handler, no script injected, no base
/// URL (so no file of the device is reachable and nothing relative loads), a non-persistent store
/// (no cookies or storage kept), every request to the network blocked by a content rule (a page
/// the model wrote cannot fetch or report anything), and every navigation away from the page
/// refused. A link the person taps goes nowhere: the preview is not a browser.
struct SandboxedWebView: UIViewRepresentable {
    let html: String

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        config.userContentController = WKUserContentController()
        config.defaultWebpagePreferences.allowsContentJavaScript = true
        config.preferences.javaScriptCanOpenWindowsAutomatically = false
        config.allowsInlineMediaPlayback = false
        config.mediaTypesRequiringUserActionForPlayback = .all
        let view = WKWebView(frame: .zero, configuration: config)
        view.navigationDelegate = context.coordinator
        view.uiDelegate = context.coordinator
        view.isOpaque = false
        let page = html
        // The page loads only once the rule that keeps it off the network is in place; a rule that
        // cannot be compiled leaves the page unloaded rather than loaded without it.
        WKContentRuleListStore.default().compileContentRuleList(
            forIdentifier: "bmoe-preview-offline", encodedContentRuleList: Self.offline)
        { list, _ in
            DispatchQueue.main.async {
                guard let list else { return }
                config.userContentController.add(list)
                view.loadHTMLString(page, baseURL: nil)
            }
        }
        return view
    }

    private static let offline = #"[{"trigger":{"url-filter":"^(https?|wss?|ftp)://"},"action":{"type":"block"}}]"#

    func updateUIView(_ view: WKWebView, context: Context) {}

    final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate {
        private var loaded = false

        func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                     decisionHandler: @escaping (WKNavigationActionPolicy) -> Void)
        {
            // The first load is the page itself (about:blank with the string); anything after it,
            // a link, a form, a script's location change, leaves the page and is refused. A frame
            // inside the page asking for a remote document is refused too.
            let isPage = !loaded && action.targetFrame?.isMainFrame == true && action.request.url?.scheme == "about"
            decisionHandler(isPage ? .allow : .cancel)
            if isPage { loaded = true }
        }

        // `window.open` and target=_blank ask for a new view: none is given.
        func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                     for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView?
        {
            nil
        }
    }
}
