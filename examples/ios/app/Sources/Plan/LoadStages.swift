import BmoeKit
import SwiftUI

/// A load in progress, as Models shows it: the seconds since it began, so a long load reads as
/// progress rather than as a hang, and while an auto load measures first, the plan's stages.
struct LoadStages: View {
    @EnvironmentObject private var engine: Engine

    var body: some View {
        if engine.loading { Running(planning: engine.info?.state == .planning, progress: engine.planProgress) }
    }

    /// Exists only while the load runs, so its clock starts with each load.
    private struct Running: View {
        let planning: Bool
        let progress: PlanProgress?
        @State private var started = Date()

        var body: some View {
            VStack(alignment: .leading, spacing: Grid.m) {
                TimelineView(.periodic(from: started, by: 1)) { t in
                    HStack(spacing: 12) {
                        ProgressView().tint(Palette.accent)
                        Text((planning ? "Measuring this device, then loading" : "Loading the model")
                            + " · \(max(0, Int(t.date.timeIntervalSince(started)))) s")
                            .font(.subheadline).foregroundStyle(Palette.text).monospacedDigit()
                    }
                }
                if planning { PlanProgressCard(progress: progress) }
            }
        }
    }
}
