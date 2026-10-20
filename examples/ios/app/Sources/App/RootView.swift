import SwiftUI

/// The five screens in a bottom bar, the status line above every one of them.
struct RootView: View {
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

    @State private var screen = Screen.chat

    init() {
        Self.styleBars()
    }

    var body: some View {
        TabView(selection: $screen) {
            ForEach(Screen.allCases) { s in
                NavigationStack {
                    VStack(spacing: 0) {
                        StatusLine(openModels: { screen = .models })
                        content(s)
                    }
                    .background(Palette.bg.ignoresSafeArea())
                    .toolbar {
                        ToolbarItem(placement: .principal) {
                            HStack(spacing: Grid.s) {
                                Logo(size: 24)
                                Text(s.rawValue).font(.pixel(20, bold: true)).foregroundStyle(Palette.text)
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
    }

    @ViewBuilder
    private func content(_ s: Screen) -> some View {
        switch s {
        case .chat: ChatView(openModels: { screen = .models })
        case .models: ModelsView()
        case .plan: PlanView(openSettings: { screen = .settings })
        case .settings: SettingsView()
        case .metrics: MetricsView()
        }
    }

    /// The bars in the palette, with the pixel face for their titles: UIKit's appearance, which
    /// SwiftUI's bars read.
    private static func styleBars() {
        let nav = UINavigationBarAppearance()
        nav.configureWithOpaqueBackground()
        nav.backgroundColor = UIColor(Palette.surface)
        nav.shadowColor = UIColor(Palette.border)
        nav.titleTextAttributes = [.foregroundColor: UIColor(Palette.text)]
        UINavigationBar.appearance().standardAppearance = nav
        UINavigationBar.appearance().scrollEdgeAppearance = nav
        UINavigationBar.appearance().compactAppearance = nav

        let tab = UITabBarAppearance()
        tab.configureWithOpaqueBackground()
        tab.backgroundColor = UIColor(Palette.surface)
        tab.shadowColor = UIColor(Palette.border)
        let item = UITabBarItemAppearance()
        let font = UIFont(name: "PixelifySans-Regular", size: 12) ?? .systemFont(ofSize: 12)
        item.normal.titleTextAttributes = [.font: font, .foregroundColor: UIColor(Palette.muted)]
        item.normal.iconColor = UIColor(Palette.muted)
        item.selected.titleTextAttributes = [.font: font, .foregroundColor: UIColor(Palette.accent)]
        item.selected.iconColor = UIColor(Palette.accent)
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
    var openModels: () -> Void

    var body: some View {
        HStack(spacing: Grid.s) {
            Rectangle().fill(color).frame(width: 10, height: 10)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.pixel(14, bold: true)).foregroundStyle(Palette.text).lineLimit(1)
                if let detail { Text(detail).font(.caption).foregroundStyle(Palette.muted).lineLimit(2) }
            }
            Spacer(minLength: Grid.s)
            action
        }
        .padding(.horizontal, Grid.m)
        .padding(.vertical, Grid.s)
        .background(Palette.surface)
        .overlay(alignment: .bottom) { Rectangle().fill(Palette.border).frame(height: Grid.edge) }
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

    @ViewBuilder
    private var action: some View {
        if !engine.serverUp {
            Button("Start") { engine.start() }.buttonStyle(.pixel)
        } else if engine.generating {
            Button("Stop") { Task { await engine.cancel() } }.buttonStyle(.pixel)
        } else if let s = engine.info?.state, s == .empty || s == .error {
            Button("Models", action: openModels).buttonStyle(.pixel)
        }
    }

    private func firstLine(_ s: String) -> String { s.split(whereSeparator: \.isNewline).first.map(String.init) ?? s }
}
