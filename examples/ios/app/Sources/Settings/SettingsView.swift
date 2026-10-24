import BmoeKit
import SwiftUI

/// Settings (docs/screens.md): every engine tunable, rendered from the parameter table, and the few
/// settings that are the app's own. One card per group of the table, in its order; a card shows its
/// basic rows (the recommended configuration) and folds its deeper rows inside it, one heading per
/// level. A filter shows rows up to a level, a search reads every level, and a link
/// (`Navigator.openSetting`) lands on one row. Each row appears once.
struct SettingsView: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var nav: Navigator
    /// The level filter, remembered on this device.
    @AppStorage("settings.level") private var storedLevel = ParamSpec.Level.advanced.rawValue
    /// A link to a deeper row widens the filter for this visit, without changing what is remembered.
    @State private var widened: ParamSpec.Level?
    @State private var query = ""
    @State private var closedCards: [String: Bool] = [:]
    @State private var openFolds: [String: Bool] = [:]
    @State private var focusKey: String?
    @State private var failure: String?
    @State private var confirmReset = false

    private var level: ParamSpec.Level { widened ?? ParamSpec.Level(rawValue: storedLevel) ?? .advanced }
    private var searching: Bool { !query.trimmingCharacters(in: .whitespaces).isEmpty }

    var body: some View {
        ScrollViewReader { proxy in
            Page {
                banners
                // While the engine is down (a sharing change restarts it) the rows stay where they are, so
                // the page keeps its length and its scroll; a line above says why a change waits.
                if !engine.serverUp || engine.params.isEmpty {
                    Text(engine.params.isEmpty ? (engine.serverUp ? "Reading the parameter table…" : "The engine is not running.")
                        : "The engine is not running; changes apply once it is back.")
                        .font(.subheadline).foregroundStyle(Palette.muted)
                }
                if !engine.params.isEmpty {
                    tools
                    let cards = SettingsForm.cards(engine.params, upTo: level, query: query,
                                                   pinned: SettingsForm.pinned(engine.params, config: engine.config))
                    if searching {
                        Text(cards.isEmpty ? "No setting matches." : "Every level is searched.").font(.caption).foregroundStyle(Palette.muted)
                    }
                    ForEach(cards) { c in card(c) }
                }
                AppSettingsSection()
            }
            .onAppear { land(proxy) }
            .onChange(of: nav.settingTarget) { _ in land(proxy) }
            .onChange(of: engine.params.count) { _ in land(proxy) }
            .onChange(of: engine.config == nil) { _ in land(proxy) }
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
                    Button("Reload") { reload() }.buttonStyle(.pixelPrimary).disabled(!c.valid)
                }
            }
        }
        if let failure {
            Banner(text: failure, color: Palette.err) {
                Button("Dismiss") { self.failure = nil }.buttonStyle(.pixel)
            }
        }
    }

    /// The search, the level filter with its counts, and Reset all my values.
    private var tools: some View {
        let counts = SettingsForm.levelCounts(engine.params)
        return VStack(alignment: .leading, spacing: Grid.s) {
            PixelField(placeholder: "Search settings", text: $query)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(ParamSpec.Level.allCases, id: \.self) { l in
                        let shown = SettingsForm.widened(l, toShow: level) == level
                        Button {
                            widened = nil
                            storedLevel = l.rawValue
                        } label: {
                            Text("\(l.rawValue.capitalized) \(counts[l] ?? 0)")
                                .font(.pixel(13, bold: l == level))
                                .foregroundStyle(l == level ? Palette.accentText : Palette.text)
                                .padding(.horizontal, 8).padding(.vertical, 4)
                                .background(l == level ? Palette.accent : .clear)
                                .overlay(Rectangle().strokeBorder(shown ? Palette.accent : Palette.border, lineWidth: Grid.edge))
                        }
                        .buttonStyle(.plain)
                        .accessibilityAddTraits(l == level ? .isSelected : [])
                    }
                }
                .padding(.vertical, 2)
            }
            .disabled(searching)
            .opacity(searching ? 0.45 : 1)
            if !resettable.isEmpty {
                Button("Reset all my values (\(resettable.count))") { confirmReset = true }.buttonStyle(.pixel)
            }
        }
    }

    // MARK: a card

    private func card(_ c: SettingsForm.Card) -> some View {
        let open = searching || !(closedCards[c.group] ?? c.basic.isEmpty)
        return VStack(alignment: .leading, spacing: Grid.s) {
            Button {
                closedCards[c.group] = open
            } label: {
                HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                    Text(c.label).font(.pixel(18, bold: true)).foregroundStyle(Palette.accent2)
                    if c.basic.isEmpty { Badge(text: "\(c.moreCount)") }
                    Spacer()
                    Image(systemName: open ? "chevron.up" : "chevron.down").foregroundStyle(Palette.muted)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(searching)
            if !c.help.isEmpty {
                Text(c.help).font(.caption).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
            }
            if open {
                rows(c.basic, c)
                if !c.more.isEmpty {
                    if !c.basic.isEmpty {
                        Fold(title: "\(c.moreCount) more in \(c.label): \(c.more.map(\.level.rawValue).joined(separator: ", "))",
                             isOpen: Binding(get: { searching || openFolds[c.group] == true },
                                             set: { if !searching { openFolds[c.group] = $0 } }),
                             boxed: false)
                        { levels(c) }
                    } else {
                        levels(c)
                    }
                }
                if c.hidden > 0, !searching {
                    Text("\(c.hidden) more at a deeper level: widen the filter above to see \(c.hidden == 1 ? "it" : "them").")
                        .font(.caption).foregroundStyle(Palette.muted)
                }
            }
        }
        .card(padding: 12)
    }

    private func levels(_ c: SettingsForm.Card) -> some View {
        ForEach(c.more, id: \.level) { m in
            VStack(alignment: .leading, spacing: 2) {
                Text(m.level.rawValue.capitalized).font(.pixel(15, bold: true)).foregroundStyle(Palette.text).padding(.top, Grid.s)
                let hint = SettingsForm.levelHint(m.level)
                if !hint.isEmpty { Text(hint).font(.caption).foregroundStyle(Palette.muted) }
                rows(m.rows, c)
            }
        }
    }

    private func rows(_ specs: [ParamSpec], _ c: SettingsForm.Card) -> some View {
        let decisions = Dictionary((engine.config?.plan ?? []).map { ($0.knob, $0) }, uniquingKeysWith: { _, last in last })
        let reload = Set(engine.config?.reloadKeys ?? [])
        return VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(specs.enumerated()), id: \.element.key) { i, spec in
                if i > 0 { Rectangle().fill(Palette.border).frame(height: 1) }
                ParamRow(spec: spec, readOnly: c.readOnly, decision: decisions[spec.key], needsReload: reload.contains(spec.key),
                         focusRequest: focusKey == spec.key, onError: { failure = $0 })
                    .id("param-\(spec.key)")
            }
        }
    }

    // MARK: actions

    /// A link to one setting lands on it: the filter widened to its level, its card and fold
    /// opened, the row scrolled into view and its field focused. Once per link.
    private func land(_ proxy: ScrollViewProxy) {
        guard let key = nav.settingTarget, engine.config != nil,
              let spec = engine.params.first(where: { $0.key == key }) else { return }
        nav.settingTarget = nil
        query = ""
        if SettingsForm.widened(level, toShow: spec.level) != level { widened = spec.level }
        closedCards[spec.group] = false
        if spec.level != .basic { openFolds[spec.group] = true }
        focusKey = nil
        // After the cards above have opened, so the row is there to scroll to.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            withAnimation { proxy.scrollTo("param-\(key)", anchor: .center) }
            focusKey = key
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
                Text(spec.label).font(.body).foregroundStyle(Palette.text)
                Spacer(minLength: Grid.s)
                if spec.kind == .bool, !readOnly { toggle }
            }
            if readOnly {
                Text(readOnlyText).font(.pixel(15)).foregroundStyle(Palette.text).lineLimit(2)
            } else if spec.kind != .bool {
                control
            }
            if let rejected {
                // The limits appear only here, beside a value the server refused.
                Text([rejected, spec.rangeText.map { "accepted: \($0)" }].compactMap { $0 }.joined(separator: " · "))
                    .font(.caption).foregroundStyle(Palette.err)
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
            Text(parts.lead).font(.caption).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
        }
        if !extra.isEmpty {
            if more {
                ForEach(extra, id: \.self) {
                    Text($0).font(.caption).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
                }
            }
            Button(more ? "Less" : "More") { more.toggle() }
                .font(.pixel(13)).foregroundStyle(Palette.accent).buttonStyle(.borderless)
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
                    .font(.pixel(15))
                    .keyboardType(spec.kind == .text ? .default : spec.acceptsAuto ? .asciiCapable : .numbersAndPunctuation)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .focused($focused)
                    .onSubmit(commit)
                    .onChange(of: focused) { f in if !f { commit() } }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .overlay(Rectangle().strokeBorder(focused ? Palette.accent : Palette.border, lineWidth: Grid.edge))
                if let unit = spec.unit { Text(unit).font(.pixel(14)).foregroundStyle(Palette.muted) }
                if spec.acceptsAuto && value.string != "auto" {
                    Button("auto") { set(.string("auto")) }.buttonStyle(.pixel)
                }
            }
            // Under the number: what the value means (0 = off), and what is usual. Never the type's limit.
            if spec.kind == .int || spec.kind == .float {
                HStack(spacing: Grid.s) {
                    if let named = spec.specialLabel(value) {
                        Text("= \(named)").font(.caption.weight(.semibold)).foregroundStyle(Palette.text)
                    }
                    if let usually = spec.typicalText { Text(usually).font(.caption).foregroundStyle(Palette.muted) }
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
                if spec.scope == .request { Badge(text: "per message", color: Palette.accent2) }
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
        Button(title) { reset() }.font(.pixel(13)).foregroundStyle(Palette.accent).buttonStyle(.borderless)
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

/// The settings that are the app's own (docs/screens.md): serving the network with the key shown,
/// the metrics files, starting and stopping the engine. Never an engine parameter.
struct AppSettingsSection: View {
    @EnvironmentObject private var engine: Engine
    @EnvironmentObject private var preferences: Preferences
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            SectionTitle("This app")
            VStack(alignment: .leading, spacing: Grid.m) {
                Toggle(isOn: Binding(get: { preferences.shareOnNetwork }, set: {
                    preferences.shareOnNetwork = $0
                    engine.restart()
                })) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Serve on the network").foregroundStyle(Palette.text)
                        Text("Other devices on this network use the engine through its OpenAI-compatible endpoint, with the key, "
                            + "while this app is open. Prompts and answers travel in clear.")
                            .font(.caption).foregroundStyle(Palette.muted)
                    }
                }
                .tint(Palette.accent)
                if engine.shared { endpoint }
                Toggle(isOn: $preferences.metricsCSV) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Metrics CSV").foregroundStyle(Palette.text)
                        Text("A per-token file for every loaded model, in Metrics and in the app's folder.")
                            .font(.caption).foregroundStyle(Palette.muted)
                    }
                }
                .tint(Palette.accent)
                HStack(spacing: Grid.s) {
                    if engine.serverUp {
                        Button("Stop engine") { engine.stop() }.buttonStyle(.pixelDestructive)
                    } else {
                        Button("Start engine") { engine.start() }.buttonStyle(.pixelPrimary)
                    }
                    Spacer()
                    if let v = engine.info?.version { Text("engine \(v)").font(.pixel(13)).foregroundStyle(Palette.muted) }
                }
            }
            .card()
        }
    }

    private var endpoint: some View {
        let address = NetworkAddress.local()
        return VStack(alignment: .leading, spacing: 6) {
            Text("Base URL").font(.pixel(13)).foregroundStyle(Palette.muted)
            if let address {
                Text("http://\(address):\(EngineServer.port)/v1").font(.code).foregroundStyle(Palette.text).textSelection(.enabled)
            } else {
                Text("Not on a Wi-Fi or wired network: no other device can reach the engine until this one joins one.")
                    .font(.caption).foregroundStyle(Palette.err)
            }
            Text("Key").font(.pixel(13)).foregroundStyle(Palette.muted)
            Text(engine.networkKey).font(.code).foregroundStyle(Palette.text).textSelection(.enabled)
            Button(copied ? "Copied" : "Copy key") {
                UIPasteboard.general.string = engine.networkKey
                copied = true
            }
            .buttonStyle(.pixel)
        }
        .padding(12)
        .pixelBox(fill: Palette.bg, shadow: false)
    }
}
