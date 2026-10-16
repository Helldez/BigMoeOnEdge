# bmoe-server HTTP API

`bmoe-server` is the engine behind a local web UI. It keeps one model loaded in one `Session`,
serves the UI's static files, and exposes a small JSON API plus an OpenAI-compatible chat
endpoint, so any OpenAI client can use it too.

It binds to `127.0.0.1` on port 8765 by default and, unless started with a key (`--api-key-file` or
`--api-key`), has no authentication:
it is a local program, like the CLI. See [Security model](#security-model) for exactly what that
does and does not protect.

It is also the application framework of this project: the same server, reached the same way,
stands behind every UI, on the desktop and on a phone ([application-framework.md](application-framework.md)).

All request and response bodies are JSON (UTF-8). Errors are `{"error": "<message>", "code": "<code>"}`
with a 4xx or 5xx status: `error` is for a person and may change, `code` is what a client decides
on, and is present wherever the failure has one. A chat or decide turn carries the engine's failure
kind (`context_full`, `empty_prompt`, `template_rejected`, `needs_chat_mode`, `invalid_request`,
`failed`, as in [telemetry.md](telemetry.md)) or the server's own refusal (`busy`, `no_model`,
`outside_models_dir`). A body that is not JSON, not a JSON object, or carries a field of the wrong
type (a `role` that is a number, an `id` that is an object) is a `400` with `code:
"invalid_request"`, never a `500`: the server's own faults are the only `500`s.
Inside a stream that had already answered `200`, the same pair arrives as one
`data: {"error": {"message": ..., "code": ...}}` chunk. Every `POST`, `PUT` and `DELETE` must carry `Content-Type: application/json`, body
or no body (`curl -X POST -H 'Content-Type: application/json' .../api/cancel`); one that does not
gets `415`.

## Security model

The server is a local program for one person, and it is protected as one. What it defends against
is the browser: other web pages open on the same machine. What it does not defend against is
other software, or other people, on that machine or on a network it was told to listen on.

**What is enforced**

- It binds to `127.0.0.1` unless `--host` says otherwise. There is no environment variable that
  widens it. A `--host` beyond loopback is refused at start unless a key is set (`--api-key-file`
  or `--api-key`) or `--no-api-key` says the open server is meant, and the server prints a warning
  in that case.
- With a key, every request to `/api/*` or `/v1/*` must carry `Authorization: Bearer KEY`,
  whatever its origin, or is refused with `401` and a `WWW-Authenticate: Bearer` header. The key is
  compared in constant time. The UI's own files are served without it: the UI asks for the key
  when it gets a `401`, keeps it in the browser, and sends it on every request, the event stream
  included. `--api-key-file PATH` reads the key from a file (trailing line breaks and blanks
  ignored; an empty file refuses to start), and is the way to pass one: a command line, and so
  `--api-key KEY`, is visible to every local user in the process list. `--api-key` stays for
  compatibility; the two together are refused.
- On loopback it answers only to loopback host names, so a DNS-rebinding page cannot reach it
  through a name of its own.
- A browser request to `/api/*` or `/v1/*` from anywhere but the UI's own page is refused with
  `403`, whatever the method: one that carries an `Origin` other than the server's own, or, with no
  `Origin`, one the browser marks with any `Sec-Fetch-Site` but `same-origin` or `none` (the person
  typed the URL). `same-site` is refused too: another port on the same host is the same site, and
  that is exactly where a hostile local page is served from. No CORS header is ever sent.
  `--allow-origin URL` admits one more origin, for a UI dev server.
- A request to `/api/*` or `/v1/*` that is not a `GET` must declare `Content-Type:
  application/json` (any case, a `; charset=` parameter allowed), whether or not it has a body;
  anything else is refused with `415` before it reaches a handler. An HTML form cannot send that
  type, so markup that ends up inside the UI's own page cannot post to the API.
- Model output is rendered as sanitised Markdown with no scripts, no form controls (`form`,
  `input`, `button`, `textarea`, `select`, `option`) and no styles (`style` blocks and
  attributes): an answer cannot carry something to click that submits, or restyle the page. It
  fetches nothing either: an image is kept only when its bytes are inline (`data:image/png`,
  `gif`, `jpeg`, `webp`) and is otherwise shown as its alt text, and media (`video`, `audio`,
  `picture`, `source`), `srcset`, `poster` and `background` are dropped. Text that talks the
  model into writing an image can no longer send what its URL carries anywhere, or have the
  browser GET one of the server's own endpoints.
- Every response carries `Content-Security-Policy: default-src 'self'; script-src 'self';
  style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'self'; media-src 'none';
  object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`, with
  `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer` and `X-Frame-Options: DENY`.
  The UI is one script and one stylesheet from its own origin with no inline script or style, so
  nothing needs `'unsafe-inline'`; whatever slips past the sanitiser still cannot load from
  another host, run inline, submit a form or put the page in a frame.
- `max_tokens` on a chat request is clamped to the `n-predict` parameter's upper bound, and one
  that is not a non-negative integer is refused with `400`.
- Downloads take a catalog id, never a URL or a path. `DELETE` on a download cancels it and
  removes no model.
- Over the API a client may name one file, the model, and it is only read. Parameters that make
  the engine write a file are set at launch and are refused over `PUT /api/config`.
- A client on the network (a connection whose peer address is not loopback) may name a model only
  in a folder the server lists models from: the models directory and each `--scan-dir`. The path is
  made canonical first, with every `..` and symbolic link resolved, a relative one taken inside the
  models directory, and anything that resolves elsewhere
  is refused: `403` with `code: "outside_models_dir"` on `POST /api/session/load`, a `rejected`
  entry on `PUT /api/config`. A local client is the person at the machine and may still open a
  model anywhere (the Android app reads models from shared storage), unless `--models-only`
  confines every client. Behind a reverse proxy on the same machine every request arrives from
  loopback, so start such a server with `--models-only`. The `-m` given at launch is the
  operator's and is not confined.
- Request bodies are capped at 8 MiB. Loads, unloads and plan measurements are serialised; a
  second measurement while one runs gets `409`.

**What is not**

- **Without a key, any local process, and any local user, can drive it.** A request without an
  `Origin` is a local program and is accepted; loopback is not per-user. On a phone every installed
  app is such a process, so the Android app never starts the server without one: a random key per
  launch, which only the app holds, or the shown key when it serves the network. On a shared machine another
  account can chat, read `/api/config` (which contains paths), and have the server open any file its
  user can read as a model (`--models-only` limits that to the folders models are listed from). With a key, a
  local program needs the key too, and whoever can read where a client keeps it (the browser's
  storage, a shell history, the key file if its permissions allow) has it.
- **`--host` with a non-loopback address removes the host-name check; the key is what stands in
  its place.** No TLS: prompts, answers and the key cross the network in clear, and anyone who can
  reach the port and has the key has everything in the previous point. Use it on a network you
  trust, or put a reverse proxy that terminates TLS in front.
- **Model output can still offer a link.** A link goes where it says, in a new tab with no
  referrer, once the person clicks it: text that talks the model into writing one can leak what its
  URL carries to whoever follows it.
- **Downloads run the system's `curl`**, found on `PATH`, against the URLs in the catalog file
  beside the executable. Both are trusted as installed.

## Concepts

- **Parameters** come from the engine's parameter table (`bmoe/params.h`). `GET /api/params`
  returns the schema; values travel as `{key: value}` objects, where a value has the JSON type the
  schema's `type` says (`bool`, `int`, `float`) or is a string (`path`, `text`, `choice`, and the literal
  `"auto"` for a parameter with `accepts_auto`).
- **Scope.** A `request`-scoped parameter (`n-predict`, `think`) applies to the next message. A
  `session`-scoped one is fixed while a model is loaded: changing it marks the config
  `reload_required` and takes effect on the next `POST /api/session/load`.
- **User-set keys.** The server remembers which keys the user set explicitly (`user_keys`).
  Everything else follows the server's defaults, so improving a default reaches existing users.
  The same set is what the planner treats as pinned: a value a person chose is never overruled.
- **One generation at a time.** A chat request that arrives while one runs waits its turn.

## Server and session state

### `GET /api/info`

```json
{
  "version": "0.24.0",
  "overlap_available": true,
  "planner_available": false,
  "host": {"ram_total_mib": 15820, "ram_available_mib": 9120, "cpu_threads": 16},
  "state": "ready",
  "error": "",
  "model": {"path": "C:/models/Qwen3-30B-A3B-Q4_K_M.gguf", "name": "Qwen3-30B-A3B-Q4_K_M.gguf",
            "arch": "qwen3moe", "n_ctx": 4096, "n_expert_used": 8, "think_ctl": "template",
            "load_s": 12.4, "streaming": true, "compute_threads": 8, "busy_threads": 12},
  "generating": false
}
```

`state` is one of `empty` (no model), `planning` (a load in auto mode is measuring the machine
before it loads: tens of seconds on a phone, nothing loaded meanwhile), `loading`, `ready`, `error`
(the last load failed; `error` says why). `model` is `null` unless `state` is `ready`. `streaming` is whether expert streaming is
on for the loaded session: with it off the engine is plain llama.cpp on mmap, and the UI must say so.
Whether a model will stream is only known once it has loaded (a dense model, a plan that declines),
so a UI says nothing about it before. `compute_threads` and `busy_threads` are what a CPU figure is
read against: the session's compute threads, and every thread busy in a decode (the compute threads
plus the read lanes when they run beside compute, under overlap). A UI that divides `cpu_ms` by
threads reads these, never the parameters.

### `POST /api/session/load`

Opens a session with the current config (unloading any loaded one first). Body: optional
`{"model": "<path>"}`, which sets the `model` parameter first. Returns `202` once the load is under
way; progress arrives as `state` events. `400` if the config does not validate (body carries the
error). With `auto_plan` on, the previous session is unloaded, the machine is measured and the plan
applied before the load starts, which adds the planner's probes (about 15 s) to the request. When
streaming ends up off (a dense model, or a plan that declines), every setting that only means
something on a stream (overlap, prefetchers, row streaming) is turned off with it.

### `POST /api/session/unload`

Cancels any generation and closes the session. `state` becomes `empty`.

### `POST /api/cancel`

Stops the running generation at the next decode boundary. Always `200`.

## Parameters and config

### `GET /api/params`

The parameter table as a JSON array, exactly `bmoe-cli --describe-params`, except that `default`
is the server's default (the server turns on streaming, the auto-sized cache and the chat
template). Each element:

```json
{"key": "cache-mb", "label": "Expert cache", "type": "int", "group": "cache",
 "group_label": "Expert cache", "level": "basic", "scope": "session", "help": "...",
 "flag": "--cache-mb", "min": 0, "max": 2147483647, "unit": "MiB", "accepts_auto": true,
 "default": "auto"}
```

Optional fields: `short`, `switches` (`[{flag, value, deprecated?}]`), `choices`
(`[{value, label}]`), `min`/`max` (present together), `unit`, `lossy`, `accepts_auto`.
`level` is `basic | advanced | experimental | debug`; `group` order in the array is form order.

### `GET /api/config`

```json
{
  "values": {"model": "...", "cache-mb": "auto", "threads": 8, "moe-stream": true},
  "user_keys": ["threads"],
  "args": ["--model", "...", "--threads", "8", "--moe-stream", "--cache-mb", "auto"],
  "valid": true,
  "error": "",
  "reload_required": false,
  "plan": null
}
```

`values` holds every parameter. `args` is the equivalent `bmoe-cli` command line (for "copy as
command"). `reload_required` is true when a session-scoped value differs from the loaded
session's. `plan` is the last applied plan's decisions (see below), or `null`. `auto_plan` is
whether every load plans first: on by default in a build with the planner, always false without
one; `PUT /api/config` with `{"auto_plan": false}` turns it off, and the choice is saved.

### `PUT /api/config`

Body: `{"values": {key: value, ...}, "reset": [key, ...]}`. Sets the given keys (they join
`user_keys`) and resets the listed keys to the server default (they leave `user_keys`), then
validates. Returns the same object as `GET /api/config`, plus `"rejected": {key: reason}` for
values that did not parse. A config that fails `validate()` is still stored (a form mid-edit is
often briefly invalid); `valid`/`error` say so, and `load` refuses it.

Persisted to `settings.json` in the data directory, so it survives a restart.

## Chat

### `POST /v1/chat/completions`

OpenAI-compatible, and tested against a coding agent (opencode) that drives it with tools.

Request fields that are read:

| field | meaning |
|---|---|
| `messages` | the whole conversation. `role` is `system` (or `developer`), `user`, `assistant` or `tool`; `content` is a string, an array of `{type: "text", text}` parts, or `null` on an assistant turn that only calls tools. An assistant message may carry `tool_calls` and `reasoning_content`; a tool message carries `tool_call_id`. The last message must be from the user or a tool. |
| `tools` | functions the model may call: `{type: "function", function: {name, description, parameters}}`. Any other tool type is refused with `400`. |
| `tool_choice` | `"auto"` (default), `"none"`, `"required"`. A named function is honoured as `"required"`: the engine can require a call, not choose which. |
| `stream`, `stream_options.include_usage` | streaming, and a last chunk with the token counts. |
| `max_tokens` / `max_completion_tokens` | default: the `n-predict` parameter (also for `null` and `0`). Clamped to that parameter's upper bound; a negative or non-integer value is refused with `400`. It is a ceiling, not a reservation: the answer gets at most what the context has left after the prompt and then stops with `finish_reason: "length"`. Only a conversation that leaves no room at all is refused, with `400`. |
| `think`, `chat_template_kwargs.enable_thinking`, `reasoning_effort` | thinking on or off, most specific first: this server's own `think` (bool), the template argument llama.cpp clients send, then `reasoning_effort`, of which only `"none"` turns it off. Default: the `think` parameter. |

Ignored, because the session fixes them: `model`, `temperature`, `top_p`, `seed` and every
other sampling field. Sampling is configured with the `temp`, `top-k`, `top-p` and `seed`
parameters. An image or audio content part is refused with `400` rather than dropped.

**Tools.** The server does not run tools and knows no tool syntax. The tools go to the model's own
chat template, which writes them the way that model was trained to read them, and the same
llama.cpp layer parses the model's calls back out of the generated text; the client runs them and
sends the results back as `tool` messages. A call's text never appears in `content`. The output is
not constrained by a grammar, so a model that writes a malformed call returns it as text.

**The conversation is the client's.** Every request carries the whole history and the server keeps
none. The engine tokenizes it, keeps the leading tokens that match what its KV cache already holds
and prefills only the rest: resending a conversation with one more message costs that message, an
edited or regenerated history prefills from its first differing token, and a different conversation
prefills in full. `usage.prompt_tokens_details.cached_tokens` says how much was reused. There is one
KV, so two conversations taking turns (an agent's session title beside its main thread) evict each
other.

**One generation at a time, in order.** A request that arrives while another runs waits its turn;
while it waits, a streaming response receives SSE comment lines (`: waiting for a turn`) so the
connection stays open. A client that disconnects gives its place up. With 16 requests already
waiting the next gets `429`.

Streaming response: `text/event-stream`, one `data: <chunk>` per token, then `data: [DONE]`.
Chunks are OpenAI `chat.completion.chunk` objects; the delta carries `content` and, for
reasoning models, `reasoning_content`. When the chat parser reclassifies already-sent text (a
closing reasoning tag), a chunk carries `"bmoe": {"reset": true}` and its delta holds the FULL
`content` / `reasoning_content` so far, which the client must use to replace, not append; a client
that does not know the extension appends it. Tool calls arrive in one delta after the text, each
whole (`index`, `id`, `type`, `function.name`, `function.arguments`), because a call is only known
once the turn has been parsed. The last choice chunk has `finish_reason` (`stop`, `length`,
`tool_calls`, or `cancelled`) and `"bmoe": {"summary": {...}}` with the run summary (same keys as
the `done` event). With `stream_options.include_usage`, one more chunk follows with empty `choices`
and `usage`.

Non-streaming response: a `chat.completion` object with `message.content` (`null` on a turn that
only calls tools), `message.reasoning_content`, `message.tool_calls`, `usage` and `bmoe.summary`.
`usage.prompt_tokens` is the whole prompt the answer was conditioned on.

Not implemented: `/v1/completions`, `/v1/embeddings`, `/v1/responses`, `response_format`, `stop`,
`n` above 1, and per-request sampling.

### `GET /v1/models`

`{"object": "list", "data": [{"id": "<model file name>", "object": "model", "owned_by": "local"}]}`
with the loaded model, or an empty list.

## Decide

### `POST /api/decide`

A decision: which of a list of choices the model would answer next, read from one prefill with no
decode ([decide.md](decide.md)). On a model streamed from flash that is the difference between
seconds and minutes. Body:

```json
{"prefix": "You are choosing the next action.\nTask: ...", "suffix": "\nScreen: ...\nAnswer with the letter.",
 "choices": ["A", "B", "C"], "reuse_prefix": true}
```

`prefix` + `suffix` is rendered as one user message through the model's own chat template, with
reasoning off; the split only says which part is worth keeping (`reuse_prefix`, default `true`:
the state after the prefix is kept and restored when the next prefix extends it). `choices` are
scored by their first token, so two sharing one are refused with `400`, as is an empty list or a
prompt past the context; `503` with no model loaded. The request waits its turn like a chat turn.

```json
{"cancelled": false, "best": 1, "choice_logp": [-3.2, -0.1, null], "n_tokens": 212, "n_reused": 180,
 "n_prefilled": 32, "restore_s": 0.02, "store_s": 0.01, "prefill_s": 0.9, "prefill_cpu_s": 3.1,
 "prefill_read_mib": 410.0, "prefill_io_s": 0.3, "prefill_stall_s": 0.1, "prefill_mgmt_s": 0.0,
 "prefill_dev_tokens": 0, "prefill_dev_read_mib": 0.0, "prefill_dev_stall_s": 0.0,
 "prefill_dev_routed": 0, "prefill_dev_demand": 0, "prefix_state_mib": 48.0}
```

`choice_logp[i]` is the log-probability of the first token of `choices[i]` over the whole
vocabulary, in request order (`null` where it is minus infinity), not renormalised over the
choices: the mass outside them says how unsure the model is. `best` is the index of the highest.
The keys are the CLI's `BMOE_DECIDE` keys; `POST /api/cancel` stops a decision between prefill
chunks, and the answer then carries `cancelled: true`. The `decide` parameter (on by default in
the server) is what lets a session accept these; off, every decision is refused.

## Live events

### `GET /api/events`

`text/event-stream`. Every event is `event: <name>` + `data: <json>`. A comment line (`: ping`)
is sent every 15 s. On connect the server sends one `state` event with the current info.

| event | data |
|---|---|
| `state` | the `GET /api/info` object, on every state change |
| `token` | per generated token: `step`, `steps`, `wall_ms`, `io_ms`, `compute_ms`, `mgmt_ms`, `stall_ms`, `flash_wait_ms`, `read_mib`, `cache_hit_pct` (-1 without a cache), `cache_budget_mib`, `rss_mib`, `mem_available_mib`, `majflt`, `cpu_ms`, `dense_resident_frac` (-1 unmeasured), `mtp_batch` |
| `done` | per generation, the CLI's `BMOE_DONE` keys (`docs/telemetry.md`): `cancelled`, `tokens`, `tok_s`, `load_s`, `prefill_s`, `prefill_tps`, `prefill_read_mib`, `prefill_dev_tokens`, `n_prompt`, `n_past`, `cache_hit_pct`, `read_mib`, `io_s_tok`, `compute_s_tok`, `stall_s_tok`, `flash_wait_s_tok`, `mgmt_s_tok`, `majflt_tok`, `cpu_s_tok`, `loop_overhead_s_tok`, `cache_resident_mib`, `cache_budget_mib`, `cache_evictions`, `cache_rereads`, `token_demand_mib`, `mtp_drafted`, `mtp_accepted`, `mtp_decodes`, `drafted_steps`, `mtp_draft_s_tok`, `ttft_s` |
| `decide` | per decision: the `POST /api/decide` answer |
| `download` | a download's progress: `id`, `file`, `received`, `total`, `state` (`running`, `done`, `error`, `cancelled`), `error` |
| `config` | the `GET /api/config` object, after any change |
| `log` | one engine diagnostic (`bmoe/log.h`): `level` (`info`, `warn`, `error`) and `message`, the line the server also writes to stderr after `bmoe: `. For a person to read; a client decides nothing on its wording |

`flash_wait_ms` (per token) and `flash_wait_s_tok` (the run's average) are the flash term that is a
share of the wall, already chosen for the run's mode: the blocking read (`io_*`) in serial, the stall
(`stall_*`) under overlap, where `io_*` is summed lane time and can exceed the wall
([telemetry.md](telemetry.md)). They are the term the engine subtracts for its compute residual, so
a UI shows "flash" from them without knowing whether the session overlaps.

## Models

### `GET /api/models`

```json
{
  "models_dir": "C:/Users/me/AppData/Local/BigMoeOnEdge/models",
  "scan_dirs": [],
  "local": [{"name": "Qwen3-30B-A3B-Q4_K_M.gguf", "path": "...", "dir": "C:/Users/me/AppData/Local/BigMoeOnEdge/models",
             "bytes": 18556686912, "shards": 1, "files": ["..."], "arch": "qwen3moe", "streamable": true,
             "complete": true}],
  "catalog": [{"id": "qwen3-30b-a3b-q4_k_m", "title": "Qwen3-30B-A3B", "quant": "Q4_K_M",
               "file": "Qwen3-30B-A3B-Q4_K_M.gguf", "bytes": 18556686912,
               "files": [{"name": "Qwen3-30B-A3B-Q4_K_M.gguf", "bytes": 18556686912, "url": "https://..."}],
               "blurb": "...", "status": "available", "note": ""}]
}
```

`local` lists every gguf in the models directory, then in each directory given with `--scan-dir
PATH` (repeatable; `scan_dirs` lists them), in that order: a split set once, by its first shard, with
`complete: false` while shards are missing. `dir` is the directory a model was found in, and `files`
every file of it that is present, in shard order, so a client that deletes a model deletes the set
and never has to know how shards are named. A scan directory is only read: downloads land in the
models directory, and a model in a scan directory is loaded in place by its `path`. The host app is
what knows where else models sit (the phone's adb-pushed and shared-storage folders); the server is
what says what they are. `streamable` is whether this build streams the
architecture. Catalog `status`: `on_disk`, `downloading`, `available`. `files` is every file of the
entry with its URL, in order, for a client that downloads for itself (a phone, where the server
has no `curl`); `file` and `bytes` are the first file's name and the whole entry's size.

### `POST /api/models/download`

Body `{"id": "<catalog id>"}`. Starts (or resumes) the download of every file of the entry into
the models directory. `202`; progress arrives as `download` events.

### `DELETE /api/models/download/<id>`

Cancels a running download. The partial file is kept so a later download resumes it.

## Plan

### `GET /api/plan`

`{"available": false, "reason": "..."}` in a build without the hardware planner, and
`{"available": true, "error": "..."}` when there is nothing to plan (no model selected).
Otherwise the planner's proposal for the configured model on this machine. It runs the planner's
probes (a second or two of storage reads and a bandwidth run, never the intrusive memory probe)
without loading the model; `409` while a generation runs, since that would skew the measurement.
Each compute device beyond the host is first asked in a process of its own (the server started
again with `--probe-device`), so a driver that ends the process it is used from marks the device
unusable instead of ending the server; `--no-isolated-probes` runs them in-process.

```json
{"available": true, "model": "C:/models/Qwen3.6-35B-A3B-Q4_K_M.gguf",
 "machine": "16 cores, 3388 MiB available of 15182 MiB, 33 GiB/s on this model's matmul",
 "regime": "experts-stream", "streaming_declined": false, "decline_reason": "",
 "decisions": [{"knob": "cache-mb", "value": "703", "source": "derived", "reason": "..."}],
 "values": {"cache-mb": 703, "threads": 8}, "args": ["--moe-stream", "--cache-mb", "703"],
 "not_applicable": [], "explain": "..."}
```

`source` is `measured | derived | policy | operator | unprobed`. Keys in `user_keys` come back as
`operator`: the planner never touches them. `values` are the parameters the plan changes, read
through the parameter table; `args` are the arguments that reproduce the plan, one per element
and unquoted, and empty when the plan declines streaming; `not_applicable` names anything the plan
decided that the table cannot hold (a tensor-placement pattern, say), so it is shown rather than
silently dropped.
With a model loaded that an auto load planned, `GET /api/plan` answers at once with that plan and
`"from_last_load": true`, without measuring. `GET /api/plan?measure=1` measures again regardless;
a plan computed while a model is loaded carries a `warning`, because that model's memory reads as
taken and the plan is sized for a smaller machine. Auto mode avoids this by planning with nothing
loaded.

### `POST /api/plan/apply`

Applies the current plan's `values` to the config without adding them to `user_keys`, and records
the decisions so `GET /api/config` returns them under `plan`. Returns the config object. A plan is
made from the configuration without the plan layer (defaults, launch flags, the person's values),
so its `values` are complete on their own and replace the previous plan whole.
