<script lang="ts">
  // Tasks (docs/agent.md, "Tasks"): a prompt that runs as a conversation on a schedule, in a folder
  // or none, read-only unless the person picks otherwise. Nobody is at the screen when it runs, so
  // anything that would ask is answered "no", and the run's conversation says what was not done.
  import { onMount } from 'svelte';
  import Dialog from '../components/Dialog.svelte';
  import Icon from '../components/Icon.svelte';
  import Segmented from '../components/settings/Segmented.svelte';
  import { ApiError } from '../lib/api';
  import { MODES, type Mode, type ScheduleKind, type Task, type TaskDraft, isTask } from '../lib/agent';
  import { agentApi } from '../lib/agentApi';
  import { describeSchedule, scheduleProblem, WEEK_ORDER, weekdayNames } from '../lib/schedule';
  import { relativeTime } from '../lib/when';
  import { agent } from '../stores/agent.svelte';
  import { prefs, t } from '../stores/prefs.svelte';
  import { server } from '../stores/server.svelte';

  let tasks = $state<Task[] | null>(null);
  let error = $state('');
  let actionError = $state('');
  let running = $state<Record<string, boolean>>({});
  let confirm = $state('');
  let now = $state(Date.now());

  // The editor's fields.
  let open = $state(false);
  let editing = $state<Task | null>(null);
  let title = $state('');
  let prompt = $state('');
  let kind = $state<ScheduleKind>('daily');
  let at = $state('09:00');
  let onceAt = $state('');
  let every = $state(60);
  let unit = $state<'minutes' | 'hours' | 'days'>('hours');
  let days = $state<number[]>([1, 2, 3, 4, 5]);
  let workspace = $state('');
  let mode = $state<Mode>('read_only');
  let route = $state('local');
  let enabled = $state(true);
  let formError = $state('');
  let saving = $state(false);

  const UNIT_MIN = { minutes: 1, hours: 60, days: 1440 };

  async function load() {
    try {
      tasks = await agentApi.tasks();
      error = '';
    } catch (e) {
      error = e instanceof ApiError && e.status === 404 ? t('tasks.unavailable') : e instanceof Error ? e.message : String(e);
    }
  }

  onMount(() => {
    load();
    const id = setInterval(() => (now = Date.now()), 30_000);
    const off = server.subscribe((ev) => {
      if (ev.event !== 'task') return;
      try {
        const task: unknown = JSON.parse(ev.data);
        if (isTask(task) && tasks) {
          const i = tasks.findIndex((x) => x.id === task.id);
          tasks = i < 0 ? [...tasks, task] : tasks.map((x) => (x.id === task.id ? task : x));
        }
      } catch {
        // not a task
      }
    });
    return () => {
      clearInterval(id);
      off();
    };
  });

  function start(task: Task | null) {
    editing = task;
    title = task?.title ?? '';
    prompt = task?.prompt ?? '';
    const s = task?.schedule;
    kind = (s?.kind as ScheduleKind) ?? 'daily';
    at = s && (s.kind === 'daily' || s.kind === 'weekly') ? (s.at ?? '09:00') : '09:00';
    onceAt = s?.kind === 'once' && s.at ? s.at.slice(0, 16) : '';
    const m = s?.every_minutes ?? 60;
    unit = m % 1440 === 0 ? 'days' : m % 60 === 0 ? 'hours' : 'minutes';
    every = m / UNIT_MIN[unit];
    days = s?.weekdays?.length ? [...s.weekdays] : [1, 2, 3, 4, 5];
    workspace = task?.workspace ?? '';
    mode = ((task?.mode as Mode) ?? 'read_only') || 'read_only';
    route = task?.route ?? 'local';
    enabled = task?.enabled ?? true;
    formError = '';
    open = true;
  }

  let schedule = $derived(
    kind === 'once'
      ? { kind, at: onceAt }
      : kind === 'interval'
        ? { kind, every_minutes: Math.round(Number(every) * UNIT_MIN[unit]) }
        : kind === 'daily'
          ? { kind, at }
          : { kind, at, weekdays: [...days].sort() },
  );
  let problem = $derived(scheduleProblem(schedule));
  let valid = $derived(title.trim() !== '' && prompt.trim() !== '' && problem === '');

  async function save(e: SubmitEvent) {
    e.preventDefault();
    if (!valid) return;
    saving = true;
    formError = '';
    const draft: TaskDraft = { title: title.trim(), prompt: prompt.trim(), workspace: workspace || null, mode, route, schedule, enabled };
    try {
      if (editing) await agentApi.updateTask(editing.id, draft);
      else await agentApi.createTask(draft);
      open = false;
      await load();
    } catch (err) {
      formError = err instanceof Error ? err.message : String(err);
    } finally {
      saving = false;
    }
  }

  async function runNow(task: Task) {
    running = { ...running, [task.id]: true };
    actionError = '';
    try {
      await agentApi.runTask(task.id);
      await load();
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
    } finally {
      running = { ...running, [task.id]: false };
    }
  }

  async function setEnabled(task: Task, on: boolean) {
    const { id: _id, last_run: _l, next_run: _n, ...draft } = task;
    tasks = (tasks ?? []).map((x) => (x.id === task.id ? { ...x, enabled: on } : x));
    try {
      await agentApi.updateTask(task.id, { ...draft, enabled: on });
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
      await load();
    }
  }

  async function remove(task: Task) {
    if (confirm !== task.id) {
      confirm = task.id;
      return;
    }
    confirm = '';
    try {
      await agentApi.deleteTask(task.id);
      await load();
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
    }
  }

  function toggleDay(d: number) {
    days = days.includes(d) ? days.filter((x) => x !== d) : [...days, d];
  }

  let dayNames = $derived(weekdayNames(prefs.lang, 'short'));
  let wsName = (id: string | null) => agent.workspaces.find((w) => w.id === id)?.name ?? '';
  let routes = $derived([
    { value: 'local', label: t('route.local') },
    ...agent.providers.flatMap((p) => p.models.map((m) => ({ value: `${p.id}/${m}`, label: `${p.name} · ${m}` }))),
  ]);
</script>

<div class="page-col">
  <div class="page-head">
    <h1>{t('nav.tasks')}</h1>
    <button type="button" class="primary" onclick={() => start(null)}><Icon name="plus" size={16} />{t('tasks.new')}</button>
    <p>{t('tasks.lead')}</p>
  </div>

  {#if error}<p class="banner error" role="alert">{error} <button type="button" class="small" onclick={load}>{t('action.retry')}</button></p>{/if}
  {#if actionError}<p class="banner error" role="alert">{actionError}</p>{/if}

  {#if tasks === null && !error}
    <div class="list">{#each [0, 1] as i (i)}<span class="skeleton row-skel"></span>{/each}</div>
  {:else if tasks && tasks.length === 0}
    <div class="empty-state card">
      <Icon name="clock" size={28} />
      <h2>{t('tasks.none')}</h2>
      <p>{t('tasks.none.help')}</p>
      <button type="button" class="primary" onclick={() => start(null)}>{t('tasks.new')}</button>
    </div>
  {:else if tasks}
    <ul class="list">
      {#each tasks as task (task.id)}
        <li class="card task" class:off={!task.enabled}>
          <div class="what">
            <div class="row wrap">
              <strong>{task.title}</strong>
              {#if task.workspace && wsName(task.workspace)}<span class="badge"><Icon name="folder" size={12} />{wsName(task.workspace)}</span>{/if}
              <span class="badge {task.mode === 'full' ? 'warn' : ''}">{t(`mode.${task.mode}`)}</span>
            </div>
            <p class="sched">{describeSchedule(task.schedule, t, prefs.lang)}</p>
            <p class="muted small figure">
              {#if task.enabled && task.next_run}{t('tasks.next', { when: relativeTime(task.next_run, now, prefs.lang) })}{:else if !task.enabled}{t('tasks.paused')}{/if}
              {#if task.last_run?.at}
                · {t('tasks.last', { when: relativeTime(task.last_run.at, now, prefs.lang) })}{task.last_run.outcome ? ` (${task.last_run.outcome})` : ''}
                {#if task.last_run.conversation}<a href="#/chat/{encodeURIComponent(task.last_run.conversation)}">{t('tasks.openRun')}</a>{/if}
              {/if}
            </p>
          </div>
          <div class="side">
            <label class="row small">
              <input type="checkbox" class="switch" role="switch" checked={task.enabled} onchange={(e) => setEnabled(task, e.currentTarget.checked)} />
              <span>{task.enabled ? t('tasks.on') : t('tasks.off')}</span>
            </label>
            <div class="row">
              <button type="button" class="small" disabled={running[task.id]} onclick={() => runNow(task)}><Icon name="play" size={14} />{t('tasks.runNow')}</button>
              {#if confirm === task.id}
                <button type="button" class="small danger" onclick={() => remove(task)}>{t('action.delete')}</button>
                <button type="button" class="small" onclick={() => (confirm = '')}>{t('action.keep')}</button>
              {:else}
                <button type="button" class="small" onclick={() => start(task)}>{t('action.edit')}</button>
                <button type="button" class="small ghost icon" aria-label="{t('action.delete')}: {task.title}" title={t('action.delete')} onclick={() => remove(task)}><Icon name="trash" size={15} /></button>
              {/if}
            </div>
          </div>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<Dialog bind:open wide title={editing ? t('tasks.edit') : t('tasks.new')}>
  <form id="task-form" onsubmit={save}>
    <div class="field-row">
      <label for="tk-title">{t('tasks.title')}</label>
      <input id="tk-title" type="text" bind:value={title} placeholder={t('tasks.title.placeholder')} />
    </div>
    <div class="field-row">
      <label for="tk-prompt">{t('tasks.prompt')}</label>
      <textarea id="tk-prompt" rows="5" bind:value={prompt} placeholder={t('tasks.prompt.placeholder')}></textarea>
    </div>
    <div class="field-row">
      <span class="label">{t('tasks.when')}</span>
      <Segmented label={t('tasks.when')} value={kind} options={(['once', 'interval', 'daily', 'weekly'] as const).map((k) => ({ value: k, label: t(`tasks.kind.${k}`) }))} onchange={(v: ScheduleKind) => (kind = v)} />
    </div>
    {#if kind === 'once'}
      <div class="field-row">
        <label for="tk-once">{t('tasks.date')}</label>
        <input id="tk-once" type="datetime-local" bind:value={onceAt} />
      </div>
    {:else if kind === 'interval'}
      <div class="field-row">
        <label for="tk-every">{t('tasks.every')}</label>
        <div class="row">
          <input id="tk-every" type="number" min="1" step="1" class="num" bind:value={every} />
          <select aria-label={t('tasks.unit')} bind:value={unit}>
            <option value="minutes">{t('tasks.unit.minutes')}</option>
            <option value="hours">{t('tasks.unit.hours')}</option>
            <option value="days">{t('tasks.unit.days')}</option>
          </select>
        </div>
      </div>
    {:else}
      {#if kind === 'weekly'}
        <div class="field-row">
          <span class="label">{t('tasks.days')}</span>
          <div class="days" role="group" aria-label={t('tasks.days')}>
            {#each WEEK_ORDER as d (d)}
              <button type="button" aria-pressed={days.includes(d)} onclick={() => toggleDay(d)}>{dayNames[d]}</button>
            {/each}
          </div>
        </div>
      {/if}
      <div class="field-row">
        <label for="tk-at">{t('tasks.time')}</label>
        <input id="tk-at" type="time" bind:value={at} class="time" />
      </div>
    {/if}
    <p class="summary" class:err-text={problem !== ''}>{problem ? t(problem) : describeSchedule(schedule, t, prefs.lang)}</p>
    <div class="grid">
      <div class="field-row">
        <label for="tk-ws">{t('workspace.label')}</label>
        <select id="tk-ws" bind:value={workspace}>
          <option value="">{t('workspace.none')}</option>
          {#each agent.workspaces as w (w.id)}<option value={w.id}>{w.name}</option>{/each}
        </select>
      </div>
      <div class="field-row">
        <label for="tk-mode">{t('mode.label')}</label>
        <select id="tk-mode" bind:value={mode}>
          {#each MODES as m (m)}<option value={m}>{t(`mode.${m}`)}</option>{/each}
        </select>
      </div>
      <div class="field-row">
        <label for="tk-route">{t('route.label')}</label>
        <select id="tk-route" bind:value={route}>
          {#each routes as r (r.value)}<option value={r.value}>{r.label}</option>{/each}
        </select>
      </div>
    </div>
    <p class="hint muted small">{t(`tasks.mode.${mode}`)}</p>
    {#if formError}<p class="err-text small" role="alert">{formError}</p>{/if}
  </form>
  {#snippet actions()}
    <button type="button" onclick={() => (open = false)}>{t('action.cancel')}</button>
    <button type="submit" form="task-form" class="primary" disabled={!valid || saving}>{t('action.save')}</button>
  {/snippet}
</Dialog>

<style>
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: var(--s2);
  }
  .row-skel {
    height: 5.5rem;
    border-radius: var(--radius);
  }
  .task {
    display: flex;
    gap: var(--s4);
    flex-wrap: wrap;
    align-items: flex-start;
  }
  .task.off .what {
    color: var(--muted);
  }
  .what {
    flex: 1 1 20rem;
    min-width: 0;
  }
  .what p {
    margin: var(--s1) 0 0;
  }
  .wrap {
    flex-wrap: wrap;
  }
  .sched {
    font-weight: 500;
  }
  .side {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: var(--s2);
  }
  .num {
    width: 6rem;
  }
  .time {
    width: 9rem;
  }
  .days {
    display: flex;
    flex-wrap: wrap;
    gap: var(--s1);
  }
  .days button {
    min-width: 3rem;
    border-radius: var(--pill);
    box-shadow: none;
  }
  .days button[aria-pressed='true'] {
    background: var(--accent-fill);
    border-color: var(--accent-fill);
    color: var(--on-accent);
  }
  .summary {
    margin: 0 0 var(--s4);
    font-weight: 550;
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(11rem, 1fr));
    gap: 0 var(--s3);
  }
  .hint {
    margin: 0 0 var(--s2);
  }
  .empty-state :global(svg) {
    color: var(--accent);
  }
</style>
