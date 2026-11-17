import BmoeKit
import SwiftUI

/// Which screen is open, and the setting a link points at: the app's `#/settings/<key>`. The Plan
/// screen's decisions, the context warning and Apply's list link to a row of Settings this way.
@MainActor
final class Navigator: ObservableObject {
    enum Screen: String, CaseIterable, Identifiable {
        case chat = "Chat", models = "Models", plan = "Plan", settings = "Settings"
        var id: String { rawValue }

        var icon: String {
            switch self {
            case .chat: return "bubble.left.and.bubble.right"
            case .models: return "shippingbox"
            case .plan: return "cpu"
            case .settings: return "slider.horizontal.3"
            }
        }
    }

    @Published var screen = Screen.chat
    /// The parameter Settings is to land on, once; nil when no link is pending.
    @Published var settingTarget: String?
    /// The first run (docs/screens.md, "First run"): the catalog entry it is getting, from its
    /// Download until the conversation opens on it; and whether the person left it (Skip, or More
    /// models) for this launch.
    @Published var setupEntry: String?
    @Published var setupDismissed = false
    /// The first run is what the Models screen shows now: its bar says "Get started".
    @Published var setupShowing = false
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

/// The four screens in a bottom bar, the status line above every one of them. Metrics is not one:
/// it opens from Settings, where it always is.
struct RootView: View {
    typealias Screen = Navigator.Screen
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var preferences: Preferences

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
                    // The screens' own controls in the accent; the bar's, outside this, on the shell.
                    .tint(Palette.accent)
                    .toolbar {
                        ToolbarItem(placement: .principal) {
                            Text(s == .models && nav.setupShowing ? "Get started" : s.rawValue)
                                .font(.ui(17, weight: .semibold)).foregroundStyle(Palette.onShell)
                        }
                    }
                    .navigationBarTitleDisplayMode(.inline)
                    .shellBar()
                }
                .tint(Palette.onShell)
                .tabItem { Label(s.rawValue, systemImage: s.icon) }
                .tag(s)
            }
        }
        .tint(Palette.accent)
        .preferredColorScheme(preferences.theme.colorScheme)
        .onAppear { applyTheme() }
        .onChange(of: preferences.theme) { _ in applyTheme() }
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
        }
    }

    /// The theme on the windows too: the bars are UIKit and resolve the palette's dynamic colours
    /// from the window's style, and a scheme SwiftUI forced is not always handed back to the
    /// system by `preferredColorScheme(nil)` alone.
    private func applyTheme() {
        let style = preferences.theme.interfaceStyle
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            for window in scene.windows { window.overrideUserInterfaceStyle = style }
        }
    }

    /// The bars in the palette, with Hanken Grotesk for their words: UIKit's appearance, which
    /// SwiftUI's bars read. The top bar is the shell's ink with its words in on-shell (every
    /// navigation bar, the sheets' too); the tab bar a sheet with a 1-point line over it.
    private static func styleBars() {
        let nav = UINavigationBarAppearance()
        nav.configureWithOpaqueBackground()
        nav.backgroundColor = UIColor(Palette.shell)
        nav.shadowColor = .clear
        nav.titleTextAttributes = [.foregroundColor: UIColor(Palette.onShell), .font: UIFont.ui(17, weight: .semibold)]
        nav.largeTitleTextAttributes = [.foregroundColor: UIColor(Palette.onShell), .font: UIFont.ui(32, weight: .bold)]
        let plain = UIBarButtonItemAppearance()
        plain.normal.titleTextAttributes = [.foregroundColor: UIColor(Palette.onShell), .font: UIFont.ui(17)]
        nav.buttonAppearance = plain
        nav.backButtonAppearance = plain
        let done = UIBarButtonItemAppearance()
        done.normal.titleTextAttributes = [.foregroundColor: UIColor(Palette.onShell), .font: UIFont.ui(17, weight: .semibold)]
        nav.doneButtonAppearance = done
        UINavigationBar.appearance().standardAppearance = nav
        UINavigationBar.appearance().scrollEdgeAppearance = nav
        UINavigationBar.appearance().compactAppearance = nav
        UINavigationBar.appearance().tintColor = UIColor(Palette.onShell)

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

extension View {
    /// A navigation bar on the shell: its background shown always, and the light status bar over it.
    func shellBar() -> some View { modifier(ShellBar()) }
}

/// The bar is told it is dark so its status bar and controls are light; the shell is then fixed to
/// the screen's own scheme, or the bar would resolve the dark scheme's shell in the light one.
private struct ShellBar: ViewModifier {
    @Environment(\.colorScheme) private var scheme

    func body(content: Content) -> some View {
        content
            .toolbarBackground(Palette.resolved(Palette.shell, scheme), for: .navigationBar)
            .toolbarBackground(.visible, for: .navigationBar)
            .toolbarColorScheme(.dark, for: .navigationBar)
    }
}

/// What the engine and the open conversation are doing, on every screen (docs/screens.md, "The
/// status line"; docs/design.md, "The seal"): the logo beside a few plain words from their state,
/// never a figure. While it works the logo breathes, unless the person asked for less motion.
struct StatusLine: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var chat: ChatModel
    @EnvironmentObject private var nav: Navigator
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    /// The load whose context warning was dismissed on this device; one at a time is enough.
    @AppStorage("ctxWarningDismissed") private var ctxDismissed = ""

    var body: some View {
        let said = StatusWords.say(input)
        let busy = said.mood == .busy && !reduceMotion
        VStack(alignment: .leading, spacing: Grid.s) {
            HStack(spacing: 10) {
                // Opacity 1 to 0.45 and back, 1.4 s each way, eased: the web's breathing chip, read off
                // the clock so it stops the moment the work does.
                TimelineView(.animation(paused: !busy)) { tl in
                    let phase = tl.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 2.8) / 2.8
                    let breath = 0.5 - 0.5 * cos(2 * .pi * phase)
                    Logo(size: 30)
                        .opacity(busy ? 1 - 0.55 * breath : said.mood == .sleep ? 0.7 : 1)
                        .saturation(said.mood == .sleep ? 0.2 : 1)
                }
                .frame(width: 30, height: 30)
                .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(said.text).font(.ui(14, weight: .semibold))
                        .foregroundStyle(said.mood == .alert ? Palette.warn : Palette.muted).lineLimit(1)
                    if let detail { Text(detail).font(.ui(12)).foregroundStyle(Palette.muted).lineLimit(2) }
                }
                .accessibilityElement(children: .combine)
                .accessibilityAddTraits(.updatesFrequently)
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
        .padding(.vertical, 8)
        .background(Palette.bg)
        .overlay(alignment: .bottom) { Rectangle().fill(Palette.line).frame(height: Grid.line) }
    }

    private var input: StatusWords.Input { .of(engine: engine, chat: chat) }

    /// Why the engine is not reachable, when the words alone leave a person stuck: the server runs
    /// inside this app, so a port another program holds is said here (README, "Run in the
    /// simulator"). A load's error is on Models, where "See Models" points.
    private var detail: String? {
        guard !engine.serverUp || engine.info == nil else { return nil }
        return engine.serverError.map { firstLine($0) }
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

extension StatusWords.Input {
    /// The engine's state and the open conversation's, as the words are chosen from them (the web
    /// UI's: an answer of the open conversation is what "working" means). The status line and the
    /// welcome read the same.
    @MainActor
    static func of(engine: Engine, chat: ChatModel) -> StatusWords.Input {
        let last = chat.items.last
        let running = chat.running
        return StatusWords.Input(
            reachable: engine.serverUp, engine: engine.serverUp ? engine.info?.state : nil, running: running,
            flashWaitMs: running ? engine.live.flashWaitMs : 0, wallMs: running ? engine.live.wallMs : 0,
            lastIsAnswer: last?.kind == .assistant, answerHasText: !(last?.text.isEmpty ?? true),
            conversationFailed: chat.current?.state == "error", stopped: chat.lastStopped)
    }
}
