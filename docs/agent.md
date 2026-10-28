# The agent

The server runs an agent: a conversation that can work in a folder of this computer, read and change
its files, run commands, look things up on the web, follow skills, hand part of a job to a helper,
and run on a schedule. It is part of `bmoe-server`, like everything else an application needs
([application-framework.md](application-framework.md)), so every UI gets the same agent through the
same API, and an agent run does not depend on a UI staying open.

This page is the contract: the concepts, the routes, the events, and the rules the server enforces.
The HTTP details, with every error, sit in [server-api.md](server-api.md#agent). The code is
`server/agent*.{h,cpp}` (the run loop, tools, skills, schedule, store, routes),
`server/provider_protocol.*` and `server/connect.*`; `tests/agent_test.cpp` drives the loop with a
scripted session and checks every rule below without a model.

## Concepts

- **Conversation.** Held by the server (`<data>/agent/conversations/<id>.json`, written whole to a
  temporary file and renamed, so a crash leaves the previous version), because a run can outlive
  the page that started it: a scheduled task has no page at all, and an approval can wait for a
  person who closed the tab. A conversation has a title, an optional workspace, a mode, a model
  route, an agent (`builtin`, or another coding agent: see "Other agents") and a list of items. Plain chat in the web UI is a conversation with no workspace. A
  conversation that was running when the server stopped reads `idle` on the next start, with a
  notice saying its run ended there.
- **Workspace.** A folder a person added (`<data>/agent/workspaces.json`): `id`, `name`, `path`
  (absolute, made canonical when added; the same folder added twice is the same workspace). It can
  be any folder the server's user can read, an app's own storage included: nothing assumes a home
  folder. File tools resolve every path inside it and refuse anything that leaves it, after
  resolving `..` and symbolic links; a dangling link is refused too, since writing through it would
  create its target wherever it points. A conversation without a workspace has no file or command
  tools. Forgetting a workspace touches no file; its conversations keep their items and lose their
  tools.
- **Mode.** What the agent may do without asking:

  | Mode | Reads | Changes files, runs commands | Web |
  |---|---|---|---|
  | `ask` (default) | yes | asks first: one approval card, nothing has run when it appears | asks once per conversation |
  | `read_only` | yes | refused, the model reads why | asks once per conversation |
  | `full` | yes | yes | yes |

  A command counts as a change unless the server can tell it only reads: every stage of a pipeline
  is a program on a short list (`ls`, `cat`, `head`, `tail`, `wc`, `grep`, `rg`, `find`, ...) with
  no option that writes or runs something (`find -exec`/`-delete`, `sort -o` or `--compress-program`
  and their abbreviations, `rg --pre`, ...) and no argument that may lead out
  of the workspace (absolute, `~`, `..`, a drive), and the line has no redirection, substitution,
  separator or background job. Anything it cannot tell asks, so a read that skips the question
  stays inside the folder the file tools are confined to. `git` always
  asks: a repository's own configuration can make even `git status` run a program (`core.fsmonitor`,
  a clean filter, `diff.external`), and the folder may have come from anyone. An edit's
  approval card carries the diff the edit will make, computed before the card is shown; after a
  yes the edit is computed again, so a file that changed meanwhile is never overwritten blind.
  The mode may change while a run waits (a person can switch to `full` with a card open); the next
  tool call reads the new one.
- **Model route.** `local` (the loaded model) or `<provider id>/<model>` for a saved provider
  ([Providers](#providers)).
- **Item.** One entry of a conversation, upserted by `id`:

  | `kind` | fields |
  |---|---|
  | `user` | `text` |
  | `assistant` | `text`, `reasoning`, `streaming`, `tool_calls` (`[{id, name, arguments}]`, when the round called tools) |
  | `tool` | `tool`, `title` (plain words: "Read src/main.cpp"), `args` (object), `state` (`running`, `waiting`, `done`, `denied`, `error`), `result` (text, capped), `diff` (unified, for edits), `file` (workspace-relative path the tool produced or changed, for the preview), `call_id`, `shortened` (true once the context ran short: the model then reads a few lines of `result`, the person still the whole) |
  | `helper` | `title`, `state` (`running`, `done`, `error`), `summary`, `conversation` (the helper's own conversation id), `call_id` |
  | `notice` | `text`, `level` (`info`, `warn`, `error`): why something did not happen, in plain words |

## The run

A message starts a run on a thread of the server: one run per conversation (a second message while
one runs is `409 busy`), and its rounds line up behind the engine's turn line like any chat turn.
A round sends the conversation as it stands: a system prompt naming the workspace, the platform,
the date, the mode and the switched-on skills' names and descriptions, then the items as messages,
and only the tools the conversation may use. What the model writes streams as `delta` events; its
tool calls run under the mode, their results are appended, and the next round asks again, until an
answer that calls no tool or the round limit (30 rounds per message; a notice says so, and another
message lets it go on). The same call failing the same way three times in a row also stops the run,
with a notice: a small model that repeats a refused call would otherwise spend every round on it.
Without a workspace the prompt tells the model to ask the person for a folder rather than look for
files some other way, and `fetch_url` takes http and https addresses only. The system prompt is the same for every round of a message and the
conversation is only appended to, so the engine reuses the prompt its KV cache already holds.

When a request no longer fits the context (the engine refuses it with `context_full`), the oldest
long tool result is marked `shortened` and the round is asked again, one result at a time; a notice
says it once. The person's messages are never dropped: when every result is short and it still does
not fit, the run ends with an error notice that says to start a new conversation or load the model
with a larger context.

A stop raises the run's stop: a waiting approval ends at once as denied, a command's whole process
group is killed, a web request or a provider's stream ends at its next piece, and the engine's turn
ends at its next token (or gives up its place in the line). `POST .../stop` answers once the run
has ended (waiting at most 30 s, the longest a prompt's prefill may delay that next token).

No model loaded is not a refused message: the message is kept, and the run ends at once with an
error notice ("No model is loaded ...") and the conversation in state `error`. Sending it again
(`edit` with the same item) retries.

## Tools

| Tool | What it does | Needs |
|---|---|---|
| `list_files` | a folder's entries, with sizes (at most 400) | workspace |
| `read_file` | a text file, or a line range of it (files up to 8 MiB) | workspace |
| `search_files` | lines containing a text, case-insensitive, under a folder (at most 200; `.git`, `node_modules`, `.venv`, `__pycache__` skipped) | workspace |
| `write_file` | create or replace a file | workspace, a change |
| `edit_file` | replace one exact passage of a file (it must appear exactly once) | workspace, a change |
| `run_command` | a command in the workspace (`/bin/sh -c`, `cmd /c` on Windows), with a 120 s limit and capped output | workspace, a change, a platform that may start a process |
| `fetch_url` | a web page (https), as text (2 MiB read at most); never this computer or a private network named by its address or a local name | web |
| `web_search` | result titles, links and snippets, from a keyless search page | web |
| `use_skill` | a skill's instructions | a skill that is switched on |
| `delegate` | a helper conversation with its own task; its final answer comes back | not inside a helper |

Which tools a conversation is offered follows from its workspace, its mode, the platform and where
the message came from: a tool that could only be refused is not declared (`write_file` and
`edit_file` in `read_only`; `run_command` where no process may start). A result the model reads is
capped at 12 KiB. A helper runs inside its parent's run, in the parent's workspace, mode and route,
with its own conversation (listed only through its parent's `helper` item); a helper may not start
helpers, and one message may start four. Every limit is a named constant in `server/agent.h` and
`server/agent_tools.h`.

## Skills

A skill is a folder with a `SKILL.md`: front matter with a kebab-case `name` and a `description`
(an optional `title`), then the instructions. They come from three places: the ones the release
ships (`skills/` beside the executable, or `--skills-dir`), the ones a person writes in the app
(`<data>/skills/`), and a workspace's own (`<workspace>/.bmoe/skills/`); a later one replaces an
earlier one of the same name. A file that does not parse, or whose name is not its folder's, is
skipped and the others are listed. Every skill is on until switched off
(`<data>/agent/skills.json` holds the switches). The agent sees every switched-on skill's name and
description, and reads the instructions with `use_skill` when it needs them. The release ships
`review-change`, `write-tests`, `summarize-document`, `web-research` and `explain-codebase`.

## Tasks

A task is a prompt that runs as a conversation on a schedule, in a workspace or none, in
`read_only` mode unless the person picked another. The schedule is local time: `once` at
`"YYYY-MM-DDTHH:MM"`, `interval` every `every_minutes` (at least 5) from the last run (or from `at`
for the first), `daily` at `"HH:MM"`, `weekly` at `"HH:MM"` on `weekdays` (0 = Sunday .. 6 =
Saturday, as a JavaScript `Date` counts them). A time that passed while the server was not running
runs once when it is next seen. Nobody is at the screen: an approval is answered "no" at once, the
model reads that it was not done, and a notice counts what was refused. Every run leaves its
conversation (its `task` field names the task), so the result can be opened like any other; a
`once` task switches itself off after its run. The scheduler is the server's, and runs while the
server runs.

## Providers

A saved connection to an OpenAI-compatible chat service: `id` (made from the name when it is
added), `name`, `base_url`, `models`, and a key that is written once and never read back
(`<data>/credentials.json`, created readable by this user only before the key is written; never
in a listing, a log or a command line: curl reads it from a file only this process writes). A
conversation on a provider's route sends its messages to `<base_url>/chat/completions` with
streaming and the tools as OpenAI tools, and nothing else leaves the computer. `http` is accepted
for a provider on this machine or the local network.

## Connecting other apps

`GET /api/connect` lists the coding agents and apps the server can serve, with the settings for
this server (address, the loaded model, whether a key is needed: the key itself is never written
into a snippet or a file), from `catalog/clients.json` (data, beside the model catalog). For a
client whose settings live in a file, `POST /api/connect/<id>` writes them there, keeping a backup
of the file it replaces (`<file>.bmoe-backup-<UTC time>`): a TOML file gets a block of its own
between two marker lines, replaced on the next write, and a JSON file has the snippet merged into
it key by key (one that does not parse is not touched). It needs a loaded model, whose id the
client asks for.

## Other agents

On a computer that runs programs, a conversation with a workspace may be worked by another coding
agent instead of the built-in one: OpenCode, Claude Code, Codex or Pi, whichever is installed. The
built-in agent stays the default; the other is chosen before the first message (`agent` on
`POST /api/conversations`; a `PATCH` that changes it on a conversation with items is `409`, since
the other agent would not know what was said). `GET /api/agent` lists them with whether each is
installed (`available`, and the programs `missing`), so the UI offers only what runs.

They are spoken to over the [Agent Client Protocol](https://agentclientprotocol.com) (ACP):
JSON-RPC 2.0, one message a line, on the program's stdin and stdout (`server/acp_connection.*`).
How each one starts is data, not code: the `acp` entry of its row in `catalog/clients.json`
(`command` and the programs it `needs` on PATH), pinned to the version the ACP registry lists.
OpenCode speaks ACP itself (`opencode acp`); Claude Code, Codex and Pi run through their published
adapters with `npx`. The program runs in the workspace, in a process group of its own (a job on
Windows) that ends with it (`platform::LineProcess`), one per conversation, kept between its
messages and ended when the conversation is deleted or the server stops.

What it does reaches the conversation as the built-in agent's would (`server/agent_external.cpp`):

- Its text and reasoning are `assistant` items, streamed as `delta` events; each tool call is a
  `tool` item, its `tool` the protocol's kind (`read`, `edit`, `execute`, `fetch`, ...), its
  result the text it reports, its diff shown when it carries one.
- A permission it asks for is the conversation's mode's to answer: `full` allows it, `read_only`
  declines it, `ask` puts it in front of the person as an approval card (with the diff, when the
  agent sent one) and waits. A stop is `session/cancel`; an agent that has not answered ten seconds
  later is ended.
- It reads and writes files through the server (`fs/read_text_file`, `fs/write_text_file`), which
  serves them inside the workspace only, with the same path confinement as the built-in tools; a
  read-only conversation takes no write. It gets no terminal from the server: a command it runs, it
  runs itself, under the permission it asked for.
- The session id is kept with the conversation (`acp_session`); an agent started again (the server
  restarted) resumes it with `session/load` where it can, and says so with a notice where it cannot.
  An `edit` of an earlier message is `409`: another agent's session cannot be rewritten.

The model is the agent's own business: each is set up with Connect (below) to use this server, or
uses its own account. Such a conversation reaches this computer, so it answers to a local client
only (see below).

## Who may use it

Everything that reaches this computer's files, processes or secrets answers only to a client whose
connection comes from loopback, whatever the server's `--host` and whatever key it sends: adding or
removing a workspace, a message (or an edit) in a conversation that has a workspace or a provider
route, a workspace file's preview, an approval, writing a skill or switching one, saving a
provider, writing a client's settings, and creating, changing or running a task. A conversation
with a workspace or a provider route is this computer's alone: from the network it is not listed,
cannot be read, changed, deleted or stopped, and its events (`conversation`, `item`, `delta`) go
to local streams only, as do `task` events. A client on the network may not give a conversation a
workspace or a provider route, nor set `full` access (asking or reading only, it may). From the
network these get `403` with `code: "local_only"`. A conversation without a workspace works from the
network with the key, so the web UI on a phone keeps its chats; a message from the network runs
without web tools, so a client elsewhere cannot have this computer fetch addresses of its network.
The rule is `agent_route_locality()` in `server/access.h`.

## On a phone

The Android and iOS apps are chat apps: they start their server with `--chat-only`, so their
conversations are held by the server (one record, the same API) but are plain chat, with no tools,
no agent prompt, no workspaces and no tasks (`GET /api/agent` answers `tools: false`). A small
model on a phone is not an agent; the agent is the desktop's. On iOS the server runs inside the app
(`run()` on a thread), and its stop (`server::Embedding`) joins every run before `run()` returns.

A server elsewhere decides what it offers from the platform's facts, not from the app:
`run_command` is offered only where `platform::can_run_commands()` holds, and `GET /api/connect`
lists no file to write where there is no home folder.

## Routes

All JSON; errors are `{"error", "code"}` as everywhere in the API. Times are Unix seconds.

| Route | Body / answer |
|---|---|
| `GET /api/agent` | `{tools, commands, agents: [{id, name, available, missing}]}`: whether conversations here have tools and commands, and the agents one may use (the built-in one first; none where commands cannot run) |
| `GET /api/workspaces` | `[{id, name, path}]` |
| `POST /api/workspaces` | `{path, name?}` → the workspace; `400 not_a_folder` |
| `DELETE /api/workspaces/<id>` | forgets the folder, touches no file |
| `GET /api/conversations` | `[{id, title, workspace, mode, route, state, created, updated, task, parent, web_allowed}]`, newest first, helpers left out; `state` is `idle`, `running`, `waiting` (an approval, the conversation's or one of its helpers') or `error` (the last run ended in one) |
| `POST /api/conversations` | `{workspace?, mode?, route?, agent?, title?, items?}` → the conversation; another `agent` needs a `workspace`. `items`: `[{kind: "user" \| "assistant", text}]`, a conversation the UI kept before the server did |
| `GET /api/conversations/<id>` | the conversation with its `items` |
| `PATCH /api/conversations/<id>` | `{title?, mode?, route?, workspace?, agent?}` → the conversation without items; `409 busy` for `workspace` or `route` while it runs, and for `agent` once it has items |
| `DELETE /api/conversations/<id>` | with its helpers; `409 busy` while it runs |
| `POST /api/conversations/<id>/messages` | `{text, think?, images?}` → `202 {item}`; the run goes on in the server and reports on `/api/events`. `409 busy` while one runs; `images` is not supported yet (`400 unsupported`) |
| `POST /api/conversations/<id>/edit` | `{item, text, think?}`: drops `item` (a `user` item) and everything after it, then sends `text` → `202 {item}` |
| `POST /api/conversations/<id>/approval` | `{item, decision: "approve" \| "deny"}`; the conversation or the helper the card is in; `404 not_found` when that item is not waiting |
| `POST /api/conversations/<id>/stop` | stops the run and its helpers; answers `{stopped}` once it has stopped (`false`: none ran) |
| `GET /api/conversations/<id>/file?path=` | a workspace file's bytes for the preview, with its content type (16 MiB at most), sandboxed by its own `Content-Security-Policy` |
| `GET /api/skills?workspace=<id>` | `[{name, title, description, source: "bundled" \| "user" \| "workspace", enabled, body}]`; the workspace's own with `workspace` |
| `POST /api/skills`, `PUT /api/skills/<name>` | `{name, description, body, title?}`: a person's own skill (a `PUT` on a bundled one writes the person's version of it); `409 exists` on a `POST` that repeats a name |
| `PATCH /api/skills/<name>` | `{enabled}` |
| `DELETE /api/skills/<name>` | a person's own skill only |
| `GET /api/tasks`, `POST /api/tasks`, `PUT /api/tasks/<id>`, `DELETE /api/tasks/<id>` | `{id, title, prompt, workspace, mode, route, schedule: {kind, at, every_minutes, weekdays}, enabled, created, last_run: {at, conversation, outcome: running \| done \| error \| stopped}, next_run, running}` |
| `POST /api/tasks/<id>/run` | runs it now → `202` the task; `409 busy` while it runs |
| `GET /api/providers` | `[{id, name, base_url, models, has_key}]` |
| `POST /api/providers`, `PUT /api/providers/<id>` | `{name, base_url, models, api_key?}`: `api_key` absent keeps the key, `""` removes it |
| `DELETE /api/providers/<id>` | forgets it and its key |
| `GET /api/connect` | `[{id, name, how, snippet, file, writable}]` |
| `POST /api/connect/<id>` | writes the settings: `{file, backup}`; `409` with no model loaded |

## Events

On `/api/events`, beside the existing ones:

| event | data |
|---|---|
| `conversation` | `{id, title, state, updated, parent}`, whenever one of those changes |
| `item` | `{conversation, item}`: an item was added or changed, whole |
| `delta` | `{conversation, item, text?, reasoning?}`: what a streaming assistant item adds. When the model's parser takes back text it showed (a closing reasoning tag), an `item` event carries the item whole instead |
| `task` | the task, after it is saved and after a run starts or ends |

An assistant item appears with its first token: a round that fails before one leaves no empty item.
A UI that reconnects reads the whole state with `GET /api/conversations/<id>`.
