import BmoeKit
import SwiftUI

/// Which screen is open, and the setting a link points at: the app's `#/settings/<key>`. The Plan
/// screen's decisions, the context warning and Apply's list link to a row of Settings this way.
@MainActor
final class Navigator: ObservableObject {
    enum Screen: String, CaseIterable, Identifiable {
        case chat = "Chat", models = "Models", plan = "Plan", settings = "Settings", metrics = "Metrics"
        var id: String { rawValue }

        var icon: String {
            switch self {
            case .chat: return "bubble.left.and.bubble.right"
            case .models: return "shippingbox"
            case .plan: return "cpu"
            case .settings: return "slider.horizontal.3"
            case .metrics: return "chart.xyaxis.line"
            }
        }
    }

    @Published var screen = Screen.chat
    /// The parameter Settings is to land on, once; nil when no link is pending.
    @Published var settingTarget: String?
    /// Whether the first screen was chosen yet: a first visit with nothing on the device opens on
    /// Models instead of an empty Chat, and only the first.
    var startChosen = false

    func open(_ s: Screen) { screen = s }

    /// Settings, landed on the row of `key` (a key the server named).
    func openSetting(_ key: String) {
        settingTarget = key
        screen = .settings
    }
}

/// The five screens in a bottom bar, the status line above every one of them.
struct RootView: View {
    typealias Screen = Navigator.Screen
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var engine: Engine

    init() {
        Self.styleBars()
    }

    var body: some View {
        TabView(selection: $nav.screen) {
            ForEach(Screen.allCases) { s in
                NavigationStack {
                    VStack(spacing: 0) {
                        StatusLine()
                        content(s)
                    }
                    .background(Palette.bg.ignoresSafeArea())
                    .toolbar {
                        ToolbarItem(placement: .principal) {
                            HStack(spacing: Grid.s) {
                                Logo(size: 22)
                                Text(s.rawValue).font(.ui(17, weight: .semibold)).foregroundStyle(Palette.text)
                            }
                        }
                    }
                    .navigationBarTitleDisplayMode(.inline)
                }
                .tabItem { Label(s.rawValue, systemImage: s.icon) }
                .tag(s)
            }
        }
        .tint(Palette.accent)
        // A first visit with nothing on the device opens on Models: there is nothing to chat with.
        .onChange(of: engine.modelsListed) { listed in
            guard listed, !nav.startChosen else { return }
            nav.startChosen = true
            if engine.models.local.isEmpty, nav.screen == .chat { nav.screen = .models }
        }
    }

    @ViewBuilder
    private func content(_ s: Screen) -> some View {
        switch s {
        case .chat: ChatView(openModels: { nav.open(.models) })
        case .models: ModelsView()
        case .plan: PlanView()
        case .settings: SettingsView()
        case .metrics: MetricsView()
        }
    }

    /// The bars in the palette, with Inter for their words and a 1-point line under them: UIKit's
    /// appearance, which SwiftUI's bars read.
    private static func styleBars() {
        let nav = UINavigationBarAppearance()
        nav.configureWithOpaqueBackground()
        nav.backgroundColor = UIColor(Palette.surface)
        nav.shadowColor = UIColor(Palette.line)
        nav.titleTextAttributes = [.foregroundColor: UIColor(Palette.text), .font: UIFont.ui(17, weight: .semibold)]
        nav.largeTitleTextAttributes = [.foregroundColor: UIColor(Palette.text), .font: UIFont.ui(32, weight: .bold)]
        UINavigationBar.appearance().standardAppearance = nav
        UINavigationBar.appearance().scrollEdgeAppearance = nav
        UINavigationBar.appearance().compactAppearance = nav

        let tab = UITabBarAppearance()
        tab.configureWithOpaqueBackground()
        tab.backgroundColor = UIColor(Palette.surface)
        tab.shadowColor = UIColor(Palette.line)
        let item = UITabBarItemAppearance()
        let font = UIFont.ui(11, weight: .medium)
        item.normal.titleTextAttributes = [.font: font, .foregroundColor: UIColor(Palette.muted)]
        item.normal.iconColor = UIColor(Palette.muted)
        item.selected.titleTextAttributes = [.font: font, .foregroundColor: UIColor(Palette.accentInk)]
        item.selected.iconColor = UIColor(Palette.accentInk)
        tab.stackedLayoutAppearance = item
        tab.inlineLayoutAppearance = item
        tab.compactInlineLayoutAppearance = item
        UITabBar.appearance().standardAppearance = tab
        UITabBar.appearance().scrollEdgeAppearance = tab
    }
}

/// What the engine is doing, in one line, on every screen (docs/screens.md, "The status line").
struct StatusLine: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var nav: Navigator
    /// The load whose context warning was dismissed on this device; one at a time is enough.
    @AppStorage("ctxWarningDismissed") private var ctxDismissed = ""

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            HStack(spacing: Grid.s) {
                StateDot(color: color)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.ui(14, weight: .semibold)).foregroundStyle(Palette.text).lineLimit(1)
                    if let detail { Text(detail).font(.ui(12)).foregroundStyle(Palette.muted).lineLimit(2) }
                }
                Spacer(minLength: Grid.s)
                downloads
                action
            }
            // A context below what a client of the server sends, while it serves the network: said
            // beside the model, with the way to the setting that raises it, until dismissed for
            // that model and context (docs/screens.md, "The status line").
            if engine.ready, let ctx = engine.contextAdvice, ctx.warn, ctx.dismissKey != ctxDismissed {
                HStack(alignment: .center, spacing: Grid.s) {
                    Text(ctx.warning).font(.ui(12)).foregroundStyle(Palette.warn).fixedSize(horizontal: false, vertical: true)
                    Spacer(minLength: 0)
                    Button("Raise \(ctx.label)") { nav.openSetting(ctx.key) }
                        .font(.ui(13, bold: true)).foregroundStyle(Palette.accentInk).buttonStyle(.borderless)
                    Button { ctxDismissed = ctx.dismissKey } label: {
                        Image(systemName: "xmark").frame(minWidth: 44, minHeight: 44)
                    }
                    .foregroundStyle(Palette.muted).buttonStyle(.borderless)
                    .accessibilityLabel("Dismiss the context warning")
                }
            }
        }
        .padding(.horizontal, Grid.m)
        .padding(.vertical, 10)
        .background(Palette.surface)
        .overlay(alignment: .bottom) { Rectangle().fill(Palette.line).frame(height: Grid.line) }
    }

    private var title: String {
        guard engine.serverUp, let info = engine.info else { return "Engine not running" }
        switch info.state {
        case .empty: return "No model loaded"
        case .planning: return "Planning, then loading…"
        case .loading: return "Loading model…"
        case .error: return "Engine error"
        case .ready:
            let name = info.model?.name ?? "model"
            return "\(name) · \(info.generating ? "generating" : "ready")"
        }
    }

    private var detail: String? {
        guard engine.serverUp, let info = engine.info else { return engine.serverError.map { firstLine($0) } }
        if info.state == .error { return firstLine(info.error) }
        // A model that loaded with streaming off is marked wherever its name is shown.
        if info.ready, info.model?.streaming == false { return "streaming off: plain mmap" }
        if engine.shared { return "shared on the network" }
        return nil
    }

    private var color: Color {
        guard engine.serverUp, let info = engine.info else { return Palette.err }
        switch info.state {
        case .ready: return info.generating ? Palette.accent : Palette.ok
        case .error: return Palette.err
        case .empty: return Palette.muted
        case .loading, .planning: return Palette.warn
        }
    }

    /// A running download, on every screen: how many and the share done, leading back to Models.
    @ViewBuilder
    private var downloads: some View {
        if let d = Library.downloadSummary(engine.downloads) {
            Button { nav.open(.models) } label: { Badge(text: Library.downloadText(count: d.count, fraction: d.fraction), color: Palette.accent) }
                .buttonStyle(.plain)
        }
    }

    @ViewBuilder
    private var action: some View {
        if !engine.serverUp {
            Button("Start") { engine.start() }.buttonStyle(.appSecondary)
        } else if engine.generating {
            Button("Stop") { Task { await engine.cancel() } }.buttonStyle(.appSecondary)
        } else if let s = engine.info?.state, s == .empty || s == .error, nav.screen != .models {
            Button("Models") { nav.open(.models) }.buttonStyle(.appSecondary)
        }
    }

    private func firstLine(_ s: String) -> String { s.split(whereSeparator: \.isNewline).first.map(String.init) ?? s }
}
