import Foundation

/// Where everything of the app lives on the device. The server compiles no path in: the app hands
/// it these at launch (docs/application-framework.md).
struct AppPaths {
    /// Models, metrics files and the agent's workspaces: the folder the Files app and the Finder
    /// show for this app, so a model can be copied in, a CSV copied out, and a workspace's files
    /// opened by other apps. Kept out of the device's backup: a model is tens of gigabytes that can
    /// be downloaded again (a workspace's files go with it; the Files app copies them out).
    let documents: URL
    /// The server's settings and its added catalog entries.
    let data: URL
    /// What the app kept of its conversations before the server held them: moved once, then empty.
    let conversations: URL
    /// The agent's folders on this phone: each one a workspace, in the folder the Files app shows.
    let workspaces: URL
    let metrics: URL
    /// What a broken-off download keeps to resume from.
    let downloads: URL
    /// The catalog the server lists, shipped inside the app.
    let catalog: URL?

    static let current: AppPaths = {
        let fm = FileManager.default
        let documents = fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("BigMoeOnEdge")
        let paths = AppPaths(
            documents: documents, data: support, conversations: support.appendingPathComponent("conversations"),
            workspaces: documents.appendingPathComponent("Workspaces"),
            metrics: documents.appendingPathComponent("Metrics"), downloads: support.appendingPathComponent("downloads"),
            catalog: Bundle.main.url(forResource: "models", withExtension: "json"))
        for dir in [paths.documents, paths.data, paths.metrics, paths.downloads, paths.workspaces] {
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        var noBackup = URLResourceValues()
        noBackup.isExcludedFromBackup = true
        var docs = documents
        try? docs.setResourceValues(noBackup)
        return paths
    }()
}
