import BmoeKit
import SwiftUI

/// Settings (docs/screens.md): every engine tunable, rendered from the parameter table, and the few
/// settings that are the app's own. The basic rows of each group make the recommended
/// configuration; the rest live in one folded section, each marked with its level.
struct SettingsView: View {
    @EnvironmentObject private var engine: Engine
    @State private var advancedOpen = false
    @State private var failure: String?

    var body: some View {
        Page {
            if let c = engine.config {
                if !c.valid { Banner(text: "This configuration does not validate, and a load is refused until it does: \(c.error)", color: Palette.err) }
                if c.reloadRequired, engine.ready {
                    Banner(text: "Some changes apply when the model is loaded again.") {
                        Button("Reload") { reload() }.buttonStyle(.pixelPrimary)
                    }
                }
            }
            if let failure { Banner(text: failure, color: Palette.err) }
            if engine.params.isEmpty {
                Text(engine.serverUp ? "Reading the parameter table…" : "The engine is not running.")
                    .font(.subheadline).foregroundStyle(Palette.muted)
            }
            ForEach(groups(basic: true), id: \.label) { g in group(g.label, g.rows) }
            if !groups(basic: false).isEmpty { advanced }
            AppSettingsSection()
        }
    }

    private func groups(basic: Bool) -> [(label: String, rows: [ParamSpec])] {
        ParamSpec.grouped(engine.params.filter { ($0.level == .basic) == basic })
    }

    private func group(_ label: String, _ rows: [ParamSpec]) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            SectionTitle(label)
            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(rows.enumerated()), id: \.element.key) { i, spec in
                    if i > 0 { Rectangle().fill(Palette.border).frame(height: 1) }
                    ParamRow(spec: spec, onError: { failure = $0 })
                }
            }
            .card(padding: 12)
        }
    }

    private var advanced: some View {
        VStack(alignment: .leading, spacing: Grid.m) {
            Button {
                withAnimation { advancedOpen.toggle() }
            } label: {
                HStack {
                    Text("Advanced").font(.pixel(18, bold: true)).foregroundStyle(Palette.accent2)
                    Spacer()
                    Image(systemName: advancedOpen ? "chevron.up" : "chevron.down").foregroundStyle(Palette.muted)
                }
            }
            if advancedOpen {
                Banner(text: "Measured once or on one device: these can slow a run down or change the reply.")
                ForEach(groups(basic: false), id: \.label) { g in group(g.label, g.rows) }
            }
        }
    }

    /// The model loaded again with the configuration as it now stands.
    private func reload() {
        Task {
            do { try await engine.load(engine.info?.model?.path) } catch { failure = error.localizedDescription }
        }
    }
}

/// One parameter: its label, its help, its control by type, and where its value came from.
struct ParamRow: View {
    @EnvironmentObject private var engine: Engine
    let spec: ParamSpec
    var onError: (String) -> Void

    @State private var text = ""
    @State private var showHelp = false
    @FocusState private var focused: Bool

    private var value: JSON { engine.config?[spec.key] ?? .null }
    private var userSet: Bool { engine.config?.userKeys.contains(spec.key) == true }
    private var planSet: Bool { !userSet && engine.config?.plan?.contains { $0.knob == spec.key } == true }
    private var rejected: String? { engine.config?.rejected[spec.key] }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline, spacing: Grid.s) {
                Text(spec.label).font(.body).foregroundStyle(Palette.text)
                if spec.level != .basic { Badge(text: spec.level.rawValue, color: Palette.muted) }
                Spacer(minLength: Grid.s)
                if spec.kind == .bool { toggle }
            }
            if spec.kind != .bool { control }
            marks
            if showHelp && !spec.help.isEmpty {
                Text(spec.help).font(.caption).foregroundStyle(Palette.muted).fixedSize(horizontal: false, vertical: true)
            }
            if let rejected { Text(rejected).font(.caption).foregroundStyle(Palette.err) }
        }
        .padding(.vertical, 10)
        .contentShape(Rectangle())
        .onTapGesture { showHelp.toggle() }
        .onAppear { text = value.display }
        .onChange(of: value) { v in if !focused { text = v.display } }
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
            // The one path a client may set: the model, from what the server lists.
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
        }
    }

    private var marks: some View {
        HStack(spacing: Grid.s) {
            if userSet {
                Badge(text: "set by you", color: Palette.accent)
                Button("Reset") { reset() }.font(.pixel(13)).foregroundStyle(Palette.accent).buttonStyle(.borderless)
            } else if planSet {
                Badge(text: "set by the plan", color: Palette.ok)
            }
            if spec.lossy { Badge(text: "changes the reply", color: Palette.warn) }
            if spec.scope == .session, engine.info?.model != nil { Text("applies on reload").font(.caption).foregroundStyle(Palette.muted) }
            if let range = spec.rangeText { Text(range).font(.caption).foregroundStyle(Palette.muted) }
        }
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
        let base = "http://\(NetworkAddress.local() ?? "<this device>"):\(EngineServer.port)/v1"
        return VStack(alignment: .leading, spacing: 6) {
            Text("Base URL").font(.pixel(13)).foregroundStyle(Palette.muted)
            Text(base).font(.code).foregroundStyle(Palette.text).textSelection(.enabled)
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
