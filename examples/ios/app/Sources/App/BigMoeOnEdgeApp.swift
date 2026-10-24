import SwiftUI
import UIKit

/// The app: five screens over the one engine, the five every UI of this project has (chat, models,
/// plan, settings, metrics), each a client of bmoe-server, which runs inside this app on loopback
/// (docs/application-framework.md). The server starts with the app; the model it holds is loaded
/// from Models.
@main
struct BigMoeOnEdgeApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var engine: Engine
    @StateObject private var chat: ChatModel
    @StateObject private var nav = Navigator()

    init() {
        let engine = Engine()
        _engine = StateObject(wrappedValue: engine)
        _chat = StateObject(wrappedValue: ChatModel(engine: engine))
        AppDelegate.engine = engine
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(engine)
                .environmentObject(engine.preferences)
                .environmentObject(chat)
                .environmentObject(nav)
                .task { engine.start() }
        }
    }
}

/// What only the application delegate is told: the system woke the app for the download session.
final class AppDelegate: NSObject, UIApplicationDelegate {
    @MainActor static var engine: Engine?

    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void)
    {
        Task { @MainActor in
            guard let engine = Self.engine else { return completionHandler() }
            engine.handleBackgroundDownloads(completionHandler)
        }
    }
}
