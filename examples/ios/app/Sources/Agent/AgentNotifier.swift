import BmoeKit
import Foundation
import UIKit
import UserNotifications

/// A local notification when the agent needs the person or is done, while they are in another app:
/// an approval is waiting, or a run finished. iOS suspends this app (and the server inside it)
/// soon after it leaves the screen, so a notification can only come from the seconds the system
/// grants a backgrounded app; a run that outlasts them pauses and finishes when the app is opened
/// again. Nothing is scheduled ahead and nothing comes from a server elsewhere.
@MainActor
final class AgentNotifier {
    private var asked = false

    /// Asks for permission the first time the agent is used, not at launch: the question makes
    /// sense once there is a run to be told about.
    func askOnce() {
        guard !asked else { return }
        asked = true
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    func stateChanged(_ c: ConversationSummary, from before: String?) {
        guard UIApplication.shared.applicationState != .active, let before, before != c.state else { return }
        if c.state == "waiting" {
            post(id: "approval-\(c.id)", title: "Approval needed", body: "\(c.displayTitle): the agent asks before it goes on.")
        } else if before == "running" || before == "waiting" {
            if c.state == "idle" {
                post(id: "done-\(c.id)", title: "Run finished", body: c.displayTitle)
            } else if c.state == "error" {
                post(id: "done-\(c.id)", title: "Run stopped with an error", body: c.displayTitle)
            }
        }
    }

    private func post(id: String, title: String, body: String) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        // The same id replaces an earlier notice of the same conversation instead of piling up.
        UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: id, content: content, trigger: nil))
    }
}
