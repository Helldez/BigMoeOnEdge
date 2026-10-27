import BmoeKit
import SwiftUI

/// Tasks (docs/agent.md, "Tasks"): a prompt that runs as a conversation on a schedule. The
/// scheduler is the server's, and the server runs inside this app, so the screen says plainly
/// when a task can run at all.
struct TasksView: View {
    @EnvironmentObject private var agent: AgentModel
    @EnvironmentObject private var nav: Navigator
    @State private var editing: AgentTask?
    @State private var failure: String?

    var body: some View {
        List {
            Section {
                Label {
                    Text("Tasks run only while this app's engine runs. iOS pauses the app soon after you leave it, so a task "
                        + "due while it is paused runs when you open the app again.")
                        .font(.ui(14)).foregroundStyle(Palette.text)
                } icon: { Image(systemName: "clock.badge.exclamationmark").foregroundStyle(Palette.warn) }
            }
            Section {
                ForEach(agent.tasks) { t in
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Text(t.title.isEmpty ? "Untitled task" : t.title).font(.ui(16, weight: .medium)).foregroundStyle(Palette.text)
                            Spacer()
                            Toggle("", isOn: Binding(get: { t.enabled }, set: { on in
                                var c = t
                                c.enabled = on
                                Task { await run { try await agent.saveTask(c) } }
                            }))
                            .labelsHidden()
                            .accessibilityLabel("Run \(t.title) on its schedule")
                        }
                        Text(ScheduleText.describe(t.schedule) + " · " + t.mode.label).font(.ui(13)).foregroundStyle(Palette.muted)
                        if let next = t.nextRun, t.enabled {
                            Text("Next: " + next.formatted(date: .abbreviated, time: .shortened)).font(.figure(12)).foregroundStyle(Palette.muted)
                        }
                        if let last = t.lastRun, let at = last.at {
                            Text("Last: " + at.formatted(.relative(presentation: .named)) + (last.outcome.isEmpty ? "" : " · \(last.outcome)"))
                                .font(.figure(12)).foregroundStyle(Palette.muted)
                        }
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { editing = t }
                    .swipeActions {
                        Button(role: .destructive) { Task { await run { try await agent.deleteTask(t) } } } label: {
                            Label("Delete", systemImage: "trash")
                        }
                        Button { Task { await run { try await agent.runTask(t) } } } label: { Label("Run now", systemImage: "play") }
                            .tint(Palette.accentInk)
                    }
                    .contextMenu {
                        Button { Task { await run { try await agent.runTask(t) } } } label: { Label("Run now", systemImage: "play") }
                        if let c = t.lastRun?.conversation, !c.isEmpty {
                            Button {
                                agent.open(c)
                                nav.open(.chat)
                            } label: { Label("Open the last run", systemImage: "bubble.left") }
                        }
                    }
                }
                Button { editing = AgentTask() } label: { Label("New task", systemImage: "plus") }
            } footer: {
                Text("Nobody is at the screen when a task runs: anything it would have to ask about is answered no, and its "
                    + "conversation says what was not done.").font(.ui(13))
            }
            if let failure { Section { Text(failure).font(.ui(14)).foregroundStyle(Palette.err) } }
        }
        .tint(Palette.accent)
        .scrollContentBackground(.hidden)
        .background(Palette.bg)
        .navigationTitle("Tasks")
        .navigationBarTitleDisplayMode(.inline)
        .task { await agent.refreshTasks() }
        .refreshable { await agent.refreshTasks() }
        .sheet(item: $editing) { TaskEditor(task: $0) }
    }

    private func run(_ body: () async throws -> Void) async {
        do {
            try await body()
            failure = nil
        } catch {
            failure = error.localizedDescription
        }
    }
}

/// A task's editor: the prompt, when, where, what it may do, on which model; Run now.
struct TaskEditor: View {
    @EnvironmentObject private var agent: AgentModel
    @Environment(\.dismiss) private var dismiss
    let task: AgentTask
    @State private var draft = AgentTask()
    @State private var date = Date()
    @State private var time = Date()
    @State private var failure: String?
    @State private var saving = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Title", text: $draft.title)
                    TextField("What the agent should do", text: $draft.prompt, axis: .vertical).lineLimit(3...10)
                }
                Section {
                    Picker("Runs", selection: $draft.schedule.kind) {
                        ForEach(TaskSchedule.Kind.allCases) { Text($0.label).tag($0) }
                    }
                    switch draft.schedule.kind {
                    case .once:
                        DatePicker("At", selection: $date, displayedComponents: [.date, .hourAndMinute])
                    case .interval:
                        Stepper(ScheduleText.describe(draft.schedule), value: $draft.schedule.everyMinutes, in: 5...(7 * 24 * 60),
                                step: draft.schedule.everyMinutes < 60 ? 5 : 30)
                    case .daily:
                        DatePicker("At", selection: $time, displayedComponents: .hourAndMinute)
                    case .weekly:
                        DatePicker("At", selection: $time, displayedComponents: .hourAndMinute)
                        weekdays
                    }
                } header: { Text("When") } footer: { Text(ScheduleText.describe(scheduled)) }
                Section {
                    Picker("Workspace", selection: $draft.workspace) {
                        Text("None").tag("")
                        ForEach(agent.workspaces) { Text($0.name).tag($0.id) }
                    }
                    Picker("May", selection: $draft.mode) {
                        ForEach(AgentMode.allCases) { Text($0.label).tag($0) }
                    }
                    Picker("Model", selection: $draft.route) {
                        Text("This phone").tag(AgentRoute.local)
                        ForEach(agent.providers) { p in ForEach(p.routes, id: \.route) { Text($0.label).tag($0.route) } }
                    }
                    Toggle("On its schedule", isOn: $draft.enabled)
                } footer: {
                    Text(draft.mode.help + (draft.mode == .ask ? " With nobody to ask, a task's questions are answered no." : ""))
                }
                if !task.id.isEmpty {
                    Section {
                        Button { Task { await save(thenRun: true) } } label: { Label("Save and run now", systemImage: "play.fill") }
                    }
                }
                if let failure { Section { Text(failure).foregroundStyle(Palette.err) } }
            }
            .font(.ui(16))
            .tint(Palette.accent)
            .navigationTitle(task.id.isEmpty ? "New task" : "Task")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { Task { await save(thenRun: false) } }
                        .disabled(saving || draft.prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
            }
        }
        .onAppear(perform: load)
    }

    private var weekdays: some View {
        HStack(spacing: 6) {
            ForEach(ScheduleText.weekOrder, id: \.self) { d in
                let on = draft.schedule.weekdays.contains(d)
                Button {
                    if on { draft.schedule.weekdays.removeAll { $0 == d } } else { draft.schedule.weekdays.append(d) }
                } label: {
                    Text(String(ScheduleText.dayNames[d].prefix(2)))
                        .font(.ui(13, weight: .medium))
                        .frame(maxWidth: .infinity, minHeight: 32)
                        .foregroundStyle(on ? Palette.onAccent : Palette.text)
                        .background(Circle().fill(on ? Palette.accentInk : Palette.surface2))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(ScheduleText.dayNames[d])
                .accessibilityAddTraits(on ? .isSelected : [])
            }
        }
    }

    /// The draft with its date and time written the way the schedule carries them.
    private var scheduled: TaskSchedule {
        var s = draft.schedule
        let c = Calendar.current
        switch s.kind {
        case .once:
            let d = c.dateComponents([.year, .month, .day, .hour, .minute], from: date)
            s.at = String(format: "%04d-%02d-%02dT%02d:%02d", d.year ?? 0, d.month ?? 0, d.day ?? 0, d.hour ?? 0, d.minute ?? 0)
        case .daily, .weekly:
            let d = c.dateComponents([.hour, .minute], from: time)
            s.at = String(format: "%02d:%02d", d.hour ?? 0, d.minute ?? 0)
        case .interval:
            break
        }
        return s
    }

    private func load() {
        draft = task
        if draft.schedule.everyMinutes <= 0 { draft.schedule.everyMinutes = 60 }
        let c = Calendar.current
        let now = Date()
        if task.schedule.kind == .once, let d = AgentTime.parse(task.schedule.at) { date = d } else {
            date = c.date(byAdding: .hour, value: 1, to: now) ?? now
        }
        let hm = task.schedule.at.split(separator: "T").last.map(String.init) ?? task.schedule.at
        let p = hm.split(separator: ":").compactMap { Int($0) }
        time = p.count >= 2 ? (c.date(bySettingHour: p[0], minute: p[1], second: 0, of: now) ?? now)
            : (c.date(bySettingHour: 9, minute: 0, second: 0, of: now) ?? now)
    }

    private func save(thenRun: Bool) async {
        saving = true
        defer { saving = false }
        var t = draft
        t.schedule = scheduled
        do {
            try await agent.saveTask(t)
            if thenRun { try await agent.runTask(t) }
            dismiss()
        } catch {
            failure = error.localizedDescription
        }
    }
}
