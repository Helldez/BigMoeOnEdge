# bmoe-server HTTP API

`bmoe-server` is the engine behind a local web UI. It keeps one model loaded in one `Session`,
serves the UI's static files, and exposes a small JSON API plus an OpenAI-compatible chat
endpoint, so any OpenAI client can use it too.

It binds to `127.0.0.1` by default and has no authentication: it is a local program, like the
CLI. Binding to another address is an explicit choice (`--host`), and the server says so at start.
On loopback it answers only to loopback host names (a DNS-rebinding page cannot reach it), and a
state-changing request carrying a browser `Origin` other than its own is refused with `403`
(`--allow-origin URL` admits one more, for a UI dev server).

All request and response bodies are JSON (UTF-8). Errors are `{"error": "<message>"}` with a 4xx
or 5xx status.

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
- **One generation at a time.** A second chat request while one runs gets `409`.

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
            "load_s": 12.4, "streaming": true},
  "generating": false
}
```

`state` is one of `empty` (no model), `loading`, `ready`, `error` (the last load failed; `error`
says why). `model` is `null` unless `state` is `ready`. `streaming` is whether expert streaming is
on for the loaded session: with it off the engine is plain llama.cpp on mmap, and the UI must say so.

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

OpenAI-compatible. Supported request fields: `messages` (`role` is `system`, `user` or
`assistant`; `content` is a string or an array of `{type: "text", text}` parts), `stream`,
`max_tokens` / `max_completion_tokens` (default: the `n-predict` parameter). Extensions: `think`
(bool; default: the `think` parameter) or the equivalent `chat_template_kwargs.enable_thinking`.

Ignored, because the session fixes them: `model`, `temperature`, `top_p`, `seed` and every
other sampling field. Sampling is configured with the `temp`, `top-k`, `top-p` and `seed`
parameters.

**Conversation continuity.** The engine keeps the conversation in its KV cache and is fed only
the new user message. When a request's `messages` are exactly the previous request's messages,
plus the assistant reply the server produced, plus one new user message, the server continues the
KV. Anything else (a new chat, an edited or regenerated history) starts fresh: a leading `system`
message is prepended to the first user message, and earlier turns that the engine cannot replay
are dropped; the response then carries `"bmoe": {"history_dropped": true}`.

Streaming response: `text/event-stream`, one `data: <chunk>` per token, then `data: [DONE]`.
Chunks are OpenAI `chat.completion.chunk` objects; the delta carries `content` and, for
reasoning models, `reasoning_content`. When the chat parser reclassifies already-sent text (a
closing reasoning tag), a chunk carries `"bmoe": {"reset": true}` and its delta holds the FULL
`content` / `reasoning_content` so far, which the client must use to replace, not append. The last
chunk before `[DONE]` has `finish_reason` (`stop`, `length`, or `cancelled`) and
`"bmoe": {"summary": {...}}` with the run summary (same keys as the `done` event).

Non-streaming response: a `chat.completion` object with `message.content`,
`message.reasoning_content`, `usage` and `bmoe.summary`.

### `GET /v1/models`

`{"object": "list", "data": [{"id": "<model file name>", "object": "model", "owned_by": "local"}]}`
with the loaded model, or an empty list.

## Live events

### `GET /api/events`

`text/event-stream`. Every event is `event: <name>` + `data: <json>`. A comment line (`: ping`)
is sent every 15 s. On connect the server sends one `state` event with the current info.

| event | data |
|---|---|
| `state` | the `GET /api/info` object, on every state change |
| `token` | per generated token: `step`, `steps`, `wall_ms`, `io_ms`, `compute_ms`, `mgmt_ms`, `stall_ms`, `read_mib`, `cache_hit_pct` (-1 without a cache), `cache_budget_mib`, `rss_mib`, `mem_available_mib`, `majflt`, `cpu_ms`, `dense_resident_frac` (-1 unmeasured), `mtp_batch` |
| `done` | per generation: `cancelled`, `tokens`, `tok_s`, `prefill_s`, `prefill_tps`, `n_prompt`, `n_past`, `cache_hit_pct`, `read_mib`, `io_s_tok`, `compute_s_tok`, `stall_s_tok`, `mgmt_s_tok`, `cache_resident_mib`, `cache_budget_mib`, `ttft_s` |
| `download` | a download's progress: `id`, `file`, `received`, `total`, `state` (`running`, `done`, `error`, `cancelled`), `error` |
| `config` | the `GET /api/config` object, after any change |

## Models

### `GET /api/models`

```json
{
  "models_dir": "C:/Users/me/AppData/Local/BigMoeOnEdge/models",
  "local": [{"name": "Qwen3-30B-A3B-Q4_K_M.gguf", "path": "...", "bytes": 18556686912,
             "shards": 1, "arch": "qwen3moe", "streamable": true, "complete": true}],
  "catalog": [{"id": "qwen3-30b-a3b-q4_k_m", "title": "Qwen3-30B-A3B", "quant": "Q4_K_M",
               "file": "Qwen3-30B-A3B-Q4_K_M.gguf", "bytes": 18556686912, "blurb": "...",
               "status": "available", "note": ""}]
}
```

`local` lists every gguf in the models directory (a split set once, by its first shard, with
`complete: false` while shards are missing). `streamable` is whether this build streams the
architecture. Catalog `status`: `on_disk`, `downloading`, `available`.

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
through the parameter table; `not_applicable` names anything the plan decided that the table
cannot hold (a tensor-placement pattern, say), so it is shown rather than silently dropped.
A plan computed while a model is loaded carries a `warning`: that model's memory reads as taken,
so the plan is sized for a smaller machine. Auto mode avoids this by planning with nothing loaded.

### `POST /api/plan/apply`

Applies the current plan's `values` to the config without adding them to `user_keys`, and records
the decisions so `GET /api/config` returns them under `plan`. Returns the config object.
