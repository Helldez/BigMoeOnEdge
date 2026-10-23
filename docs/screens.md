# One experience, three native UIs: the five screens

Every UI of this project (the web UI the desktop serves, the Android app, the iOS app) is a client of the same `bmoe-server` API ([server-api.md](server-api.md)) and shows the same
five screens. This page says what each screen shows, what a person can do on it, and what every
state means, so the three toolkits render the same thing their own way. It describes behaviour,
not layout: a phone puts the five in a bottom bar and the prompt under a thumb, a desktop puts them
in a top navigation and the prompt at the foot of a wide column. Alike in what they show and in
what a setting means; native in how they feel. How it all looks is [design.md](design.md).

The rule behind every screen: **a UI knows no engine parameter by name.** Settings, the plan's
decisions, the metrics columns and the model list come from the API, and a knob added to the engine
appears in every UI with no UI change. The exceptions are the two the chat itself needs (the
number of tokens to generate and whether to think) and the handful of figures the live panel draws.

## The status line

Present on every screen, or on every screen that acts on the engine: what the engine is doing, in
one line, from `GET /api/info` and the `state` events.

| State | Shown as | What the UI offers |
|---|---|---|
| server unreachable | "Engine not running" (phone: with the first line of why) | start it (phone), wait (desktop reconnects) |
| `empty` | "No model loaded" | the way to Models |
| `loading` | "Loading model…" (with auto-plan: "Planning, then loading…" where the UI can tell) | nothing; a load is not cancelled |
| `ready` | the model's name, "ready" | everything |
| `ready`, `generating` | the model's name, "generating" | Stop |
| `error` | "Engine error", the message | the way to Models, to load again |

A model that loaded with streaming off (a dense model, or a plan that declined) is marked as such
wherever the model's name is shown: the live metrics then have no flash column, and the UI says why.

A loaded model whose context is below what a client of the server sends (`model.ctx_advice.short`)
gets a warning beside its name: the context, the recommendation (`ctx_advice.recommended`, what a
coding agent's first request needs) and a link to the setting `ctx_advice.param` names, labelled
from the parameter table; a larger context takes memory from the expert cache, and the warning says
so. The web UI does this; the phone apps do not yet.

## Chat

What it is: a conversation with the loaded model, kept on the device, with the model's reasoning
visible and every answer's cost visible.

- **Conversations.** A list (drawer on the phone, side column on the desktop) of saved
  conversations, newest first, each with its title (the first line of the first message, cut to
  fit), when it was last answered, how many messages, which model. New chat, open, delete. The
  conversation is the client's: the server keeps none, every request carries it whole, and the
  engine reuses the leading tokens its KV cache already holds, so a follow-up costs its own
  message and switching conversations costs a full prefill.
- **Messages.** The person's text verbatim; the assistant's as rendered Markdown, with no scripts,
  forms or styles from the model. A reasoning model's thinking span sits folded above its answer
  ("Thinking", open while it streams, closed once committed). Under a finished answer, one line of
  figures from the turn's own summary, each rate named by its phase:
  `decode 1.7 tok/s (9 tok) · prefill 3.3 tok/s (13 tok, 4.0 s) · ctx 22/4096 · cache 54%`. The
  prefill counts the tokens this turn read, which with the conversation reused from the KV cache is
  the new message, not the history. Under speculation the real rate and the share of guesses kept
  follow.
- **Streaming.** The answer appears as it is generated. Before the first token: "Reading the
  prompt…". A chunk marked `reset` replaces the text shown so far instead of appending.
- **Actions.** Send (disabled with no model loaded, which the status line explains), Stop (the
  answer so far is kept), Copy an answer, Regenerate the last answer (the prompt is already in the
  KV, so only the answer is paid), New chat.
- **Errors.** Shown in the transcript, under the turn they belong to, and decided on by the error's
  `code`, never its wording. A full context (`context_full`) says so, with the context's size, and
  offers New chat and a link to the setting that raises the context (from `ctx_advice`); a server
  without a model (`no_model`) offers Models; anything else is dismissable. The person's message
  stays, saved with the conversation, and Send again sends it as it is after the fix. A decision
  that fails leaves no question behind: it is not a conversation turn.
- **The live line.** While an answer streams and after: the rate, the compute and flash shares,
  the cache hit rate, the temperature where the device reports one. The full card is in Metrics.
- **Choose.** Where the server offers `POST /api/decide`: a mode in which the prompt is a question
  and a second field holds options, one per line; the model picks one without writing an answer,
  and each option comes back with the probability the model put on it, the picked one marked. A
  decision is not a conversation turn.

## Models

What it is: what is on this device, how to get a model onto it, and which one is loaded.

- **On this device.** What the server lists (`GET /api/models`, `local`): each model with its
  size, architecture, whether this build streams it, whether a split set is complete, and the
  directory when it is not the server's own. Load, Unload, Delete (never while loaded; a split set
  deletes as a unit, every file the server listed for it). The other places a model can sit on the
  phone (adb-pushed, shared storage) are directories the app hands the server at launch
  (`--scan-dir`), so they are listed the same way and loaded in place, with the folders directly
  inside each (a split set in a folder of its own). Files the server says are not models
  (`role`: a projector, a draft head) are listed apart, after the models, with what they are and
  no Load.
- **From Hugging Face.** A model's name, or a repository id or its URL, in one field, and the
  steps the server does (`docs/server-api.md`, "Models"): Find (a name searches the site and the
  repositories found are offered, the most downloaded first, a gated one marked, with the date it
  last changed; a repository is listed: every gguf it holds, split sets as one
  model, with quantisation, size and whether every shard is there, and the files whose names
  suggest a projector or a draft head folded apart after the models), Check (the model's header
  read through a range request before any download: architecture, experts and top-k, layers,
  what stays resident and what streams, and the verdict on this machine: streams, plain mmap,
  not a model (a projector or a draft head, by its header, which cannot be added) or not
  readable, needs so much disk against what is free, the resident floor against what this
  process may hold with the loaded model given back), Add (the model becomes a catalog entry and its download starts). A token
  field for a gated or private repository, used for the requests and kept nowhere. The UI shows
  what the server answered and reads no gguf.
- **The catalog.** The two reference models of `catalog/models.json` and the entries added from
  a repository, as the server lists them: title, quantisation, size, one line on why you would
  pick it, and one action for its state: Download, Cancel (with one progress bar over the whole
  entry, shards included), On device (with Delete). An added entry can be removed; its files stay.
  The desktop server downloads for itself; the phone's app does the transfers, resumable,
  surviving the app being killed.
- **Any other model.** A direct gguf URL, or a file picked from the device and imported.
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

- **Opening it measures nothing.** The screen reads the plan the loaded model runs with, else the
  last one the server holds (`GET /api/plan?cached=1`), and shows it in this same layout, marked as
  the last measured; with none it says so, with Measure. Measuring is always the person's tap.
- **Auto-plan.** A switch: plan before every load. Off, a load uses the settings as they are.
- **Summary, first.** The plan's verdict (`summary.sentence`: "Experts stream from flash, with a
  7.1 GiB cache"), the machine line, the lanes (which device computes the prompt and which the
  tokens) in one line, and four figures: decode speed, expected from `predicted` and, once the
  loaded model has answered, measured from `measured` with the difference (`+26%`); the cache hit
  rate the plan credited (`predicted.cache_hit_pct`, the cost model's own), measured with a mark at
  the credited one; the memory held against what the process may hold (the host pool of
  `memory.pools`); and the worst case, milliseconds per token with nothing held in memory
  (`predicted.s_per_token_worst`), beside the expected. Apply and Measure again sit together under
  them, and Apply lists what it changed, each row linked to its setting. A plan measured with a
  model loaded carries the server's warning: it is sized for a smaller machine.
- **Memory.** One bar per pool (`memory.pools`): every row the ledger holds, then what is free,
  against what the process may hold; each row named, sized and marked when locked. A row held in
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
  every line of it has a worded twin on the screen. A link leads to how the planner decides
  ([hardware-planning.md](hardware-planning.md)).
- **With nothing loaded**, the decisions the configuration carries from the last applied plan are
  shown, grouped the same way, so the screen is never empty after a plan has been made.

## Settings

What it is: every engine tunable, rendered from the parameter table, and the few settings that are
the UI's own.

- **From the table** (`GET /api/params`): one card per group, in the table's order
  (`group_order`), titled with `group_label` and its one line of `group_help`. Each row has its
  label, its help text, its control by type (a switch for `bool`, a menu for `choice`, a field
  with its unit for `int` and `float`, `auto` accepted where the row says so). A group marked
  `group_read_only` (the model file, chosen on the Models screen) shows its value with no control.
  Under a number: its unit, the name of a special value when the field holds one ("= off"), and
  the typical range ("usually 2 to 8"), never the type's limit; the limits appear only when a value
  is refused. The help's first sentence is always shown, the rest and the special values on demand.
- **Levels, inside each group.** A card shows its basic rows open (the recommended configuration)
  and folds its own advanced, experimental and debug rows inside it, under one heading per level
  with what the level means; a card with no basic row starts folded. A filter at the top shows
  rows up to a level (Basic, Advanced, Experimental, Debug, each with its count; remembered on this
  device), and a card says how many rows the filter leaves out. A row that someone or the plan set
  is shown whatever the filter. Each row appears once.
- **Search.** A search field filters every level at once, by label, help and flag.
- **Deep links.** `#/settings/<key>` lands on that row: the filter widened to its level, its card
  and fold opened, the field scrolled into view and focused. The Plan screen's decisions and the
  context warning link this way.
- **Where a value came from.** One chip per row: `default`, `from the plan`, or `set by you`. A
  value the person set can be reset; where the plan decided a different value, the row offers
  "Use plan value" instead, which removes the person's value so the plan's applies. "Reset all my
  values" removes every one (`user_keys`) after a confirmation. A row whose value changes the reply
  is marked as such, a per-request row says "per message", and "needs reload" marks only a row
  whose value differs from the loaded session's (`reload_keys`); any such change raises a banner
  with Reload.
- **A configuration that does not validate** is kept (a form mid-edit is often briefly invalid),
  shown with the server's reason, and a load is refused until it does.
- **The UI's own**: on the phone, Serve on the network with the address on the local network and the key shown and copyable, Metrics
  CSV, Stop/Start engine; on the desktop, nothing yet (the key and the address are launch
  options). Never an engine parameter.

## Metrics

What it is: what the engine measured about its own work, live and recorded.

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
