import BmoeKit
import SwiftUI

/// A load in progress, as Models shows it: the seconds since it began, so a long load reads as
/// progress rather than as a hang, and while an auto load measures first, the plan's stages.
struct LoadStages: View {
    @EnvironmentObject private var engine: Engine

    var body: some View {
        if engine.loading {
            Running(planning: engine.info?.state == .planning, progress: engine.planProgress,
                    base: engine.loadBase ?? (0, Date()))
        }
    }

    /// The seconds are counted from what the server reported and when (the engine's), not from
    /// this view: leaving and coming back used to restart the count at zero.
    private struct Running: View {
        let planning: Bool
        let progress: PlanProgress?
        let base: (elapsed: Double, at: Date)

        var body: some View {
            VStack(alignment: .leading, spacing: Grid.m) {
                TimelineView(.periodic(from: base.at, by: 1)) { t in
                    HStack(spacing: 12) {
                        ProgressView().tint(Palette.accent)
                        Text((planning ? "Measuring this device, then loading" : "Loading the model")
                            + " · \(max(0, Int(base.elapsed + t.date.timeIntervalSince(base.at)))) s")
                            .font(.ui(15)).foregroundStyle(Palette.text).monospacedDigit()
                    }
                }
                if planning { PlanProgressCard(progress: progress) }
            }
        }
    }
}
