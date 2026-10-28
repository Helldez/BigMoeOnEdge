# One experience, three native UIs: the screens

Every UI of this project (the web UI the desktop app and the server show, the Android app, the iOS
app) is a client of the same `bmoe-server` API ([server-api.md](server-api.md), the agent's routes in
[agent.md](agent.md)) and shows the same screens: conversations, Models, Plan and Settings (with
Metrics inside it once switched on), and a first-run setup; the desktop's conversations are the agent's, with Skills, Tasks and
Connect apps beside them, and the phone apps are chat apps
([below](#the-phone-apps-chat-not-the-agent)). This page says what each screen shows, what a
person can do on it, and what every state means, so the three toolkits render
the same thing their own way. It describes behaviour, not pixels: a phone puts the navigation in a
drawer and the prompt under a thumb, a desktop puts it in a sidebar and the prompt at the foot of a
wide column. Alike in what they show and in what a setting means; native in how they feel. How it
all looks is [design.md](design.md).

The rule behind every screen: **a UI knows no engine parameter by name.** Settings, the plan's
decisions, the metrics columns and the model list come from the API, and a knob added to the engine
appears in every UI with no UI change. The exceptions are the one the composer itself needs
(whether to think) and the handful of figures the live panel draws.

**Words.** Every word on a screen is plain: what a thing does, never a bare term of art. A figure
says what it measures ("Answer speed 12 tok/s"), with a tooltip, or a line under it on a phone,
that explains the unit ("a token is a piece of a word"). Every error says what to do next. The UIs
are in English; the web UI keeps its words in one dictionary, and a key the code names that the
dictionary lacks fails its tests.

## Layout

- **The sidebar** (on a phone, a drawer behind a menu button in the top bar): the mark and the name;
  New conversation; a search over conversation titles; the conversations the server holds, each
  workspace's together under its folder name, then the plain chats by date (today, yesterday, the
  previous 7 and 30 days, older), each row with its title, when it was last updated, and its state
  (working, needs you, stopped with an error) instead of the time while it is not idle; rename and
  delete on each row, delete asked twice; an entry for Tasks. At its foot, a card with the loaded
  model and the engine's state in words (it leads to Models), and the way to Models, Plan, Skills,
  Tasks, Connect apps and Settings. On the desktop it folds to a rail of icons, remembered.
- **The screen**, in the middle: the open conversation, or the screen picked in the sidebar, in a
  readable column.
- **The preview pane**, on the right beside a conversation, for a file the agent produced or
  changed (`GET /api/conversations/<id>/file`): a page or a drawing (HTML, SVG) runs in a sandboxed
  frame apart from the app; Markdown is rendered; code is highlighted; an image is shown; any other
  text is shown as text, cut past 512 KiB with its size said; anything else says what it is and its
  size. Refresh, full screen, close; its width is dragged (or set with the arrow keys) and
  remembered. Where there is no room beside the conversation (a phone, a narrow window) it covers
  the screen. Opening another conversation closes it.
- **Keyboard.** Every control is reached and used from the keyboard, with a visible focus ring;
  Ctrl or Cmd+K goes to the conversation search; Escape closes a menu, a dialog, the drawer or the
  full-screen preview. Reduced motion is honoured.

## The status line

Present on every screen: what the engine is doing, from `GET /api/info` and the `state` events. On
the desktop it is the model card at the foot of the sidebar (a dot and the state in words, the
model's name, a running download), with a banner over the screen for what asks for attention: an
unreachable server, a load under way or failed, the context warning below. On a phone it is the
line the toolkit gives it.

| State | Shown as | What the UI offers |
|---|---|---|
| server unreachable | "Engine not running" (phone: with the first line of why) | start it (phone), wait (desktop reconnects) |
| `empty` | "No model loaded" | the way to Models |
| any, a download running | "Downloading 42%" (or "2 downloads 42%"), beside the state | the way to Models |
| `loading` | "Loading model…" (with auto-plan: "Planning, then loading…" where the UI can tell) | nothing; a load is not cancelled |
| `ready` | the model's name, "ready" | everything |
| `ready`, `generating` | the model's name, "generating" | Stop |
| `error` | "Engine error", the message | the way to Models, to load again |

A model that loaded with streaming off (a dense model, or a plan that declined) is marked as such
wherever the model's name is shown: the live metrics then have no flash column, and the UI says why.

A loaded model whose context is below what a client of the server sends gets a warning beside its
name when the server says to (`model.ctx_advice.warn`): while the server serves the network, where
such clients are, and on any server once a request has been refused for the context
(`ctx_advice.refused`), since the client may show that refusal only as an unknown error. A server
only its own device reaches, with nothing refused, warns of nothing. The warning says what happened
or what will ("A request was refused: it did not fit the context of 4096 tokens" / "Coding agents
need a larger context: 16384 tokens, this model has 4096"), that a larger context takes memory from
the expert cache, and links the setting `ctx_advice.param` names, labelled from the parameter
table. A ✕ (a full-size touch target on a phone) dismisses it, remembered on the device for that
model and context; another model, another context or a first refusal brings it back. In the apps,
the same line stays under "Serve on the network", where the clients are switched on, dismissed or
not. Every UI does this.

## Conversation

What it is: a conversation with the agent, held by the server ([agent.md](agent.md)), so a run goes
on whether the screen is open or not and every UI shows the same one. A plain chat is a
conversation with no folder.

- **Messages.** Drawn from the server's items, kept current by the `conversation`, `item` and
  `delta` events; after a lost connection the open conversation is read again. The person's text
  verbatim, with Copy and Edit: editing a past question and sending it again removes it and
  everything after it (`/edit`), and the screen says so before. The model's answer as sanitised
  Markdown (no scripts, forms or styles from the model), code blocks highlighted with a Copy button;
  its reasoning folded above it ("Reasoning", open while it streams). Under a finished answer: Copy,
  Answer again on the last one (the last question sent again), and, on the local model, one line of
  figures from the turn's own summary, each rate named by its phase:
  `decode 1.7 tok/s (9 tok) · prefill 3.3 tok/s (13 tok, 4.0 s) · ctx 22/4096 · cache 54%`, with a
  tooltip saying what each means. Before the first word: "Reading your message…".
- **Tool calls.** One card each, with the tool's plain title ("Read src/main.cpp"), its state in
  words, the lines an edit added and removed, and on demand what it was given (the command, the
  address, the arguments) and what it answered, an edit as a diff whose `+` and `−` stay in the
  text. A file it produced or changed opens in the preview. Consecutive calls between two messages
  are one run, summarised in a line ("Read 3 files, edited 2 files · +14 −3") that is open while the
  run works and folded once it is done; a call alone is shown as itself.
- **Approvals.** A call that waits for the person is a card that says nothing has happened yet,
  shows what would run (the command, the diff, the text to write), and offers Allow and Deny. It is
  never folded away inside a run. In the desktop app a notification says so when the window is in
  the background.
- **Helpers and notices.** A helper the agent handed part of the job to is a row with its task, its
  state, its summary when done, and a way into its own conversation. A notice says in plain words
  why something did not happen.
- **The composer.** The text (Enter sends, Shift+Enter starts a line), and chips: the **agent**,
  where the server offers more than its own (a computer that runs programs; `GET /api/agent`): the
  built-in agent, or OpenCode, Claude Code, Codex or Pi, each saying whether it is installed and
  which program it needs when it is not; it is chosen before the first message and shown as a plain
  label after it, another agent needs a folder (the line under the composer says so), and with
  another agent the model chip is not shown, since that agent uses its own. Then the **folder** (none,
  one of the saved workspaces, or Add a folder: the system's folder dialog in the desktop app, a
  typed path elsewhere), the **mode** (Ask first, Read only, Full access, each with a line saying
  what it allows; Full access warns above the composer while a folder is set), the **model**
  (the local model, or a saved provider's model, saying that messages then go there), **Reasoning**
  where the local model has a switch for it, and **Choose** (below). Send, or Stop while a run
  works. Before the first message the chips are the new conversation's; after it they change the
  open one. With no model loaded and the local model picked, Send is disabled and the line under
  the composer says why and leads to Models.
- **Choose.** Where the server offers `POST /api/decide`: a mode in which the text is a question
  and a second field holds options, one per line; the model picks one without writing an answer,
  and each option comes back with the probability the model put on it, the picked one marked. A
  decision is not a conversation turn: it is shown under the conversation and not kept.
- **Errors.** Decided on by the error's `code`, never its wording, each with the next step: a run
  already going (`busy`: wait or Stop), no model (`no_model`: Models), a full context
  (`context_full`: New conversation, and the setting that raises the context from `ctx_advice`),
  something only the computer that runs the engine may do (`local_only`), a path that is not a
  folder (`not_a_folder`). The text of a message that was refused stays in the composer.
- **The live line.** While an answer streams on the local model and after: the answer speed, the
  shares of computing and of waiting for the disk, how often what a token needed was already in
  memory, each with a tooltip. The full card is in Metrics, when switched on.
- **Earlier conversations.** A UI that kept conversations on the device before the server held them
  sends them to the server once (`POST /api/conversations` with their messages), says how many
  moved, and removes its own copy of each once the server has it.

## First run

With no model on the device and none coming, the first screen is a guided setup instead of an empty
conversation: what this computer has, in plain words (memory, processor threads, the graphics the
plan saw); the graphics backend pack to install where one applies (from `GET /api/backends`, with
progress, then the restart the app does or the person does); the catalog's model that suits the
memory (the largest that fits in it, else the smallest, with a sentence saying which and why) and
the others beside it; its download with progress, resumable; then "Load it and start", which loads
it and opens a new conversation. Skip leads to an empty conversation; Models has everything else.

## Models

What it is: what is on this device, how to get a model onto it, and which one is loaded. One
list, a guided way to add to it, and nothing else.

- **Your models: one list.** What the server lists (`GET /api/models`): the models on the device
  (`local`) and the catalog's entries, merged by file, so a model the catalog downloaded is one row,
  not a file row and a catalog card. A row has one state and one primary action for it:

  | State | Shown with | Primary action |
  |---|---|---|
  | Available | size, quantisation, why you would pick it | Download |
  | Downloading | one progress bar over the whole entry, shards included | Cancel |
  | Partly downloaded | what is already here, of the whole | Resume (X of Y GB), Delete partial |
  | On this device | size, architecture, whether this build streams it | Load |
  | Loaded | the same | Reload, Unload, Go to the conversation |
  | Shards missing | which set, how many shards it should have | Download, where an entry has the rest |

  Beside it, Delete for a model in the server's models directory (never while loaded or
  downloading; a split set deletes as a unit, every file the server listed for it; asked twice), and
  Forget for an entry added from a repository or a link (its files stay). The other places a model
  can sit on the phone (adb-pushed, shared storage) are directories the app hands the server at
  launch (`--scan-dir`), listed the same way and loaded in place, with the folders directly inside
  each (a split set in a folder of its own); the server deletes nothing there, and the phone's app
  deletes what it may write. Catalog entries with nothing on the device and nothing started are
  suggestions, at the bottom of the same list, not a second grid. Files the server says are not
  models (`role`: a projector, a draft head) are listed apart, after the models, with what they are
  and no Load. Where the models directory is, and which scan directories are listed, sits behind a
  "where models live" disclosure.
- **Nothing here yet.** With no model on the device and none coming, the screen is three steps:
  1 Find a model, 2 Check it fits here, 3 Download, then Load; with the Hugging Face field inline,
  the catalog's suggestions as one-click Downloads and the direct link below. A first visit with
  nothing on the device opens on the first-run setup instead ([First run](#first-run)).
- **From Hugging Face: Find, Pick, Check, Download.** A model's name, or a repository id or its
  URL, in one field, and the steps shown as such, the current one marked (`docs/server-api.md`,
  "Models"). Find: a name searches the site and the repositories found are offered, the most
  downloaded first, a gated one marked, with the date it last changed; a repository is listed.
  Pick: every gguf it holds, split sets as one model, with quantisation, size and whether every
  shard is there, and the files whose names suggest a projector or a draft head folded apart after
  the models. Check: the model's header read through a range request before any download, and the
  verdict as a headline, the server's own line coloured by its code (`verdict.headline`,
  `verdict.fit`: fits here, runs without streaming, will not fit, not a model), then the facts:
  architecture, experts and top-k, layers, what stays resident and what streams, the disk needed
  against what is free, the resident floor against what this process may hold with the loaded model
  given back. Download: one button, the model becomes a catalog entry and its download starts; its
  row in Your models is scrolled into view, carries the progress, and turns to Load when it lands.
  A token field for a gated or private repository, used for the requests and kept nowhere. The UI
  shows what the server answered, reads no gguf and keeps no threshold of its own.
- **Any other model.** A direct https link to a gguf (or to any shard of a split set), which the
  server makes a catalog entry downloaded like the others (`POST /api/models/url`); on the phone,
  also a file picked from the device and imported.
- **Downloads everywhere.** A running download shows in the status line on every screen (how many,
  and the share done), and leads back here. The desktop server downloads for itself; the phone's
  app does the transfers, resumable, surviving the app being killed.
- **Loading.** With auto-plan on (the default where the planner is built in), Load unloads the
  previous model, measures the machine, applies the plan, then loads; the status line follows.
  A model this build does not stream loads with streaming off, and says so.

## Plan

What it is: what the hardware planner measured on this machine and what it decided for the
chosen model, with the fact behind each knob.

One layout for every machine. A phone with an NPU, a laptop with an integrated GPU, a desktop with
a discrete card and a server with only cores show the same blocks in the same order; what changes is
what the lists hold. Device names are shown as the plan reports them, and no block has a case for
a device, a system or a vendor: a badge's colour follows a device's role, a bar's colour a row's
position. Every word on the screen, down to a badge and a stage of the measurement, comes from the
server beside a code; a UI reads the code for a colour and shows the word, so the three UIs say
the same thing and a stage or role added later reaches them all unchanged.

- **Opening it measures nothing.** The plan lives in the UI's engine state, not in the screen:
  read (`GET /api/plan?cached=1`) when the server is reached, again when a session opens and
  again when a turn ends, whichever screen is open. So the screen opens on the plan the loaded
  model runs with, else the last one the server holds, marked as the last measured, and its
  measured figures land when the answer does; with none it says so, with Measure. Measuring is
  always the person's tap.
- **One banner for what went wrong**, first: a failed read, a failed Apply, or a plan the server
  could not make, with Measure again beside it. Then the stages of a running measurement.
- **Auto-plan.** A switch, above the plan: plan before every load. Off, a load uses the settings
  as they are.
- **Summary.** The plan's verdict (`summary.sentence`: "Experts stream from flash, with a
  7.1 GiB cache"), the machine line, the lanes (which device computes the prompt and which the
  tokens) in one line, and three figures: decode speed, expected from `predicted` and, once the
  loaded model has answered, measured from `measured`, with the difference (`+26%`) when the model
  runs this plan (`from_last_load`) and the expected figure as a plain line otherwise (the last plan
  measured by hand is not the one running); the cache hit rate the plan credited
  (`predicted.cache_hit_pct`, the cost model's own), measured with a mark at the credited one; and
  the memory held against what the process may hold (the host pool of `memory.pools`). The API's
  `predicted.s_per_token_worst`, the cost model's bound with nothing resident, is not drawn: it is
  in the diagnostic. Apply and Measure again sit together under the figures, the only place they
  are, and Apply lists what it changed, each row linked to its setting. A plan measured with a
  model loaded carries the server's warning: it is sized for a smaller machine.
- **Memory.** One bar per pool (`memory.pools`): every row the ledger holds, then what is free,
  against what the process may hold; each row named, sized and marked when locked, and coloured
  by its place from the series palette ([design.md](design.md)), so neighbouring parts stay apart. A row held in
  turn with the cache is a sentence under the bar, not a segment.
- **Devices.** One row per device the plan saw, the host first among equals: its rate on a prompt
  as a bar scaled to the fastest device measured, a badge for what it does (`Prompt`, `Tokens`,
  `Layers`, combined) or why it does nothing (`Not used`, `Wrong results`, `Unusable`), and the
  reason in a sentence. With no device beyond the host, one line says which of the three it is: the
  build carries no backend for one, the backends it carries found none here, or nobody looked.
- **Decisions, grouped.** The settings the plan decided (open), the ones it left (the person's
  own, or unmeasured), and the notes that are no setting: regime, placement, memory use and each
  of the cost model's notes under its own name, from the server's `label` (folded), each with a
  count. A row shows the knob's settings label, its value and where it came
  from (`measured`, `derived`, `policy`, `operator` for a value the person set and the planner left
  alone, `unprobed`); opening it shows the decision in one short line (`brief`), the full account
  behind Why, and a link that lands on the setting itself (`#/settings/<key>`: its card and fold
  open, the field focused). The two folded groups say in a line what they hold.
- **Measuring.** While the probes run (Measure, or an auto load), the stages from `plan_progress`:
  done, running, how many to go. Never a blind wait.
- **Actions.** Measure again (the probes run again: storage reads, a matmul on every device; on a
  phone it takes a while and never starts on its own), Apply (the plan's values go to the settings
  without becoming the person's), the command line, and Copy diagnostic: the command and the
  planner's whole account (`explain`) on the clipboard, for a bug report. The account is not shown:
  every line of it has a worded twin on the screen. Beside it, a link to how the planner decides
  ([hardware-planning.md](hardware-planning.md)). With nothing loaded and no plan to show, the
  decisions the settings carry from the last applied plan, grouped the same way.
- **With nothing loaded**, the decisions the configuration carries from the last applied plan are
  shown, grouped the same way, so the screen is never empty after a plan has been made.

## Skills

What it is: the instructions the agent follows when a job calls for them ([agent.md](agent.md#skills)).
One row per skill: its title, what it is for, where it comes from (Included, Yours, From a folder),
and a switch. A person's own skill can be written (name in kebab-case, when to use it, the
instructions), changed and deleted (asked twice); the others can be read. Empty, the screen says
what a skill is for and offers New skill.

## Tasks

What it is: a prompt that runs on a schedule as a conversation ([agent.md](agent.md#tasks)). One
row per task: its name, its folder and mode, its schedule in words ("Every weekday at 08:30",
"Every 2 hours", "Once, 3 Nov 2026, 09:00"), when it runs next, when it last ran with its outcome
and a way into that run's conversation, a switch to pause it, Run now, Edit and Delete. The editor:
name, what to do, when (Once with a date and time; Repeat every so many minutes, hours or days;
Every day at a time; Some days, picked, at a time), the folder, the mode (Read only by default, and
a line on what each mode means with nobody there to approve) and the model. The schedule is said
back in words as it is set, and a schedule that cannot run says what is missing. Runs update the
list live (`task` events).

## Connect apps

What it is: the coding agents and apps the server can serve, with the exact settings for this
server ([agent.md](agent.md#connecting-other-apps)). One card per client: how to connect it, the
settings file when there is one, the snippet with Copy, and, where the server may write the file,
Write settings, asked once, then the file written and the backup kept beside it.

## Settings

What it is: the engine tunables a person sets, rendered from the parameter table, and the settings
that are the UI's own. Four sections, each a tab (`#/settings/<section>`), and a fifth, Metrics,
once switched on:

- **General.** Appearance: the theme (system, light, dark), the text size (90 to 125%), and
  Metrics (off until switched on); all kept on the device. Inside the desktop app, the app's own
  preferences through its bridge ([desktop-app.md](desktop-app.md)): keep running in the tray when
  the window closes, notifications, the interface size, and the port (a new one restarts the
  engine).
- **Providers.** The saved online chat services ([agent.md](agent.md#providers)): name, address,
  model names, whether a key is saved. Add, edit and remove (asked once). The key field is
  write-only: it is never filled from the server, and left empty it keeps the saved key.
- **Graphics and updates.** The graphics backends this engine registered, the packs the release
  offers that apply to this machine (Install, with progress, then a restart: by the app, or by the
  person in a browser) and the others folded; and Check for updates, which reads the release feed
  only when pressed and, in the app, downloads the checked installer and installs it.
- **Engine.** The parameter table, below. A link to one parameter (`#/settings/<key>`) opens it here.

- **From the table** (`GET /api/params`): one card per group, in the table's order
  (`group_order`), titled with `group_label` and its one line of `group_help`. Each row has its
  label, its help text, its control by type (a switch for `bool`, a menu for `choice`, a field
  with its unit for `int` and `float`, `auto` accepted where the row says so). A group marked
  `group_read_only` (the model file, chosen on the Models screen) shows its value with no control.
  Under a number: its unit, the name of a special value when the field holds one ("= off"), and
  the typical range ("usually 2 to 8"), never the type's limit; the limits appear only when a value
  is refused. The help's first sentence is always shown, the rest and the special values on demand.
- **Only what a person sets.** A form shows the rows the table marks `form`: every basic row, and
  the few deeper ones a person picks rather than the plan (which devices, speculative drafts, the
  context's precision, the active experts). The rest stay flags of the command line, set by the
  plan; a row the person set anyway is shown, so it can be reset. No level filter and no search:
  every card starts closed, with its row count, and each row appears once.
- **Deep links.** `#/settings/<key>` lands on that row: its card opened, the field scrolled into
  view and focused. The Plan screen's decisions and the
  context warning link this way.
- **Where a value came from.** One chip per row: `default`, `from the plan`, or `set by you`. A
  value the person set can be reset; where the plan decided a different value, the row offers
  "Use plan value" instead, which removes the person's value so the plan's applies. "Reset all my
  values" removes them after a confirmation: `user_keys` less the server's launch flags
  (`launch_keys`) and less a read-only group's rows (the model a load chose), counted as such. A row whose value changes the reply
  is marked as such, a per-request row says "per message", and "needs reload" marks only a row
  whose value differs from the loaded session's (`reload_keys`); any such change raises a banner
  with Reload.
- **A configuration that does not validate** is kept (a form mid-edit is often briefly invalid),
  shown with the server's reason, and a load is refused until it does.
- **The UI's own** (besides General; inside the desktop app, Serve on the network is General's first
  card, the same card), on the phone: **Serve on the network** first, a card of its
  own whose closed header says whether it is on and at which address; open, the switch, each
  address on the local network to copy, the key (hidden until shown, copyable, replaceable with New
  key, which restarts the engine), the line a client needs (base URL and key) and a warning that
  the traffic is unencrypted. Then **This app**: Metrics and Stop/Start engine. Never an engine
  parameter.

## Metrics

What it is: what the engine measured about its own work, live and recorded: a section of Settings,
shown once the Metrics switch is on (off by default; on a phone the same switch records the per-token
CSV). A calm monitor: four
figures with their recent history (answer speed, disk reads, memory in use against what is free,
how often what a token needed was already in memory, with the cache's size), where a token's time
goes as one bar, then the last answer's account. With no model loaded it says so and leads to
Models; a model loaded without streaming says which figures it cannot have.

- **The live card.** For the current or last generation: the rate (the last token's while
  generating, the run's average after), and each token's wall time split into the four terms the
  engine measures or leaves unclaimed: compute, flash wait, cache management, unattributed
  ([telemetry.md](telemetry.md)); CPU busy share and major faults per token; the cache hit rate;
  after the run, the prefill rate, time to first token, the bytes streamed and the cache's
  resident size against its budget; under speculation, what the guesses bought. The figures come
  from the `token` and `done` events and nowhere else.
- **Records.** The desktop server writes its own per-token CSV when launched with one, which the
  web UI does not read yet; the phone writes one per loaded model from the same events, in the
  same shape. The phone's screen lists them,
  shows one with its configuration preamble and per-turn summaries, correlates columns of one
  file, compares one column of two files on one axis, and shares a file. A glossary names every
  column and every configuration key.

## What is deliberately not a screen

- **A login.** The server is a local program for one person; on the network it is one key.
- **A model editor.** The catalog is data in the repository; a model is a file.
- **An engine console.** What the engine prints goes to the server's log; the UI shows the
  state, the error and the figures, which is what a person acts on.

## The phone apps: chat, not the agent

The Android and iOS apps start their server with `--chat-only` ([agent.md](agent.md)): their
conversations are the server's, through the same routes and events, but plain chat. A small model on
a phone is not an agent; the agent is the desktop's. What that leaves out of the screens above, and
only that:

- **No agent.** No workspaces, mode, model route or providers in the composer; no tool calls,
  approvals, helpers or file preview in the transcript; no Skills, Tasks or Connect apps screens,
  and no notification of a run. The conversation list is by date, its rows marked while they are
  being answered or after an error.
- **Messages as above.** The person's text with Copy and Edit and send again (on a long press),
  the answer as Markdown with its reasoning folded above it, Copy under it and Answer again on the
  last one, the server's notices, Stop. Choose from options stays, in the composer.
- **The empty chat** says the model runs on the phone and leads to Models while none is loaded.
- **Serve on the network** (Settings) is the address and key a client of the phone needs; there is
  no client configuration to write on a phone.
- **The conversations the apps used to keep** are handed to the server once, on the first launch
  that reaches it, and the local copy is deleted only once the server's answer holds every message.
