import Foundation

/// A task's schedule in plain words, for the list and the editor: "Every day at 09:00".
/// English throughout like the rest of the app, so it does not depend on the device's locale.
public enum ScheduleText {
    static let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
    /// By `weekdays` number: 0 = Sunday.
    public static let dayNames = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
    /// The order a week is listed in: Monday first.
    public static let weekOrder = [1, 2, 3, 4, 5, 6, 0]

    public static func describe(_ s: TaskSchedule) -> String {
        switch s.kind {
        case .once:
            // `YYYY-MM-DDTHH:MM`: read as written, the server's local time and the phone's.
            let parts = s.at.split(separator: "T")
            let date = parts.first.map { $0.split(separator: "-").compactMap { Int($0) } } ?? []
            let time = parts.count > 1 ? clock(String(parts[1])) : ""
            guard date.count == 3, (1...12).contains(date[1]) else { return "Once" }
            return "Once, \(date[2]) \(months[date[1] - 1])" + (time.isEmpty ? "" : " at \(time)")
        case .interval:
            let m = s.everyMinutes
            if m <= 0 { return "Every few minutes" }
            if m == 1 { return "Every minute" }
            if m == 60 { return "Every hour" }
            if m % 60 == 0 { return "Every \(m / 60) hours" }
            return "Every \(m) minutes"
        case .daily:
            return "Every day" + at(s.at)
        case .weekly:
            let days = Set(s.weekdays.filter { (0...6).contains($0) })
            if days.isEmpty { return "Every week" + at(s.at) }
            if days.count == 7 { return "Every day" + at(s.at) }
            if days == [1, 2, 3, 4, 5] { return "Every weekday" + at(s.at) }
            if days == [0, 6] { return "Every weekend" + at(s.at) }
            return "Every " + weekOrder.filter(days.contains).map { dayNames[$0] }.joined(separator: ", ") + at(s.at)
        }
    }

    private static func at(_ text: String) -> String {
        let c = clock(text)
        return c.isEmpty ? "" : " at \(c)"
    }

    /// `H:MM` or `HH:MM[:SS]` as `HH:MM`; anything else as nothing.
    static func clock(_ text: String) -> String {
        let p = text.split(separator: ":").compactMap { Int($0) }
        guard p.count >= 2, (0...23).contains(p[0]), (0...59).contains(p[1]) else { return "" }
        return String(format: "%02d:%02d", p[0], p[1])
    }
}
