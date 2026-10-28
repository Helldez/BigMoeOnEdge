# bmoe-server HTTP API

`bmoe-server` is the engine behind a local web UI. It keeps one model loaded in one `Session`,
serves the UI's static files, and exposes a small JSON API plus an OpenAI-compatible chat
endpoint, so any OpenAI client can use it too, and the Anthropic Messages and OpenAI Responses
APIs that Claude Code and Codex CLI speak.

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
`invalid_output`, `failed`, as in [telemetry.md](telemetry.md)) or the server's own refusal (`busy`, `no_model`,
`outside_models_dir`, `streams_full`). A body that is not JSON, not a JSON object, or carries a field of the wrong
type (a `role` that is a number, an `id` that is an object) is a `400` with `code:
"invalid_request"`, never a `500`: the server's own faults are the only `500`s.
Inside a stream that had already answered `200`, the same pair arrives as one
`data: {"error": {"message": ..., "code": ...}}` chunk. Every `POST`, `PUT` and `DELETE` must carry `Content-Type: application/json`, body
or no body (`curl -X POST -H 'Content-Type: application/json' .../api/cancel`); one that does not
gets `415`. The agent protocols, `/v1/messages` and `/v1/responses`, answer every error in their
own protocol's shape instead ([Agent protocols](#agent-protocols-what-the-two-below-share)).

## Security model

The server is a local program for one person, and it is protected as one. What it defends against
is the browser: other web pages open on the same machine. What it does not defend against is
other software, or other people, on that machine or on a network it was told to listen on.

**What is enforced**

- It binds to `127.0.0.1` unless `--host` says otherwise. There is no environment variable that
  widens it. A `--host` beyond loopback is refused at start unless a key is set (`--api-key-file`
  or `--api-key`) or `--no-api-key` says the open server is meant, and the server prints a warning
  in that case.
- With a key, every request to `/api/*` or `/v1/*` must carry `Authorization: Bearer KEY` (or
  `X-Api-Key: KEY`, as Anthropic clients send it), whatever its origin, or is refused with `401` and a `WWW-Authenticate: Bearer` header. The key is
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
  object-src 'none'; frame-src 'self' blob:; base-uri 'none'; form-action 'none';
  frame-ancestors 'none'`, with `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`
  and `X-Frame-Options: DENY`. The UI is one script and one stylesheet from its own origin with no
  inline script or style, so nothing needs `'unsafe-inline'`; whatever slips past the sanitiser
  still cannot load from another host, run inline, submit a form or put the page in a frame. The
  page may frame two things only, both for the preview of a workspace file: this server's own
  responses and a `blob:` the page made from bytes it fetched.
- A workspace file served for that preview (`GET /api/conversations/<id>/file`, answered `200`) is
  the agent's work, not the UI, and may be a page with script of its own. It carries a policy of
  its own instead: `Content-Security-Policy: sandbox allow-scripts; default-src 'none'; script-src
  'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; font-src data:; media-src data:
  blob:; connect-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none';
  frame-ancestors 'self'` and `X-Frame-Options: SAMEORIGIN`, with the same `nosniff` and referrer
  rules. The `sandbox` directive gives the document an opaque origin however it is opened (framed
  by the UI with `sandbox="allow-scripts"` and no `allow-same-origin`, or opened directly), so its
  script runs but cannot read the UI, its storage or the key, cannot reach this server's API or any
  other host, cannot submit a form, and only the UI's own page may frame it. With a key set, the
  frame's own request could not carry it: the UI then fetches the bytes with the key and frames a
  `blob:` of them, where the UI's own policy holds and the page's inline script does not run.
- `max_tokens` on a chat request is clamped to the `n-predict` parameter's upper bound, and one
  that is not a non-negative integer is refused with `400`.
- Downloads take a catalog id, never a URL or a path. `DELETE` on a download cancels it and
  removes no model. Deleting a partial download (`DELETE /api/models/partial/<id>`) takes a catalog
  id too: the server deletes the `<name>.part` of each file that entry names, in the models
  directory only (never a scan directory), and refuses a file name that is not a plain name, so a
  client can name an entry but never a file. An entry added from a Hugging Face repository (`POST /api/hf/add`) is built by
  the server from a repository id and file names it validated (letters, digits, `.`, `_`, `-`,
  `+`, folders, no `..`), on the one endpoint it was started with (`--hf-endpoint`, the site or a
  mirror); a repository request never makes the server fetch a URL a client wrote. A token sent with those requests goes to
  that endpoint as a bearer token through a file only this process reads, never on a command line
  or in a log, and is kept nowhere. The one URL a client writes is a direct link to a gguf
  (`POST /api/models/url`), and only a local client may send one: it must be `https`, carry no
  user name or password, and end in a file name of letters, digits, `.`, `_`, `-`, `+`; curl is
  held to https on every redirect, and the link becomes a catalog entry downloaded like the others.
- Deleting a model (`DELETE /api/models/local/<name>`) is for a local client only, and takes the
  name the listing gives (`<folder>/<file>` or `<file>`), never a path: the server deletes the files
  its own scan of the models directory groups under that name, a split set whole. Never a file in a
  scan directory, never the model loaded or loading (`409`), never a file a running download is
  writing (`409`); a link is removed as the link, its target kept.
- Some routes answer only a client whose connection comes from this machine, whatever `--host`
  and the key say: they install software, read the network on the person's behalf, or change
  files. They are one list, `local_only_routes()` in `server/access.h`, enforced before routing
  (`403`, `code: "local_only"`): deleting a model, adding one from a link, and the backend-pack and
  update routes ([Backend packs and updates](#backend-packs-and-updates)).
- A server started by an app with `--parent-pid PID` stops when that process ends, so an app that
  crashes leaves no server holding the port and a model's memory.
- Over the API a client may name one file to open, the model, and it is only read (what it may delete is a model as the listing names it, above). Parameters that make
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
- No one client can take every worker. The server answers with 32 workers, and each request holds
  one for its whole life, so what holds one for long is bounded: at most 8 event streams
  (`GET /api/events`; the next gets `503` with `code: "streams_full"`), one running chat or decide
  turn and 16 waiting behind it (the next gets `503` with `code: "busy"`). That leaves 7 workers for
  everything else, the UI's own requests included. Connections that arrive while every worker is
  busy wait in a queue of 64; past that the server closes the next one at once.
- The agent's routes that reach this computer's files, processes or secrets answer only to a
  connection from loopback, whatever `--host` and whatever key: adding or removing a workspace, a
  message or an edit in a conversation that has a workspace or a provider route, a workspace
  file's preview, an approval, writing or switching a skill, creating, changing or running a task,
  saving a provider, writing a client's settings. A conversation with a workspace or a provider
  route is not listed, read, changed, deleted or stopped from the network, and its events (and
  every `task` event) reach loopback streams only; nor may the network give a conversation a
  workspace, a provider route or `full` access. From the network they get `403` with
  `code: "local_only"`. A message from the network to a conversation without a workspace runs, with
  no web tools, so a client elsewhere cannot have this machine fetch addresses on its network. The
  agent's file tools resolve every path inside the workspace (`..` and links resolved, dangling
  links refused); its commands run in the workspace through the platform's shell, as one process
  group killed at the time limit or a stop; provider keys never appear in a listing, a log or a
  command line ([agent.md](agent.md#who-may-use-it)).
- `--open` starts the system's opener (`open` on macOS, `xdg-open` elsewhere; the shell's own call
  on Windows) directly, with the URL as one argument: no shell is involved.

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
            "load_s": 12.4, "streaming": true, "compute_threads": 8, "busy_threads": 12,
            "ctx_advice": {"recommended": 16384, "short": true, "param": "ctx-size", "refused": false,
                           "warn": false}},
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

`ctx_advice` compares the session's context with what a client of the server sends: `recommended`
is the engine's `RunConfig::ctx_for_clients` (16384 tokens: a coding agent's first request, its
system prompt and tool schemas alone, measured at 9911 tokens for opencode, plus room for the
answer), `short` is whether `n_ctx` is below it, `param` is the parameter that raises it, `refused`
is whether a request has been refused with `context_full` since the load, and `warn` is `short`
on a server bound beyond loopback (`--host`, whose clients are other devices) or after a refusal:
a UI shows the warning only when `warn` is true, links the setting from these, and names neither
the number nor the key. A refusal publishes a `state` event, so an open UI learns of it at once. When
the context is short, the server also logs a `warn` line at load (on stderr and as a `log` event)
naming the flag that raises it, and a `warn` line every time a request is refused with
`context_full`, so the person at the server learns why even when the client hides the error.
See [Sizing the context for a client](#sizing-the-context-for-a-client).

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
 "group_label": "Expert cache", "group_help": "The memory that keeps experts once read, ...",
 "group_order": 5, "level": "basic", "scope": "session", "help": "...",
 "flag": "--cache-mb", "min": 0, "unit": "MiB", "typical": {"min": 1500, "max": 8192},
 "special": [{"value": 0, "label": "off"}], "accepts_auto": true, "default": "auto"}
```

Optional fields: `short`, `switches` (`[{flag, value, deprecated?}]`), `choices`
(`[{value, label}]`), `min` and `max`, `unit`, `typical`, `special`, `lossy`, `accepts_auto`.
`min` and `max` are limits: `max` is absent where the only upper limit is the type's, which is no
range to show a person. `typical` is guidance, not a limit: what a person usually sets, which a form
shows as the range. `special` names the values that mean something other than their number (`0` is
`off`, `4294967295` is `random per run`), with `value` in the row's own JSON type; a form shows the
label in place of the number.
`level` is `basic | advanced | experimental | debug`. A form shows the groups in `group_order` (a
row's place in the array is not its group's: a group's rows need not be contiguous), each under its
short `group_label` and its one-line `group_help`. `group_read_only: true` marks a group a client
sets with a control of its own (the model file, from the model list), which a settings form shows
without editing; the field is absent otherwise. The prompt's device and reads are their own group,
`prefill`.

### `GET /api/config`

```json
{
  "values": {"model": "...", "cache-mb": "auto", "threads": 8, "moe-stream": true},
  "user_keys": ["threads"],
  "launch_keys": [],
  "args": ["--model", "...", "--threads", "8", "--moe-stream", "--cache-mb", "auto"],
  "valid": true,
  "error": "",
  "reload_required": false,
  "reload_keys": [],
  "plan": null
}
```

`values` holds every parameter. `user_keys` are the operator's keys (the launch flags and the
values a client set); `launch_keys` are the ones of them the server was launched with, which a
client's "reset all my values" leaves alone. `args` is the equivalent `bmoe-cli` command line (for "copy as
command"). `reload_required` is true when a session-scoped value differs from the loaded
session's, and `reload_keys` names those parameters in table order (empty with no session). `plan` is the last applied plan's decisions (see below), or `null`. `auto_plan` is
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
| `think`, `chat_template_kwargs.enable_thinking`, `reasoning_effort` | thinking on or off, most specific first: this server's own `think` (bool), the template argument llama.cpp clients send, then `reasoning_effort`, where `"none"` turns it off and any other level turns it on. Default: the `think` parameter, off unless set. |
| `response_format` | `{"type":"json_object"}` requests JSON object output; `{"type":"json_schema","json_schema":{"schema":{...}}}` supplies a schema. Omitted, `null` or `{"type":"text"}` leaves the top-level `json_schema` unchanged; without one, output is ordinary text. See the support boundary below. |
| `json_schema` | llama.cpp-compatible top-level schema for this request. A JSON object supplies schema keywords; `null` or omission adds no constraint. An empty object defaults to `{"type":"object"}`. |

Ignored, because the session fixes them: `model`, `temperature`, `top_p`, `seed` and every
other sampling field. Sampling is configured with the `temp`, `top-k`, `top-p` and `seed`
parameters. An image or audio content part is refused with `400` rather than dropped.

**Tools.** The server does not run tools and knows no tool syntax. The tools go to the model's own
chat template, which writes them the way that model was trained to read them, and the same
llama.cpp layer parses the model's calls back out of the generated text; the client runs them and
sends the results back as `tool` messages. A call's text never appears in `content`. Once the model
opens a call, sampling is held to the grammar the template ships with its tools, as llama.cpp's own
server does: the call names a tool the request offered and its arguments are JSON. Without an
output schema, free text and the reasoning span are not constrained. On an
ordinary request, if the parser still finds no answer, reasoning or call in what was generated,
the raw text comes back as `content` and the server logs a warning, instead of an empty message
that ends with `stop`.

**Structured output.** Set `response_format` on the request, not in `/api/config`:

```json
{
  "messages": [{"role": "user", "content": "Return a JSON object describing Paris."}],
  "chat_template_kwargs": {"enable_thinking": false},
  "response_format": {"type": "json_object"}
}
```

For a schema, place it in `response_format.json_schema.schema`:

```json
{
  "messages": [{"role": "user", "content": "What is the capital of France?"}],
  "chat_template_kwargs": {"enable_thinking": false},
  "response_format": {
    "type": "json_schema",
    "json_schema": {
      "name": "capital",
      "schema": {
        "type": "object",
        "properties": {"city": {"type": "string"}},
        "required": ["city"],
        "additionalProperties": false
      }
    }
  }
}
```

The schema is passed to llama.cpp's chat template machinery, which builds the grammar and
parser together. `json_object` defaults to `{"type":"object"}`. For llama.cpp compatibility,
`json_object.schema` can supply a schema; otherwise a nonempty top-level `json_schema` is
retained. `json_schema` format takes its schema from the nested wrapper and overrides the
top-level field. If no schema is selected, or the selected schema is `null` or empty, either
response-format form defaults to `{"type":"object"}`. Schema values must be JSON objects;
boolean schemas are refused, but an object-valued schema may describe an array or scalar root. The wrapper's optional `name`
and `description` are strings, and `strict` is a boolean (`null` leaves these unset); they are
metadata and do not enable an additional validation pass. Unknown metadata is tolerated. Malformed known field types
and unsupported format types return `400` with `code: "invalid_request"`.

The pinned llama.cpp implementation supports a subset of JSON Schema. This endpoint does not
add a second, full JSON Schema validator after generation;
unsupported keywords do not gain a guarantee merely because they appear in the request.
JSON syntax and a schema cannot guarantee that the generated values are factually correct.

Thinking and speculative decoding use the existing template grammar path. Tool combinations
are template-dependent: the pinned Qwen3.5 specialized parser produces an invalid lazy grammar
for a response schema with offered tools and `tool_choice: "auto"`; that request returns `400`
with `code: "template_rejected"` before inference. The differential Qwen3 parser accepts this
combination and produces a JSON answer. Both tested parsers give JSON precedence over required
tool calls, so a response schema with `tool_choice: "required"` (including a named-function choice)
is explicitly refused with `400` / `invalid_request`, instead of silently ignoring the requirement.
These cases are covered by vendored-template regressions; tools are not universally disabled.
Reasoning is returned separately in `reasoning_content`. To request thinking
off, use `chat_template_kwargs.enable_thinking` as in the examples. The existing `think` alias
and precedence remain available. The output constraint belongs to one turn: a following
request without it generates normally without reloading the model.

A JSON answer that completes with `finish_reason: "stop"` must parse as JSON in
`message.content`. A malformed completed answer returns `invalid_output` and HTTP `500`, or
an error chunk if streaming has already started. The turn is rolled back and the model stays
loaded for the next request. This check is performed on the parsed answer, apart from reasoning
and tool-call data; a turn containing only tool calls is not required to contain a JSON answer.

A token limit (`length`), cancellation (`cancelled`) or disconnection can leave partial JSON;
partial output is not repaired by appending delimiters or relabelled as successful completion.
Individual SSE chunks need not be valid JSON. Reconstruct `delta.content` using the streaming
rules below, including `bmoe.reset`, and inspect the final finish reason. Clients must handle
an error event even after content has arrived. HTTP `200` or `[DONE]` alone is not a completeness
guarantee.

**The conversation is the client's.** Every request carries the whole history and the server keeps
none. The engine tokenizes it, keeps the leading tokens that match what its KV cache already holds
and prefills only the rest: resending a conversation with one more message costs that message, an
edited or regenerated history prefills from its first differing token, and a different conversation
prefills in full. `usage.prompt_tokens_details.cached_tokens` says how much was reused. There is one
KV, so two conversations taking turns (an agent's session title beside its main thread) evict each
other.

**One generation at a time, in order.** A request that arrives while another runs waits its turn;
while it waits, a streaming response receives SSE comment lines (`: waiting for a turn`) so the
connection stays open. A client that disconnects while it waits gives its place up and stops
nothing else; a streaming client that disconnects during its own answer stops that answer. A
disconnect never ends another client's turn. With 16 requests already waiting the next gets `429`.

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

Not implemented: `/v1/completions`, `/v1/embeddings`, `stop`, `n` above 1, and per-request
sampling.

### Sizing the context for a client

The server's default context is 4096 tokens: enough for a chat in the web UI, not for a coding
agent. An agent sends its system prompt and every tool schema with each request; opencode's first
request alone is 9911 tokens, so at 4096 every request it makes is refused with `400`
`context_full` before it has done anything, and some clients show that only as an unknown error.
Load the model with at least 16384 tokens (`-c 16384` on the server's command line, or the Context
setting) for an agent; a long agent session grows past that, and one six-step opencode run that
fixed two bugs fit in 32768. The cost is memory: the KV cache and the compute buffers are reserved
for the whole context, and they come out of the same RAM as the expert cache, so a larger context
leaves a smaller cache and a slower decode on a machine that is short of memory. The plan's
`kv-budget` note (`GET /api/plan`) says what the loaded context takes. `ctx_advice` in
`GET /api/info` and a `warn` log line at load say when the context is below the recommendation.

### `GET /v1/models`

`{"object": "list", "data": [{"id": "<model file name>", "object": "model", "owned_by": "local"}]}`
with the loaded model, or an empty list.

### Agent protocols: what the two below share

`POST /v1/messages` (Anthropic Messages, what Claude Code speaks) and `POST /v1/responses` (OpenAI
Responses, what Codex CLI speaks) are two more front doors on the same turn: each request becomes
the conversation, tools, budget and thinking switch `/v1/chat/completions` builds, and runs in the
same line, with the same context checks, cancel-on-disconnect and key. The README says how to point
each agent at the server. What they share:

- **Errors** come in the protocol's own shape, so the client can read them: on these paths every
  refusal, the access checks' included, is `{"type": "error", "error": {"type", "message"}}` for
  Messages and `{"error": {"message", "type", "param", "code"}}` for Responses. A full context is
  said the way each protocol says it, so an agent compacts its conversation instead of failing:
  Messages answers `invalid_request_error` with a message that starts `prompt is too long:`, and
  Responses has `code: "context_length_exceeded"`. Responses' other codes are this server's own
  (`no_model`, `busy`, `invalid_request`, ...). Inside a stream that already answered `200`, a
  failed turn ends with an `error` event (Messages) or `response.failed` (Responses).
- **System messages.** A chat template takes one system message, at the start, and most raise on
  any other. The leading system text (Messages' `system`, Responses' `instructions` and leading
  `system`/`developer` items) becomes one system message; a system message later in the
  conversation (Claude Code adds them, so does Codex) is passed as a user message in its place,
  which also leaves the prompt before it unchanged, so the KV is reused.
- **Streams are append-only.** Neither protocol can take back text it sent, so there is no `reset`
  as on the chat endpoint: the text and the reasoning are each sent only as far as the chat
  parser's view extends what was already sent, held back to the last whole UTF-8 character, and a
  snapshot that reclassifies earlier text is not sent until a later one extends it again. The
  parser holds back markup it cannot classify yet, so in practice the stream carries the whole
  answer; if a final answer does not extend what was sent, the client keeps what it holds. Tool
  calls are known whole once the turn is parsed, and arrive after the text, each in one piece.
  A reasoning model's thinking is sent as thinking (Messages) or reasoning items (Responses)
  whether or not the client asked to see it, so that the client returns it in the next request
  and the prompt still matches the KV.
- **Images and files** are refused with `400`, as on the chat endpoint, rather than dropped.
- **Ignored**: `model` (the loaded one answers), `temperature`, `top_p`, `top_k` and every other
  sampling field, as on the chat endpoint, and the fields that only mean something to a hosted
  service (prompt caching, metadata, `store`, `include`, `prompt_cache_key`, `parallel_tool_calls`).

### `POST /v1/messages`

The Anthropic Messages API. Request fields that are read:

| field | meaning |
|---|---|
| `system` | a string or an array of `{type: "text", text}` blocks, joined. The per-request `x-anthropic-billing-header:` block Claude Code puts first is client metadata that changes on every request, and is dropped, or no request could reuse the KV. |
| `messages` | `role` is `user`, `assistant` or `system`; `content` a string or blocks. `user`: `text` and `tool_result` (`tool_use_id`, content a string or `text` blocks; a `tool_reference` from Claude Code's tool search reads as `Tool loaded: <name>`). `assistant`: `text`, `thinking` (the reasoning, sent back), `redacted_thinking` (skipped) and `tool_use` (`id`, `name`, `input`). Any other block type is refused with `400`. The last message must be from the user or carry tool results: a prefilled assistant answer is refused, since the engine does not continue one. |
| `tools` | client tools, `{name, description, input_schema}`. A tool with `defer_loading: true` is not described to the model (tool search loads it later). A server tool (one with a `type` such as `web_search_20250305`) is refused with `400`: nothing here runs it. |
| `tool_choice` | `{type: "auto"}` (default), `"any"` or `"tool"` (both required: the engine can require a call, not choose which), `"none"`. |
| `max_tokens` | the budget, read as `max_tokens` on the chat endpoint. Required by the protocol; absent, the `n-predict` parameter. |
| `thinking` | `{type: "enabled"}` or `"adaptive"` turns thinking on, `"disabled"` off; absent, the `think` parameter. `budget_tokens` and `display` are not read. |
| `output_config.format` | `{type: "json_schema", schema}`: the answer must match the schema, as `response_format` on the chat endpoint. |
| `stream` | stream the answer as events. |

Also accepted and not read: `stop_sequences` (the engine has no stop strings), `metadata`,
`cache_control`, `context_management`, `output_config.effort`.

Non-streaming response: a `message` with `content` blocks in order, a `thinking` block (with an
empty `signature`: a local model has none to give), a `text` block, then a `tool_use` block per call
with its `input` as an object; `stop_reason` `end_turn`, `max_tokens` or `tool_use` (a cancelled
turn is `end_turn`); `usage` with `input_tokens` (the part of the prompt prefilled for this turn),
`cache_read_input_tokens` (the part the KV already held) and `output_tokens`; and `bmoe.summary`.

Streaming response: `event: message_start` (usage zero: the counts are known once the turn has
run), then content blocks, each `content_block_start` / `content_block_delta` / `content_block_stop`:
thinking (`thinking_delta`, closed by an empty `signature_delta`), text (`text_delta`), and one
`tool_use` block per call with its whole input in one `input_json_delta`. Then `message_delta` with
the `stop_reason` and the full `usage` (and `bmoe.summary`), and `message_stop`. While the request
waits its place in line, `event: ping`.

### `POST /v1/messages/count_tokens`

The same body as `/v1/messages`, answered with `{"input_tokens": N}`. N is an estimate from the
request's length (about 3 bytes of text per token, on the high side for an agent's prompt of code
and JSON schemas), not a count: the engine exposes no tokenizer outside a turn. It needs no loaded
model.

### `POST /v1/responses`

The OpenAI Responses API, stateless: every request carries its whole conversation in `input`, as
Codex sends it with `store: false`. Nothing is stored, so `previous_response_id`, `conversation`,
an `item_reference` and `background: true` are refused with `400` rather than answered without the
state they name; `store` itself is accepted and not read. Request fields that are read:

| field | meaning |
|---|---|
| `instructions` | the system prompt. |
| `input` | a string (one user message) or items: `message` (`role` `user`, `system`, `developer` or `assistant`; content a string or `input_text` / `output_text` / `refusal` parts), `function_call` (`call_id`, `name`, `arguments`), `function_call_output` (`call_id`, `output` a string or `input_text` parts) and `reasoning` (its `content` text; a summary or an encrypted content is not the reasoning, and is not read). The items of one model turn (reasoning, text, calls) become one assistant message. An image or file part, or another item type, is refused with `400`. The last item must be from the user or a `function_call_output`. |
| `tools` | `{type: "function", name, description, parameters}` tools are described to the model. Every other tool type (`web_search`, `namespace`, `custom`, `local_shell`, ...) is dropped, not refused: nothing here runs it, and Codex sends some whatever the provider has. |
| `tool_choice` | `"auto"` (default), `"none"`, `"required"`, or `{type: "function", name}`, honoured as required. |
| `max_output_tokens` | the budget, read as `max_tokens` on the chat endpoint. |
| `reasoning.effort` | `"none"` turns thinking off, any other level on; absent, the `think` parameter. |
| `text.format` | `{type: "json_schema", schema}` or `{type: "json_object"}` constrains the answer, as `response_format` on the chat endpoint. |
| `stream` | stream the answer as events. |

Non-streaming response: a `response` object with `status` `completed`, or `incomplete` with
`incomplete_details.reason` `max_output_tokens` (or `cancelled`); `output` items in order, a
`reasoning` item (`content: [{type: "reasoning_text", text}]`), a `message` item
(`content: [{type: "output_text", text}]`) and a `function_call` item per call (`call_id`, `name`,
`arguments`); `usage` with `input_tokens` (the whole prompt), `input_tokens_details.cached_tokens`
(the part the KV already held), `output_tokens`, `total_tokens`; and `bmoe.summary`.

Streaming response: each event named in `event:` and in its data's `type`, numbered by
`sequence_number`. `response.created` and `response.in_progress`; then per item
`response.output_item.added`, its deltas (`response.reasoning_text.delta` for reasoning;
`response.content_part.added`, `response.output_text.delta`, `.done` and
`response.content_part.done` for a message; `response.function_call_arguments.delta` and `.done`,
the whole arguments in one delta, for a call) and `response.output_item.done` with the item whole;
and last `response.completed` (or `response.incomplete`) with the response object, its `output`
and `usage`. While the request waits its place in line, an SSE comment.

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

## Agent

The agent's conversations, workspaces, skills, tasks, providers and client settings
([agent.md](agent.md), which is the contract: concepts, modes, tools, item and event shapes, the
route table). What follows is what a client needs beyond it on the wire.

**State.** The server keeps it in `<data>/agent/` (`conversations/<id>.json`, `workspaces.json`,
`tasks.json`, `providers.json`, `skills.json`), each written to a temporary file and renamed, and
provider keys in `<data>/credentials.json`, created readable by its owner only. A person's skills
are folders in `<data>/skills/`; the bundled ones are `skills/` beside the binary (`--skills-dir`
names another folder). The client catalog is `clients.json` beside the model catalog.

**Runs.** `POST /api/conversations/<id>/messages` answers `202 {item}` as soon as the person's item
is stored; everything after that arrives on `/api/events` (`conversation`, `item`, `delta`) and is
in the stored conversation. A UI that reconnects reads `GET /api/conversations/<id>`. An approval
card is a `tool` item in state `waiting`, answered with `POST .../approval` on the conversation (or
the helper's conversation) it appears in. `POST .../stop` answers once the run has ended.

**Errors.** `{"error", "code"}`, with these codes:

| status | `code` | when |
|---|---|---|
| 400 | `invalid_request` | a field of the wrong type or value (a mode, a route naming no saved provider, a schedule that cannot run, an imported item that is not `user` / `assistant`) |
| 400 | `not_a_folder` | `POST /api/workspaces` with a path that is not an absolute, existing folder |
| 400 | `unsupported` | a message with `images` |
| 403 | `local_only` | a route that reaches this computer, from a client not on loopback (below) |
| 403 | `outside_workspace` | a preview path that resolves outside the workspace |
| 404 | `not_found` | no such conversation, workspace, skill, task, provider or client; an approval for an item that is not waiting |
| 409 | `busy` | a message, an edit, a delete, or a change of workspace or route while the conversation runs; a task run while it runs; a message to a helper's conversation |
| 409 | `exists` | `POST /api/skills` with the name of one of the person's skills |
| 409 | `conflict` | `POST /api/connect/<id>` with no model loaded, or a settings file the server will not overwrite (JSON that does not parse, an unclosed block) |
| 413 | `too_large` | a preview of a file over 16 MiB |
| 503 | `stopping` | a message while the server stops |

A run's own failures are not HTTP errors: they arrive as `notice` items (no model loaded, a context
that no longer fits, a provider that answered with an error) and leave the conversation in state
`error`.

## Live events

### `GET /api/events`

`text/event-stream`. Every event is `event: <name>` + `data: <json>`. A comment line (`: ping`)
is sent every 15 s. On connect the server sends one `state` event with the current info.

At most 8 streams are open at once, since each holds one of the server's workers for as long as it
is open: one more gets `503` with `code: "streams_full"` and should retry later, as the UI and the
apps do (with back-off). A client that closes its connection gives its place back within a second,
so a reloaded page finds it free; one that keeps the connection open but stops reading is let go
when a ping cannot be written (the server's write timeout, 5 s, once the connection's buffer is
full). A client that vanishes without closing (a phone that drops off Wi-Fi) is bounded by TCP:
the server enables keep-alive probes (after 15 s idle, every 5 s, 3 of them) and gives up on data
left unacknowledged for 60 s. These are set on the listening socket, the one place the HTTP library
offers, so they reach a connection where the system copies a listener's options to the connections
it accepts: Linux, and so Android, clones the listener's TCP state into each one; macOS was
measured to copy keep-alive itself, the probe interval and the count, but not the idle time or the
retransmission bound, so there such a client keeps its place until the system's own retransmission
limit.

| event | data |
|---|---|
| `state` | the `GET /api/info` object, on every state change |
| `token` | per generated token: `step`, `steps`, `wall_ms`, `io_ms`, `compute_ms`, `mgmt_ms`, `stall_ms`, `flash_wait_ms`, `read_mib`, `cache_hit_pct` (-1 without a cache), `cache_budget_mib`, `rss_mib`, `mem_available_mib`, `majflt`, `cpu_ms`, `dense_resident_frac` (-1 unmeasured), `mtp_batch` |
| `done` | per generation, the CLI's `BMOE_DONE` keys (`docs/telemetry.md`): `cancelled`, `tokens`, `tok_s`, `load_s`, `prefill_s`, `prefill_tps`, `prefill_read_mib`, `prefill_dev_tokens`, `n_prompt`, `n_past`, `cache_hit_pct`, `read_mib`, `io_s_tok`, `compute_s_tok`, `stall_s_tok`, `flash_wait_s_tok`, `mgmt_s_tok`, `majflt_tok`, `cpu_s_tok`, `loop_overhead_s_tok`, `cache_resident_mib`, `cache_budget_mib`, `cache_evictions`, `cache_rereads`, `token_demand_mib`, `mtp_drafted`, `mtp_accepted`, `mtp_decodes`, `drafted_steps`, `mtp_draft_s_tok`, `ttft_s` |
| `decide` | per decision: the `POST /api/decide` answer |
| `download` | a download's progress: `id`, `file`, `received`, `total`, `state` (`running`, `done`, `error`, `cancelled`), `error` (with `running`: why the last attempt failed while the server waits to resume) |
| `backend` | a backend pack's install: `id` (the pack), `kind` (`backend`), `file`, `received`, `total`, `state` (`running`, `done`, `error`, `cancelled`), `error` (with `running`: the step, such as `unpacking`); with `done`, `restart_required` and `in_place` (false when the pack replaces one in use and takes its place at the next start) |
| `update` | the installer's download: the same fields with `id` `update`, and with `done` its `path` and `version` |
| `config` | the `GET /api/config` object, after any change |
| `plan_progress` | a measurement (`GET /api/plan?measure=1`, or an auto load) entering a stage: `step` (`devices`, `model`, `compute`, `storage`, `memory`, `placement`, `rules`), its words `label`, `index` from 0 and `count`; then `{"step": "done"}` when it ends. A UI shows `label`; `step` is the protocol |
| `conversation` | an agent conversation's `{id, title, state, updated, parent}`, whenever one of those changes ([agent.md](agent.md#events)) |
| `item` | `{conversation, item}`: an agent item added or changed, whole |
| `delta` | `{conversation, item, text?, reasoning?}`: what a streaming assistant item adds |
| `task` | an agent task, after it is saved and after a run of it starts or ends |
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
             "folder": "", "bytes": 18556686912, "shards": 1, "files": ["..."], "arch": "qwen3moe",
             "role": "model", "streamable": true, "complete": true}],
  "catalog": [{"id": "qwen3-30b-a3b-q4_k_m", "title": "Qwen3-30B-A3B", "quant": "Q4_K_M",
               "file": "Qwen3-30B-A3B-Q4_K_M.gguf", "bytes": 18556686912,
               "files": [{"name": "Qwen3-30B-A3B-Q4_K_M.gguf", "bytes": 18556686912, "url": "https://...",
                          "sha256": "..."}],
               "blurb": "...", "status": "available", "received": 12884901888, "partial": true,
               "note": ""}]
}
```

`local` lists every gguf in the models directory, then in each directory given with `--scan-dir
PATH` (repeatable; `scan_dirs` lists them), in that order: a split set once, by its first shard, with
`complete: false` while shards are missing. Each of these directories is read one level deep: its
own files, then those of every folder directly inside it, which is where a split set kept in a
folder of its own sits (`<dir>/<model>/<model>-00001-of-00003.gguf`). No deeper, and a link to a
folder is not followed, so a scan cannot loop or leave the directory through a name inside it; what
a client may open is still decided by the confinement above, whatever the listing shows. `dir` is
the directory a model was found in, `folder` the folder inside it ("" for the directory itself),
and `files`
every file of it that is present, in shard order, so a client that deletes a model deletes the set
and never has to know how shards are named. A scan directory is only read: downloads land in the
models directory, and a model in a scan directory is loaded in place by its `path`. The host app is
what knows where else models sit (the phone's adb-pushed and shared-storage folders); the server is
what says what they are. `streamable` is whether this build streams the
architecture. `role` is what the file is for, read from its header and never from its name:
`model`; `projector` (a multimodal projector: `general.type` is `mmproj`, or the projector
architecture `clip`); `draft` (a draft head for speculative decoding: it names another model's
`<arch>.target_layers`, or every one of its `<arch>.block_count` blocks is a prediction head,
`<arch>.nextn_predict_layers`); `other` (another `general.type`, or no `<arch>.block_count` at
all); `""` when the header could not be read. A UI lists anything but `model` and `""` apart from
the models, as files that are not loaded on their own. Catalog `status`: `on_disk`, `downloading`, `available`. `files` is every file of the
entry with its URL, in order, for a client that downloads for itself (a phone, where the server
has no `curl`), and its `sha256` when the catalog names one (64 lowercase hex digits; absent
otherwise), which the Android app checks its download against as the server does, and the iOS
app too; `file` and `bytes` are the first file's name and the whole entry's size. A catalog row is held to what its fields
become: `name` a bare file name (no separator, no `.` or `..`, no drive colon), `url` an https
URL (plain http only under an http mirror named with `--hf-endpoint`), `sha256` a digest when present. An entry with a file that fails this is not listed, and the
reason is in the server's start-up log; the Android and iOS apps drop such an entry too.
`received` is what of the entry is already in the models directory, complete files and `.part`
files alike, and `partial` is true when the entry is `available` with a `.part` on disk: a download
stopped by a failure, a cancel or a restart of the server (jobs live in memory, the `.part` files do
not), which downloading again resumes. A UI labels that action **Resume (X of Y GB)** (decimal GB,
`received` of `bytes`) and offers **Delete partial download** beside it. The phone's app downloads
into the server's models directory, so the same two fields describe its downloads too.

### `POST /api/models/download`

Body `{"id": "<catalog id>"}`. Starts (or resumes) the download of every file of the entry into
the models directory. `202`; progress arrives as `download` events. Each file is written to
`<name>.part` and renamed once whole; a start resumes whatever `.part` is there. Before a byte
moves, the bytes still needed (the entry's size less what is already on disk) are checked against
the free space of the models directory: `507` with `needs X GB more, only Y GB free` when they do not
fit. A dropped connection (curl's exit 6, 7, 16, 18, 28, 35, 52, 55, 56, 92: the line failed, not the
request) is retried on its own, resuming from the `.part`, up to six attempts per file with a wait
that starts at 2 s and doubles to at most 60 s (`server/download_policy.h`); while it waits the
`download` event stays `running` and carries the failure in `error`. Any other failure, or the last
attempt, ends in `error` with the `.part` kept. curl is run with `--proto =https --proto-redir
=https` for an https URL (a redirect cannot downgrade it; only a plain-http mirror named with
`--hf-endpoint` may use http), and `--` before the URL, so no URL is read as an option. When the
file has a `sha256`, a complete `.part` is read back and checked before it is renamed (the event
stays `running`, its `error` saying so); a digest that differs ends in `error` and deletes the
`.part`, since resuming it could only reproduce the same bytes.

### `DELETE /api/models/download/<id>`

Cancels a running download. The partial file is kept so a later download resumes it.

### `DELETE /api/models/partial/<id>`

Deletes the partial files a stopped download of entry `<id>` left: `{"ok": true, "freed": <bytes>}`
(`freed` 0 when there were none). Only the `.part` of each file the entry names, in the models
directory; complete files stay. `404` for an unknown entry, `409` while it downloads (cancel it
first).

### `DELETE /api/models/local/<name>`

Deletes a model from the models directory, named as `GET /api/models` lists it: `<folder>/<name>`
for one in a folder directly inside the directory, `<name>` otherwise, with `name` the first file
(shard 1 of a split set). Every file the scan groups under it goes, and the folder with them once it
is empty: `{"ok": true, "freed": <bytes>}`. A symbolic link is removed as the link. A set goes
whole or not at all: every file is first renamed aside, and one that cannot be (a file held open on
Windows) puts the others back, deletes nothing and answers `500` naming it. `400` for a name
that is not one (a separator, `..`, a drive), `404` when the models directory has no such model (a
scan directory's models are loaded in place and never deleted here), `409` with `code: "in_use"`
while it is loaded or loading (unload it first) or while a running download writes one of its
files, `403` with `code: "local_only"` from a client whose connection is not loopback.

### `POST /api/models/url`

Body `{"url": "https://.../<name>.gguf"}`. A direct link to a gguf becomes a catalog entry, as a
repository's model does with `POST /api/hf/add`, and is then downloaded by id with resume, cancel
and progress like every other: id `url:<name>`, titled by the file name, listed with
`"added": true`, kept between runs, forgotten with `DELETE /api/hf/entry/<id>`. The file name is
the link's last path segment (percent-escapes decoded), which must be a `.gguf` name made of
letters, digits, `.`, `_`, `-`, `+`, not starting with `.` or `-`; the query is kept, the fragment
dropped. A link to any shard of a split set (`<stem>-00002-of-00003.gguf`) names the whole set: the
other shards are the same link with the name changed, and a link may name at most 128 of them
(`kMaxUrlShards` in `server/url_entry.h`: the largest sets published are under 50 shards). Each
file's size comes from a `HEAD` request that follows redirects (on https only), asked shard by
shard within 120 s in all (`kUrlSizeDeadline`); a host that answers no size leaves it unknown (`0`)
and the download runs without a total, and so does a server with no `curl` to ask with (a phone's,
whose app downloads for itself and counts a file of unknown size whole once it is there). Without
`curl` nothing checks the link here, so any https gguf link becomes an entry; on a desktop server
with no `curl` the link is then refused at download, not at add. `201` with `{"id", "title", "quant", "bytes"}`; `400` with
`code: "invalid_url"` for a link that is not one of these, `502` when the host answers a `HEAD`
with an HTTP error, `504` when the sizes took longer than the deadline (no entry is made), `409` for an id the shipped catalog has, `403` with `code: "local_only"` from a
client whose connection is not loopback. Adding the same file again replaces the entry. A client
that downloads for itself does what it does for `POST /api/hf/add`: it fetches the entry's
`files[].url` from `GET /api/models` (the Android app, whose download ends at once, with the host's
status on its row, when the host answers a definite `4xx`; a refusal here is shown as the server
words it).

### `POST /api/hf/search`

Body `{"query": "<a model's name>", "token": "<optional>"}`. The repositories on the site with gguf
files whose name matches, the most downloaded first, twenty at most: `{"query", "repos": [{"id",
"downloads", "likes", "modified", "gated"}]}`. `gated` is a repository that needs a token, and
`modified` when it last changed, as the site gives it (ISO 8601); the server asks the site for each
of these fields by name (`expand[]`), since its search leaves the last two out otherwise. A UI
calls it when `POST /api/hf/models` refused the text with the code `not_a_repo`.

### `POST /api/hf/models`

Body `{"repo": "<owner/name, or a huggingface.co URL>", "token": "<optional>"}`. The gguf files
of the repository, as models: a split set is one model named by its first shard. A URL to a tree,
a blob or a resolve link names its revision, and a file where it has one (`file`), so a UI can
preselect it.

```json
{"repo": "ggml-org/gemma-4-26B-A4B-it-GGUF", "revision": "main", "file": "",
 "models": [{"name": "gemma-4-26B-A4B-it-Q4_0.gguf", "quant": "Q4_0", "bytes": 14618145824, "role": "model",
             "count": 1, "complete": true, "files": [{"path": "gemma-4-26B-A4B-it-Q4_0.gguf", "bytes": 14618145824}]}]}
```

`role` here comes from the file NAME, because the listing reads no header: `projector` for a name
with `mmproj` in it, `draft` for one starting `mtp-`, `dflash-` or `draft-`, `model` otherwise. Names
are a convention and not the format, so this only orders the listing (what the names suggest are
models first, then the companion files) and lets a UI group them; `POST /api/hf/inspect` reads the
header and decides.

`400` with the code `not_a_repo` for text that is not a repository id or a URL on the site (a
name to search for, `POST /api/hf/search`); `502` when the listing could
not be fetched (the server runs the system `curl`, as for downloads; a repository that does not
exist or needs a token answers through it as `401`); `404` when the site answers with an error.

### `POST /api/hf/inspect`

Body `{"repo", "name": "<a model's name from the listing>", "token"}`. What the model is before
a byte of it is downloaded: its header, read through a range request (the first tens of
megabytes of each shard) and parsed by the planner's own model probe, set against this machine.

```json
{"repo": "...", "revision": "main", "name": "gemma-4-26B-A4B-it-Q4_0.gguf", "quant": "Q4_0",
 "bytes": 14618145824, "files": 1, "complete": true,
 "model": {"readable": true, "arch": "gemma4", "role": "model", "type": "model", "block_count": 30, "is_moe": true, "n_expert": 128, "n_expert_used": 8,
           "n_layer": 30, "n_moe_layer": 30, "n_embd": 2816, "dense_bytes": 1755954296,
           "expert_bytes": 12846366720, "token_cycle_bytes": 802897920, "largest_dense_tensor": 784334848,
           "has_mtp": false, "streamable": true, "refusal": ""},
 "machine": {"residency_budget": 5736517632, "budget_if_loaded": 5736517632, "loaded_model": "", "loaded_held": 0,
             "memory_total": 15919931392, "disk_free": 38495465472, "present_bytes": 0},
 "verdict": {"downloadable": true, "mode": "streams", "fit": "fits",
             "headline": "Fits here: the experts stream from flash",
             "disk": "needs 13940 MiB of disk; the models folder has 36712 MiB free",
             "memory": "the experts stream from flash and 1674 MiB stays resident, plus a cache of at least one token cycle (765 MiB); that fits in the 5470 MiB this process may hold, and the plan sizes the cache from the rest"}}
```

`model.readable` false means the header did not parse, and the rest of `model` is empty; a MoE
whose experts this engine does not stream carries the reason in `refusal` and `streamable` false.
`model.role` is what the file is for, by its header, as `GET /api/models` has it; `type` and
`block_count` are the header's `general.type` and `<arch>.block_count` it was read from. A file
that is not a model (a projector, a draft head, or no language model at all) is `verdict.mode`
`not a model`, with a sentence saying what it is, and `downloadable` false: it cannot be chatted
with. Otherwise `verdict.mode` is `streams`, `plain mmap` or `not readable`, and `downloadable` is
false only when the disk cannot take what is still to download (`present_bytes` is what is
already there, complete).

`verdict.fit` is the answer as a code a UI colours by, and `verdict.headline` the server's line for
it, shown as the verdict's headline: `fits` (it streams and its resident floor fits what this process
may hold), `no_streaming` (a dense model,
or a MoE this build does not stream: it runs whole from the file), `will_not_fit` (its resident
floor is more than this process may hold), `no_disk` (the disk cannot take it), `not_a_model`,
`unknown` (the header did not parse, or it streams and the memory free here could not be read,
which the headline says). A UI keeps no threshold of its own; it shows the line and
colours by the code, and a code it does not know gets no colour.

The memory verdict answers "what happens if I load this", and a load unloads the model loaded now
first. `residency_budget` is what this process may hold as measured now, with that model still
resident; the verdict is set against `budget_if_loaded`, which adds back `loaded_held`, what the
plan the loaded model was loaded with charged in its memory ledger (no more than `memory_total`),
and the sentence names the model and how much of the figure it holds. A model loaded without a plan
has no such account (`loaded_held` 0): the figure stays the measured one, and the sentence says it
was measured with that model loaded. No second measurement is made for this. The two sentences are
the facts in words, for a UI to show as they are. `404` when the listing
has no such model, `502` when a header could not be fetched.

### `POST /api/hf/add`

Body `{"repo", "name", "token"}`. The model becomes a catalog entry, id
`hf:<repo>/<name>`, one file per shard with its resolve URL and, where the repository's listing
gives it, its LFS object id as `sha256` (the SHA-256 of the file), listed by `GET /api/models` with
`"added": true` and kept between runs (`added-models.json` beside the settings). `201` with
`{"id", "title", "quant", "bytes"}`; the download is then `POST /api/models/download` with that
id, or, on a host that downloads for itself, the entry's `files[].url`. Adding a model that was
already added is a `201` too, and replaces that entry (a repository's files change). `409` for a
split set with shards missing, or for an id the shipped catalog (`catalog/models.json`) already
has, which an addition never replaces.

### `DELETE /api/hf/entry/<id>`

Forgets an added entry, from a repository (`hf:`) or a link (`url:`). Files on disk stay. `404` for
an id that is not an added entry.

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
 "decisions": [{"knob": "cache-mb", "label": "Expert cache", "group": "set", "value": "703",
                "source": "derived", "brief": "703 MiB: what the cache earns most with, after the dense weights.",
                "reason": "..."}],
 "values": {"cache-mb": 703, "threads": 8}, "args": ["--moe-stream", "--cache-mb", "703"],
 "not_applicable": [],
 "summary": {"sentence": "Experts stream from flash, with a 0.7 GiB cache.",
             "lanes": [{"phase": "prompt", "label": "Prompt", "on": ["CPU"]},
                       {"phase": "tokens", "label": "Tokens", "on": ["CPU"]}]},
 "predicted": {"s_per_token": 0.302, "cache_hit_pct": 38.0, "s_per_token_worst": 1.69},
 "memory": {"pools": [{"name": "host", "label": "This process", "cap_bytes": 3552575488,
                       "lockable_cap_bytes": 0, "held_bytes": 3322400768, "locked_bytes": 0,
                       "rows": [{"name": "dense", "label": "Dense", "bytes": 1932735283,
                                 "locked": false, "in_turn": false}]}]},
 "devices": [{"name": "CPU", "description": "...", "cpu": true, "own_memory": false, "memory_total": 0,
              "prompt_ratio": 1.0, "token_ratio": 1.0, "rate_text": "The reference",
              "uses": ["prompt", "tokens"], "status": "used", "badge": "Prompt and tokens",
              "why": "The host: every rate is measured against it."}],
 "devices_none": "build", "devices_none_text": "No other device: this build carries no backend for one.",
 "explain": "..."}
```

`source` is `measured | derived | policy | operator | unprobed`. Keys in `user_keys` come back as
`operator`: the planner never touches them. A decision's `brief` is one plain sentence for a
person (at most 120 characters, the number that decided it, no flag names; the `reason` where a
decision has none), and `reason` the full account `explain` prints, for a "why" a UI folds away.
`value_text` is `value` as a person reads it: a choice by its settings label ("pinned in memory"),
a size in GiB or MiB, a number with its unit, a planner word as words; `value` stays the raw one.
Its `label` is its knob's settings label from the parameter table, or, for a knob that is not a
parameter, the name of the note (`regime` is "Streaming mode", `storage-rate` "Storage speed",
`memory-ledger` "Memory use"; a topic the server does not know is titled from its key), so a UI
shows "label: brief". `group` is how a UI reads it: `set` (a parameter the plan decided from a
fact, whether or not the value changed), `kept` (a parameter it left: the caller's own, or one
nothing measured), `notes` (what is not a parameter: the regime, the placement, the ledger, and
each of the cost model's notes under its own topic - `storage-rate`, `cache-floor`,
`shared-memory`, ... - with its key number as `value`, or `-`; there is no `cost` knob any more).

The rest describes the plan for a person (`server/plan_view.h`), so a UI shows it without knowing
any device, system or knob. Every list is as long as the machine makes it, and every word a UI
shows is here: `label`, `badge`, `rate_text`, `why`, the sentences. A code beside a word
(`phase`, `uses`, `status`, `devices_none`) is for a UI to pick a colour by, never to word itself,
so a stage or a role the server adds reaches every UI with no UI change:

- `summary.sentence` is what the plan decided, in a sentence; `summary.lanes` which device names
  compute the prompt and the tokens (a device holding layers in memory of its own is on the
  tokens lane beside the host).
- `predicted` is decode as the plan priced it, the figures `bmoe-cli --auto` prints after a run:
  `s_per_token`; `cache_hit_pct` where there is a cache, the hit rate the cost model credited the
  cache with (`Allocation::cache_hit_credited`), not the cache's share of the experts: read off
  the model's own hit curve when the server kept a routing record for that file (`routing/` in the
  data folder, one file per model; see `docs/hardware-planning.md`), else the linear credit; and
  `s_per_token_worst`, the same token with nothing resident and every byte read again, a bound
  beside the prediction and never divided into it. Each is absent when not predicted.
- `memory.pools` is the ledger the plan charged: the host's pool (`name` `host`; `cap_bytes`
  what the process may hold, `lockable_cap_bytes` what it may hold locked) with one row per thing
  held for the whole run, and one pool for each device with memory of its own that holds layers.
  The host pool's `lockable_cap_known` distinguishes a measured ceiling (including zero) from an
  unread one; when false, `lockable_cap_bytes` preserves an upper bound rather than a measured
  grant. `cache_lock_unverified` means cache pinning is a runtime request: that cache is not counted
  in `locked_bytes` or marked as a locked row. Neither field changes the ordinary process capacity.
  A row with `in_turn` is not held beside the expert cache but in its place while a prompt runs.
  A device pool with `estimated` is an even share of a sum the placement reports over several
  devices; with one device its figures are that device's own.
- `devices` is every device the plan saw, the host included: its rate against the host on a
  prompt-wide matmul (`prompt_ratio`, in words `rate_text`) and on one token (`token_ratio`), 0
  when unmeasured; `uses`, which of `prompt`, `tokens`, `layers` it does in this run; `status`,
  `used`, `unused`, `refused` (its results did not match the host's) or `unusable` (its probe did
  not come back); `badge`, the two in words; `why` in a sentence.
- `devices_none` says why there is no device beyond the host, when there is none: `build` (this
  build carries no backend that could offer a local one), `machine` (it carries one and this
  machine gave it no device) or `not_probed`; empty otherwise. `devices_none_text` is the same
  in words. A remote backend (RPC) and a library the host calls into (BLAS) are not counted as
  backends that could offer a device, so a build that carries only those reads `build`.

`values` are the parameters the plan changes, read
through the parameter table; `args` are the arguments that reproduce the plan, one per element
and unquoted, and empty when the plan declines streaming; `not_applicable` names anything the plan
decided that the table cannot hold (a tensor-placement pattern, say), so it is shown rather than
silently dropped.
Until the model's lossless knobs are measured on this machine, a plan's `values` include
`trial-threads` and `trial-overlap`: the first answer after the load decodes in alternating windows
to measure them (`docs/hardware-planning.md`), and the server keeps the verdict beside the model's
routing record under `routing/`. The next plan of that model reads it, its `threads` and `overlap`
decisions say `measured on this model`, and those two values are gone.
With a model loaded that an auto load planned, `GET /api/plan` answers at once with that plan and
`"from_last_load": true`, without measuring, as long as nothing was measured after that load; once
the model has answered it also carries `measured`: what its last complete turn measured for decode
(`s_per_token`, `cache_hit_pct` over that turn's decode alone, -1 without a cache, and `tokens`), to
set beside `predicted`; a cancelled turn does not count. `GET /api/plan?cached=1` never measures:
it answers the last plan this server holds (the last measurement, else the last load's) with
`"cached": true`, or `{"available": true, "none": true}` when it holds none, so a screen can open on
it at no cost; with a model loaded and that plan measured for it by hand after the load, the plan
the person saw last is what Apply applies, so it is the one answered, with `measured` beside it
too (the figures are the running model's, under the load's settings, so a UI shows them without
comparing them to this plan's prediction). `GET /api/plan?measure=1` measures again regardless; a
plan computed while a model is loaded carries a `warning`, because that model's memory reads as
taken and the plan is sized for a smaller machine. Auto mode avoids this by planning with nothing
loaded.

### `POST /api/plan/apply`

Applies the current plan's `values` to the config without adding them to `user_keys`, and records
the decisions so `GET /api/config` returns them under `plan`. Returns the config object. A plan is
made from the configuration without the plan layer (defaults, launch flags, the person's values),
so its `values` are complete on their own and replace the previous plan whole.

## Backend packs and updates

The desktop app's side of the release ([desktop-app.md](desktop-app.md)): the signed feed, read
when a person asks and never otherwise, backend packs installed into `<data>/backends/<id>/`, and
the installer of a newer release. Every route here answers a local client only (`403`,
`code: "local_only"` from the network). A build that carries no feed key answers `503` with
`code: "no_feed_key"` where it would read the feed. `--feed-url URL` (https) reads the feed from
elsewhere, for testing.

### `GET /api/backends`

`{registered, target, feed, packs, installed, restart_required, feed_error?}`. `registered` is every
backend that registered at start (`CPU`, `MTL`, `Vulkan`, ...); `target` the build's
(`macos-arm64`, ...); `feed` `{version, published}` of the verified feed known, or `null`. `packs`
is that feed's packs for this target: `{id, title, size, applies, installed, installed_version,
current}`, `applies` when one of the pack's driver libraries loads here, `current` when the
installed copy is this feed's file. `installed` lists every pack on disk (`{id, version, offered}`).
`restart_required` is true once a pack was installed since the server started. With `?refresh=1`
the feed is read from the network first; a feed that cannot be read or does not verify is said in
`feed_error`, and the rest of the answer stands. Without it, the feed kept from the last read
answers (checked again), or none.

### `POST /api/backends/<id>/install`

Body `{}` or `{"force": true}` (install a pack that does not apply here). Downloads the pack of the
last feed read, checks its size and SHA-256, lists the archive with the system's `tar` and refuses
it if an entry would leave the folder (an absolute path, a drive, a backslash, `..`, a hard link, a
device, an entry under a link), unpacks it into a staging folder, checks the files again (every link
points inside), and moves it to `<data>/backends/<id>/` with a `pack.json` marker. The answer comes
when it is done: `200` with `{ok, id, restart_required: true}`; progress meanwhile as `backend`
events. `409` with `no_feed` (no feed read yet), `not_applicable` (no driver library loads; `force`
overrides), `busy` (another download runs); `404` `not_found` (no such pack for this target); `502`
`failed` with the reason when the download, the checks or the unpacking fail, in which case nothing
is installed. The engine loads the pack at the next start of the server.

### `GET /api/update`

Reads the feed and answers `{current, latest, available, notes, published, target, installer}`:
`available` when the feed's version is newer than this server's and has an installer for this
target, `installer` `{url, sha256, size, file}` or `null`. `502` `feed` when the feed cannot be read
or does not verify.

### `POST /api/update/download`

Downloads the installer of the feed last read into `<data>/updates/`, checks its size and SHA-256,
and answers once it is there: `200` with `{ok, version, path}`; progress as `update` events. The
desktop app runs `path` (`bmoeDesktop.applyUpdate`), which accepts nothing outside that folder.
`409` with `no_feed` or `up_to_date`, or `busy`; `502` `failed` when it does not arrive whole.
