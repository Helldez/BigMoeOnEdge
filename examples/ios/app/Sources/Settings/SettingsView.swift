import BmoeKit
import SwiftUI

/// Settings (docs/screens.md): the engine tunables a form offers, rendered from the parameter
/// table, and the few settings that are the app's own. "Set by you" first, every row holding a
/// value the person set, then one card per group of the table, in its order, listing the other
/// rows the table puts on a form; every card starts closed. A link (`Navigator.openSetting`) opens
/// its row's card and lands on it. Each row appears once.
struct SettingsView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var preferences: Preferences
    /// The cards opened on this visit; everything else is closed, so the page opens as a short list.
    @State private var openCards: Set<String> = []
    @State private var appOpen = false
    @State private var shareOpen = false
    @State private var appearanceOpen = false
    @State private var mineOpen = false
    /// A row a link landed on that the form does not offer, shown for this visit.
    @State private var revealed: String?
    @State private var focusKey: String?
    @State private var failure: String?
    @State private var confirmReset = false

    var body: some View {
        ScrollViewReader { proxy in
            Page {
                banners
                ShareSection(open: $shareOpen)
                AppearanceSection(open: $appearanceOpen)
                // While the engine is down (a sharing change restarts it) the rows stay where they are, so
                // the page keeps its length and its scroll; a line above says why a change waits.
                if !engine.serverUp || engine.params.isEmpty {
                    Text(engine.params.isEmpty ? (engine.serverUp ? "Reading the parameter table…" : "The engine is not running.")
                        : "The engine is not running; changes apply once it is back.")
                        .font(.ui(15)).foregroundStyle(Palette.muted)
                }
                if !engine.params.isEmpty {
                    tools
                    let mine = SettingsForm.setByYou(engine.params, config: engine.config)
                    if !mine.isEmpty { setByYouCard(mine) }
                    ForEach(SettingsForm.cards(engine.params, revealed: revealed.map { [$0] } ?? [],
                                               setByYou: Set(mine.map(\.key)))) { c in card(c) }
                }
                AppSettingsSection(open: $appOpen)
                if preferences.metricsCSV {
                    NavigationLink {
                        MetricsView()
                            .background(Palette.bg.ignoresSafeArea())
                            .navigationTitle("Metrics")
                            .navigationBarTitleDisplayMode(.inline)
                    } label: {
                        HStack(spacing: Grid.s) {
                            Text("Metrics").font(.ui(18, bold: true)).foregroundStyle(Palette.accentInk)
                            Spacer()
                            Image(systemName: "chevron.right").foregroundStyle(Palette.muted)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .card(padding: 12)
                }
            }
            .onAppear { land(proxy) }
            .onChange(of: nav.settingTarget) { _ in land(proxy) }
            .onChange(of: engine.params.count) { _ in land(proxy) }
            .onChange(of: engine.config == nil) { _ in land(proxy) }
            // A row a link revealed is shown for that visit only.
            .onDisappear {
                revealed = nil
                focusKey = nil
            }
        }
        .confirmationDialog(resetTitle, isPresented: $confirmReset, titleVisibility: .visible) {
            Button("Reset", role: .destructive) { resetAll() }
            Button("Keep", role: .cancel) {}
        } message: {
            Text("The server's defaults and the plan apply to them again. The launch options and the model stay.")
        }
    }

    private var resettable: [String] {
        engine.config.map { SettingsForm.resettable($0, params: engine.params) } ?? []
    }

    private var resetTitle: String {
        let n = resettable.count
        return "Remove your \(n) \(n == 1 ? "value" : "values")?"
    }

    @ViewBuilder
    private var banners: some View {
        if let c = engine.config {
            if !c.valid { Banner(text: "This configuration does not validate, and a load is refused until it does: \(c.error)", color: Palette.err) }
            if c.reloadRequired, engine.ready {
                Banner(text: "Some changes apply when the model is loaded again: the rows marked needs reload.") {
                    Button("Reload") { reload() }.buttonStyle(.appPrimary).disabled(!c.valid)
                }
            }
        }
        if let failure {
            Banner(text: failure, color: Palette.err) {
                Button("Dismiss") { self.failure = nil }.buttonStyle(.appSecondary)
            }
        }
    }

    /// Reset all my values, when there is one.
    @ViewBuilder
    private var tools: some View {
        if !resettable.isEmpty {
            Button("Reset all my values (\(resettable.count))") { confirmReset = true }.buttonStyle(.appSecondary)
        }
    }

    // MARK: a card

    private func card(_ c: SettingsForm.Card) -> some View {
        let open = openCards.contains(c.group)
        return VStack(alignment: .leading, spacing: Grid.s) {
            Button {
                if open { openCards.remove(c.group) } else { openCards.insert(c.group) }
            } label: {
                HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                    Text(c.label).font(.ui(18, bold: true)).foregroundStyle(Palette.accentInk)
                    Spacer()
                    Image(systemName: open ? "chevron.up" : "chevron.down").foregroundStyle(Palette.muted)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if !c.help.isEmpty {
                Text(c.help).font(.ui(12)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
            }
            if open { rows(c.rows, readOnly: c.readOnly) }
        }
        .card(padding: 12)
    }

    /// Every value the person set, wherever its row sits in the table: one place to see and undo them.
    private func setByYouCard(_ specs: [ParamSpec]) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            CardHeader(title: "Set by you · \(specs.count)", open: $mineOpen)
            if mineOpen { rows(specs, readOnly: false) }
        }
        .card(padding: 12)
    }

    private func rows(_ specs: [ParamSpec], readOnly: Bool) -> some View {
        let decisions = Dictionary((engine.config?.plan ?? []).map { ($0.knob, $0) }, uniquingKeysWith: { _, last in last })
        let reload = Set(engine.config?.reloadKeys ?? [])
        return VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(specs.enumerated()), id: \.element.key) { i, spec in
                if i > 0 { Rectangle().fill(Palette.line).frame(height: 1) }
                ParamRow(spec: spec, readOnly: readOnly, decision: decisions[spec.key], needsReload: reload.contains(spec.key),
                         focusRequest: focusKey == spec.key, onError: { failure = $0 })
                    .id("param-\(spec.key)")
            }
        }
    }

    // MARK: actions

    /// A link to one setting lands on it: "Set by you" opened when the person set it, else its group's
    /// card, with the row shown even where the form does not offer it; the row scrolled into view
    /// and its field focused. Once per link.
    private func land(_ proxy: ScrollViewProxy) {
        guard let key = nav.settingTarget, engine.config != nil,
              let spec = engine.params.first(where: { $0.key == key }) else { return }
        nav.settingTarget = nil
        if SettingsForm.setByYou(engine.params, config: engine.config).contains(where: { $0.key == key }) {
            mineOpen = true
        } else {
            if !spec.form { revealed = key }
            openCards.insert(spec.group)
        }
        focusKey = nil
        // After the cards above have opened, so the row is there to scroll to.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            withAnimation { proxy.scrollTo("param-\(key)", anchor: .center) }
            focusKey = key
            // The focus is taken once: a later visit opens on the page, not on this row.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { if focusKey == key { focusKey = nil } }
        }
    }

    /// The model loaded again with the configuration as it now stands.
    private func reload() {
        Task {
            do { try await engine.load(engine.info?.model?.path) } catch { failure = error.localizedDescription }
        }
    }

    private func resetAll() {
        let keys = resettable
        Task {
            do { try await engine.setConfig(reset: keys) } catch { failure = error.localizedDescription }
        }
    }
}

/// One parameter: its label, its help, its control by type, what its value means and what is
/// usual, where its value came from and whether the loaded model runs with it.
struct ParamRow: View {
    @EnvironmentObject private var engine: Engine
    let spec: ParamSpec
    /// Shown, not edited: its group is set elsewhere (the model, from Models).
    var readOnly = false
    /// The applied plan's decision on this row, if it made one.
    var decision: PlanDecision?
    /// The value differs from the one the loaded session runs with (the server says which).
    var needsReload = false
    /// A link landed here: the field takes the focus.
    var focusRequest = false
    var onError: (String) -> Void

    @State private var text = ""
    @State private var more = false
    @FocusState private var focused: Bool

    private var value: JSON { engine.config?[spec.key] ?? .null }
    private var userSet: Bool { engine.config?.userKeys.contains(spec.key) == true }
    private var source: SettingsForm.Source { SettingsForm.source(userSet: userSet, decision: decision) }
    private var rejected: String? { engine.config?.rejected[spec.key] }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                Text(spec.label).font(.ui(17)).foregroundStyle(Palette.text)
                Spacer(minLength: Grid.s)
                if spec.kind == .bool, !readOnly { toggle }
            }
            if readOnly {
                Text(readOnlyText).font(.ui(15)).foregroundStyle(Palette.text).lineLimit(2)
            } else if spec.kind != .bool {
                control
            }
            if let rejected {
                // The limits appear only here, beside a value the server refused.
                Text([rejected, spec.rangeText.map { "accepted: \($0)" }].compactMap { $0 }.joined(separator: " · "))
                    .font(.ui(12)).foregroundStyle(Palette.err)
            }
            help
            marks
        }
        .padding(.vertical, 10)
        .padding(.leading, SettingsForm.isDefault(spec, value) ? 0 : 6)
        .overlay(alignment: .leading) {
            // A value off its default is edged in the accent, as the web form does.
            if !SettingsForm.isDefault(spec, value) { Rectangle().fill(Palette.accent).frame(width: 2).padding(.vertical, 10) }
        }
        .onAppear {
            text = value.display
            if focusRequest { focused = true }
        }
        .onChange(of: value) { v in if !focused { text = v.display } }
        .onChange(of: focusRequest) { if $0 { focused = true } }
    }

    private var readOnlyText: String {
        let shown = spec.kind == .path ? (value.display as NSString).lastPathComponent : value.display
        return shown.isEmpty ? "none" : shown
    }

    /// The help's first sentence, always; the rest and the named values on demand.
    @ViewBuilder
    private var help: some View {
        let parts = spec.helpParts
        let extra = [parts.rest.isEmpty ? nil : parts.rest,
                     spec.special.isEmpty ? nil : spec.special.map { "\($0.value) = \($0.label)" }.joined(separator: "; ")]
            .compactMap { $0 }
        if !parts.lead.isEmpty {
            Text(parts.lead).font(.ui(12)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
        }
        if !extra.isEmpty {
            if more {
                ForEach(extra, id: \.self) {
                    Text($0).font(.ui(12)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
                }
            }
            Button(more ? "Less" : "More") { more.toggle() }
                .font(.ui(13)).foregroundStyle(Palette.accentInk).buttonStyle(.borderless)
        }
    }

    private var toggle: some View {
        Toggle("", isOn: Binding(get: { value.bool ?? false }, set: { set(.bool($0)) }))
            .labelsHidden()
            .tint(Palette.accent)
    }

    @ViewBuilder
    private var control: some View {
        switch spec.kind {
        case .choice:
            Picker(spec.label, selection: Binding(get: { value.display }, set: { set(.string($0)) })) {
                ForEach(spec.choices, id: \.value) { Text($0.label).tag($0.value) }
            }
            .pickerStyle(.menu)
            .tint(Palette.accent)
        case .path:
            // A path a client may set: one of the files the server lists.
            Picker(spec.label, selection: Binding(get: { value.display }, set: { set(.string($0)) })) {
                Text("None").tag("")
                ForEach(engine.models.local) { Text($0.name).tag($0.path) }
            }
            .pickerStyle(.menu)
            .tint(Palette.accent)
        default:
            HStack(spacing: Grid.s) {
                TextField(spec.acceptsAuto ? "auto" : spec.label, text: $text)
                    .font(.ui(15))
                    .keyboardType(spec.kind == .text ? .default : spec.acceptsAuto ? .asciiCapable : .numbersAndPunctuation)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .focused($focused)
                    .onSubmit(commit)
                    .onChange(of: focused) { f in if !f { commit() } }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .overlay(RoundedRectangle(cornerRadius: Grid.radius, style: .continuous).strokeBorder(focused ? Palette.accent : Palette.line, lineWidth: Grid.line))
                if let unit = spec.unit { Text(unit).font(.ui(14)).foregroundStyle(Palette.muted) }
                if spec.acceptsAuto && value.string != "auto" {
                    Button("auto") { set(.string("auto")) }.buttonStyle(.appSecondary)
                }
            }
            // Under the number: what the value means (0 = off), and what is usual. Never the type's limit.
            if spec.kind == .int || spec.kind == .float {
                HStack(spacing: Grid.s) {
                    if let named = spec.specialLabel(value) {
                        Text("= \(named)").font(.ui(12, weight: .semibold)).foregroundStyle(Palette.text)
                    }
                    if let usually = spec.typicalText { Text(usually).font(.ui(12)).foregroundStyle(Palette.muted) }
                }
            }
        }
    }

    /// One chip for where the value came from, the row's other marks, and the way back.
    private var marks: some View {
        let planText = decision.map { $0.valueText.isEmpty ? $0.value : $0.valueText } ?? ""
        return VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: Grid.s) {
                if !readOnly {
                    Badge(text: source.text, color: source == .you ? Palette.accent : source == .plan ? Palette.ok : Palette.muted)
                }
                if spec.lossy { Badge(text: "changes the reply", color: Palette.warn) }
                if spec.scope == .request { Badge(text: "per message", color: Palette.accentInk) }
                if needsReload { Badge(text: "needs reload", color: Palette.warn) }
            }
            if userSet, !readOnly {
                // Taking the plan's value is removing the person's: the plan's layer is beneath it.
                if SettingsForm.differsFromPlan(value, decision) {
                    link("Use plan value" + (planText.isEmpty ? "" : ": \(planText)"))
                } else if decision?.isSet == true {
                    link("Let the plan set it")
                } else {
                    link("Reset to default")
                }
            }
        }
    }

    private func link(_ title: String) -> some View {
        Button(title) { reset() }.font(.ui(13)).foregroundStyle(Palette.accentInk).buttonStyle(.borderless)
    }

    private func commit() {
        guard text != value.display else { return }
        guard let v = spec.value(fromText: text) else {
            onError("\(spec.label): \"\(text)\" is not a \(spec.kind == .float ? "number" : spec.kind == .int ? "whole number" : "value") here.")
            text = value.display
            return
        }
        set(v)
    }

    private func set(_ v: JSON) {
        Task {
            do { try await engine.setConfig([spec.key: v]) } catch { onError(error.localizedDescription) }
        }
    }

    private func reset() {
        Task {
            do { try await engine.setConfig(reset: [spec.key]) } catch { onError(error.localizedDescription) }
        }
    }
}

/// A card's header: its title, a line under it, and the chevron that opens it.
private struct CardHeader: View {
    let title: String
    var detail = ""
    var detailColor = Palette.muted
    @Binding var open: Bool

    var body: some View {
        Button {
            open.toggle()
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                    Text(title).font(.ui(18, bold: true)).foregroundStyle(Palette.accentInk)
                    Spacer()
                    Image(systemName: open ? "chevron.up" : "chevron.down").foregroundStyle(Palette.muted)
                }
                if !detail.isEmpty {
                    Text(detail).font(.ui(13)).foregroundStyle(detailColor).lineLimit(2).multilineTextAlignment(.leading)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

/// Serving the engine to other devices (docs/screens.md): first in Settings, since it is what makes
/// the phone a server. Its header says the state while the card is closed: whether it serves, and
/// where. The key is the one kept in the keychain; the addresses are read when the card is drawn.
struct ShareSection: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var nav: Navigator
    @EnvironmentObject private var preferences: Preferences
    @Binding var open: Bool
    @State private var showKey = false
    @State private var copied: String?

    private var urls: [String] { NetworkAddress.all().map { "http://\($0):\(EngineServer.port)" } }

    var body: some View {
        let on = preferences.shareOnNetwork
        let urls = on ? urls : []
        VStack(alignment: .leading, spacing: Grid.m) {
            CardHeader(title: "Serve on the network",
                       detail: !on ? "Off" : urls.first.map { "On · \($0)" } ?? "On · not on a network",
                       detailColor: on && urls.isEmpty ? Palette.warn : Palette.muted,
                       open: $open)
            if open {
                Toggle(isOn: Binding(get: { preferences.shareOnNetwork }, set: {
                    preferences.shareOnNetwork = $0
                    engine.restart()
                })) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Serve on the network").foregroundStyle(Palette.text)
                        Text("Other devices on this network use this phone's engine as an OpenAI-compatible server. "
                            + "Changing this restarts the engine.")
                            .font(.ui(12)).foregroundStyle(Palette.muted)
                    }
                }
                .tint(Palette.accent)
                if on { details(urls) }
            }
        }
        .card(padding: 12)
    }

    private func details(_ urls: [String]) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Text("Address").font(.ui(13)).foregroundStyle(Palette.muted)
            if urls.isEmpty {
                Text("Not on a Wi-Fi or wired network: no other device can reach the engine until this one joins one.")
                    .font(.ui(12)).foregroundStyle(Palette.warn).fixedSize(horizontal: false, vertical: true)
            }
            ForEach(urls, id: \.self) { url in
                HStack(spacing: Grid.s) {
                    Text(url).font(.mono(15)).foregroundStyle(Palette.text).textSelection(.enabled)
                    Spacer(minLength: Grid.s)
                    copyButton(url, id: url)
                }
            }
            Text("API key").font(.ui(13)).foregroundStyle(Palette.muted).padding(.top, 4)
            // Masked by default: the screen may be seen or recorded by someone the key is not for.
            Text(showKey ? engine.networkKey : String(repeating: "•", count: min(engine.networkKey.count, 24)))
                .font(.mono(15)).foregroundStyle(Palette.text).textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: Grid.s) {
                Button(showKey ? "Hide" : "Show") { showKey.toggle() }.buttonStyle(.appSecondary)
                copyButton(engine.networkKey, id: "key")
                Button("New key") {
                    copied = nil
                    engine.newNetworkKey()
                }
                .buttonStyle(.appSecondary)
            }
            if let first = urls.first {
                Text("In a client: base URL \(first)/v1, and the key above.")
                    .font(.ui(12)).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
            }
            // Where the clients are switched on, what they need: said here whether or not the
            // status line's warning was dismissed.
            if engine.shared, engine.ready, let ctx = engine.contextAdvice, ctx.short {
                HStack(alignment: .center, spacing: Grid.s) {
                    Text(ctx.warning).font(.ui(12)).foregroundStyle(Palette.warn)
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer(minLength: 0)
                    Button("Raise \(ctx.label)") { nav.openSetting(ctx.key) }
                        .font(.ui(13, bold: true)).foregroundStyle(Palette.accentInk).buttonStyle(.borderless)
                }
            }
            Text("Prompts and answers cross the network unencrypted: use it on a network you trust.")
                .font(.ui(12)).foregroundStyle(Palette.warn).fixedSize(horizontal: false, vertical: true)
        }
    }

    private func copyButton(_ text: String, id: String) -> some View {
        Button(copied == id ? "Copied" : "Copy") {
            UIPasteboard.general.string = text
            copied = id
        }
        .buttonStyle(.appSecondary)
    }
}

/// How the app looks and what it shows: the theme, applied at once, and the Metrics switch.
struct AppearanceSection: View {
    @EnvironmentObject private var preferences: Preferences
    @Binding var open: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            CardHeader(title: "Appearance", open: $open)
            if open {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Theme").foregroundStyle(Palette.text)
                    Text("Follow the system, or always light or dark.").font(.ui(12)).foregroundStyle(Palette.muted)
                    Picker("Theme", selection: $preferences.theme) {
                        ForEach(Preferences.Theme.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                }
                Toggle(isOn: $preferences.metricsCSV) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Metrics").foregroundStyle(Palette.text)
                        Text("Record a per-token CSV for every loaded model and show the measurements here.")
                            .font(.ui(12)).foregroundStyle(Palette.muted)
                    }
                }
                .tint(Palette.accent)
            }
        }
        .card(padding: 12)
    }
}

/// The settings that are the app's own (docs/screens.md): starting and stopping the engine. Never
/// an engine parameter. A card that folds like the groups' cards.
struct AppSettingsSection: View {
    @EnvironmentObject private var engine: Engine
    @Binding var open: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            CardHeader(title: "This app", detail: "How this phone runs the engine.", open: $open)
            if open {
                HStack(spacing: Grid.s) {
                    if engine.serverUp {
                        Button("Stop engine") { engine.stop() }.buttonStyle(.appDestructive)
                    } else {
                        Button("Start engine") { engine.start() }.buttonStyle(.appPrimary)
                    }
                    Spacer()
                    if let v = engine.info?.version { Text("engine \(v)").font(.ui(13)).foregroundStyle(Palette.muted) }
                }
            }
        }
        .card(padding: 12)
    }
}
