import SwiftUI
import UIKit

/// The app: five screens over the one engine, the five every UI of this project has (chat, models,
/// plan, settings, metrics), each a client of bmoe-server, which runs inside this app on loopback
/// (docs/application-framework.md). The server starts with the app; the model it holds is loaded
/// from Models; the chat is the server's agent, whose workspaces, skills, tasks and providers are
/// reached from Settings.
@main
struct BigMoeOnEdgeApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var engine: Engine
    @StateObject private var agent: AgentModel
    @StateObject private var nav = Navigator()

    init() {
        let engine = Engine()
        _engine = StateObject(wrappedValue: engine)
        _agent = StateObject(wrappedValue: AgentModel(engine: engine))
        AppDelegate.engine = engine
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(engine)
                .environmentObject(engine.preferences)
                .environmentObject(agent)
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
