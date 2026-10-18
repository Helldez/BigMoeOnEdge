# One experience, three native UIs: the five screens

Every UI of this project (the web UI the desktop serves, the Android app, the iOS app when it
comes) is a client of the same `bmoe-server` API ([server-api.md](server-api.md)) and shows the same
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
  `code`, never its wording. A full context (`context_full`) says so and offers New chat; a server
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
  (`--scan-dir`), so they are listed the same way and loaded in place.
- **The catalog.** The entries of `catalog/models.json`, as the server lists them: title,
  quantisation, size, one line on why you would pick it, and one action for its state:
  Download, Cancel (with one progress bar over the whole entry, shards included), On device
  (with Delete). The desktop server downloads for itself; the phone's app does the transfers,
  resumable, surviving the app being killed.
- **Any other model.** A direct gguf URL, or a file picked from the device and imported.
- **Loading.** With auto-plan on (the default where the planner is built in), Load unloads the
  previous model, measures the machine, applies the plan, then loads; the status line follows.
  A model this build does not stream loads with streaming off, and says so.

## Plan

What it is: what the hardware planner measured on this machine and what it decided for the
chosen model, with the fact behind each knob.

- **Auto-plan.** A switch: plan before every load. Off, a load uses the settings as they are.
- **The plan.** One line for the machine (cores, memory available and total, this model's matmul
  rate), the regime, whether streaming was declined and why, then one row per decision: the knob,
  its value, where it came from (`measured`, `derived`, `policy`, `operator` for a value the
  person set and the planner left alone, `unprobed`), and the reason in a sentence. A plan
  measured with a model loaded carries the server's warning: it is sized for a smaller machine.
- **Actions.** Measure (the probes run again: storage reads, a matmul on every device; on a phone
  it takes a while and never starts on its own), Apply (the plan's values go to the settings
  without becoming the person's), and the reproduce line, copyable, for a bug report.
- **With nothing loaded**, the decisions the configuration carries from the last applied plan are
  shown, so the screen is never empty after a plan has been made.

## Settings

What it is: every engine tunable, rendered from the parameter table, and the few settings that are
the UI's own.

- **From the table** (`GET /api/params`): the groups in the table's order, each row with its
  label, its help text, its control by type (a switch for `bool`, a menu for `choice`, a field
  with its unit for `int` and `float`, `auto` accepted where the row says so, the model's name for
  the one `path` a client may set), and the bounds the server validates. The basic rows of each
  group make the recommended configuration; the advanced, experimental and debug rows live in one
  section of their own, folded by default, grouped the same way and each marked with its level,
  with the caveat that they were measured once or on one device and can slow a run down or change
  the reply.
- **Where a value came from.** A value the person set is marked ("set by you") and can be reset
  to the server's default; a value a plan set is marked as the plan's. A row whose value changes
  the reply is marked as such. A row fixed while a model is loaded says "applies on reload", and a
  change to one raises a banner with Reload.
- **A configuration that does not validate** is kept (a form mid-edit is often briefly invalid),
  shown with the server's reason, and a load is refused until it does.
- **The UI's own**: on the phone, Serve on the network with the key shown and copyable, Metrics
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
