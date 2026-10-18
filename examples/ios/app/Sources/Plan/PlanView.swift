import BmoeKit
import SwiftUI

/// Plan (docs/screens.md): what the hardware planner measured on this device and what it decided
/// for the chosen model, with the fact behind each knob.
struct PlanView: View {
    @EnvironmentObject private var engine: Engine
    @State private var plan: PlanResult?
    @State private var measuring = false
    @State private var failure: String?
    @State private var copied = false

    var body: some View {
        Page {
            autoPlan
            if let failure { Banner(text: failure, color: Palette.err) }
            if let plan {
                result(plan)
            } else if let decisions = engine.config?.plan, !decisions.isEmpty {
                // With nothing measured yet, what the configuration carries from the last applied
                // plan, so the screen is never empty after a plan has been made.
                SectionTitle("Last applied plan")
                decisionList(decisions)
            } else {
                Text("Measure to see what this device can do with the model chosen in Settings, or the loaded one. "
                    + "The probes read storage and run a matmul on every device; on a phone that takes a while.")
                    .font(.subheadline).foregroundStyle(Palette.muted)
            }
        }
        // With a model loaded, its plan; never the probes on their own (the Android app does the same).
        .task(id: engine.info?.model?.path) {
            if engine.info?.model != nil { await show(measure: false) }
        }
    }

    private var autoPlan: some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            Toggle(isOn: Binding(get: { engine.config?.autoPlan ?? false },
                                 set: { on in Task { try? await engine.setAutoPlan(on) } }))
            {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Auto-plan").font(.pixel(16, bold: true)).foregroundStyle(Palette.text)
                    Text("Plan before every load. Off, a load uses the settings as they are.")
                        .font(.caption).foregroundStyle(Palette.muted)
                }
            }
            .tint(Palette.accent)
            .disabled(engine.info?.plannerAvailable != true)
            HStack(spacing: Grid.s) {
                Button(measuring ? "Measuring…" : "Measure") { Task { await show(measure: true) } }
                    .buttonStyle(.pixelPrimary)
                    .disabled(measuring || !engine.serverUp || engine.generating || engine.loading)
                if let plan, plan.available, plan.error.isEmpty {
                    Button("Apply") {
                        Task {
                            do { try await engine.applyPlan() } catch { failure = error.localizedDescription }
                        }
                    }
                    .buttonStyle(.pixel)
                }
                if measuring { ProgressView().tint(Palette.accent) }
            }
        }
        .card()
    }

    @ViewBuilder
    private func result(_ p: PlanResult) -> some View {
        if !p.available || !p.error.isEmpty {
            Banner(text: p.error.isEmpty ? "No plan." : p.error, color: Palette.muted)
        } else {
            if !p.warning.isEmpty { Banner(text: p.warning) }
            VStack(alignment: .leading, spacing: Grid.s) {
                SectionTitle(p.fromLastLoad ? "The loaded model's plan" : "The plan")
                Text(p.machine).font(.pixel(15)).foregroundStyle(Palette.text)
                HStack(spacing: Grid.s) {
                    Badge(text: p.regime, color: Palette.accent)
                    if p.streamingDeclined { Badge(text: "streaming declined", color: Palette.warn) }
                }
                if p.streamingDeclined && !p.declineReason.isEmpty {
                    Text(p.declineReason).font(.caption).foregroundStyle(Palette.muted)
                }
            }
            .card()
            decisionList(p.decisions)
            if !p.notApplicable.isEmpty {
                Banner(text: "Decided but not settable here: " + p.notApplicable.joined(separator: ", "), color: Palette.muted)
            }
            VStack(alignment: .leading, spacing: Grid.s) {
                SectionTitle("Reproduce")
                Text(p.reproduceLine).font(.code).foregroundStyle(Palette.text).textSelection(.enabled)
                Button(copied ? "Copied" : "Copy") {
                    UIPasteboard.general.string = p.reproduceLine
                    copied = true
                }
                .buttonStyle(.pixel)
            }
            .card()
        }
    }

    private func decisionList(_ decisions: [PlanDecision]) -> some View {
        VStack(alignment: .leading, spacing: Grid.s) {
            ForEach(decisions) { d in
                VStack(alignment: .leading, spacing: 4) {
                    HStack(alignment: .firstTextBaseline) {
                        Text(d.knob).font(.code).foregroundStyle(Palette.text)
                        Spacer()
                        Text(d.value).font(.pixel(16, bold: true)).foregroundStyle(Palette.accent)
                        Badge(text: d.source, color: sourceColor(d.source))
                    }
                    if !d.reason.isEmpty { Text(d.reason).font(.caption).foregroundStyle(Palette.muted) }
                }
                .card(padding: 12)
            }
        }
    }

    private func sourceColor(_ s: String) -> Color {
        switch s {
        case "measured": return Palette.ok
        case "operator": return Palette.warn
        case "unprobed": return Palette.err
        default: return Palette.muted
        }
    }

    private func show(measure: Bool) async {
        guard engine.serverUp else { return }
        measuring = measure
        failure = nil
        do {
            plan = try await engine.plan(measure: measure)
        } catch {
            failure = error.localizedDescription
        }
        measuring = false
    }
}
