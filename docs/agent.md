# The agent

The server runs an agent: a conversation that can work in a folder of this computer, read and change
its files, run commands, look things up on the web, follow skills, hand part of a job to a helper,
and run on a schedule. It is part of `bmoe-server`, like everything else an application needs
([application-framework.md](application-framework.md)), so every UI gets the same agent through the
same API, and an agent run does not depend on a UI staying open.

This page is the contract: the concepts, the routes, the events, and the rules the server enforces.
The HTTP details sit in [server-api.md](server-api.md#agent).

## Concepts

- **Conversation.** Held by the server (`<data>/agent/conversations/<id>.json`), because a run can
  outlive the page that started it: a scheduled task has no page at all, and an approval can wait
  for a person who closed the tab. A conversation has a title, an optional workspace, a mode, a
  model route and a list of items. Plain chat in the web UI is a conversation with no workspace.
- **Workspace.** A folder a person added (`<data>/agent/workspaces.json`): `id`, `name`, `path`.
  File tools resolve every path inside it and refuse anything that leaves it (after resolving `..`
  and symbolic links). A conversation without a workspace has no file or command tools.
- **Mode.** What the agent may do without asking:

  | Mode | Reads | Changes files, runs commands | Web |
  |---|---|---|---|
  | `ask` (default) | yes | asks first: one approval card, nothing has run when it appears | asks once per conversation |
  | `read_only` | yes | refused, the model reads why | asks once per conversation |
  | `full` | yes | yes | yes |

  A command counts as a change unless the server can tell it only reads (a short list of read-only
  programs with no write or execute option); anything it cannot tell asks.
- **Model route.** `local` (the loaded model) or `<provider id>/<model>` for a saved provider
  ([Providers](#providers)).
- **Item.** One entry of a conversation, upserted by `id`:

  | `kind` | fields |
  |---|---|
  | `user` | `text` |
  | `assistant` | `text`, `reasoning`, `streaming` |
  | `tool` | `tool`, `title` (plain words: "Read src/main.cpp"), `args` (object), `state` (`running`, `waiting`, `done`, `denied`, `error`), `result` (text, capped), `diff` (unified, for edits), `file` (workspace-relative path the tool produced or changed, for the preview) |
  | `helper` | `title`, `state`, `summary`, `conversation` (the helper's own conversation id) |
  | `notice` | `text`, `level` (`info`, `warn`, `error`): why something did not happen, in plain words |

## Tools

| Tool | What it does | Needs |
|---|---|---|
| `list_files` | a folder's entries, with sizes | workspace |
| `read_file` | a text file, or a line range of it | workspace |
| `search_files` | lines matching a pattern under a folder | workspace |
| `write_file` | create or replace a file | workspace, a change |
| `edit_file` | replace one exact passage of a file | workspace, a change |
| `run_command` | a command in the workspace, with a time limit and capped output | workspace, a change, a platform that may start a process |
| `fetch_url` | a web page, as text | web |
| `web_search` | result titles, links and snippets | web |
| `use_skill` | a skill's instructions | a skill that is switched on |
| `delegate` | a helper conversation with its own task; its final answer comes back | — |

Which tools a conversation is offered follows from its workspace, its mode and the platform: a tool
that could only be refused is not declared. Limits (rounds per message, output bytes kept per tool
result, command time) are named constants in the server.

## Skills

A skill is a folder with a `SKILL.md`: front matter with a kebab-case `name` and a `description`,
then the instructions. They come from three places: the ones the release ships (`skills/` beside the
executable), the ones a person writes in the app (`<data>/skills/`), and a workspace's own
(`<workspace>/.bmoe/skills/`). The agent sees every switched-on skill's name and description, and
reads the instructions with `use_skill` when it needs them.

## Tasks

A task is a prompt that runs as a conversation on a schedule (`once`, `interval`, `daily`,
`weekly`), in a workspace or none, in `read_only` mode unless the person picked another. Nobody is at
the screen: an approval is answered "no" at once and the result says what was not done. Every run
leaves its conversation, so the result can be opened like any other. The scheduler is the server's,
and runs while the server runs.

## Providers

A saved connection to an OpenAI-compatible chat service: `id`, `name`, `base_url`, `models`, and a
key that is written once and never read back (`<data>/credentials.json`, readable by this user only).
A conversation on a provider's route sends its messages there, and nothing else leaves the computer.

## Connecting other apps

`GET /api/connect` lists the coding agents and apps the server can serve, with the exact settings
for this server (address, key, model name), from `catalog/clients.json`. For a client whose settings
live in a file, `POST /api/connect/<id>` writes them there, keeping a backup of the file it replaces.

## Who may use it

Everything that reaches this computer's files, processes or secrets answers only to a client on
loopback, whatever the server's `--host`: adding or removing a workspace, a message in a
conversation that has one, an approval, writing a skill, saving a provider, writing a client's
settings, and creating or changing a task. From the network these get `403` with
`code: "local_only"`. A conversation without a workspace works from the network with the key, so the
web UI on a phone keeps its chats.

## Routes

All JSON; errors are `{"error", "code"}` as everywhere in the API.

| Route | Body / answer |
|---|---|
| `GET /api/workspaces` | `[{id, name, path}]` |
| `POST /api/workspaces` | `{path, name?}` → the workspace; `400 not_a_folder` |
| `DELETE /api/workspaces/<id>` | forgets the folder, touches no file |
| `GET /api/conversations` | `[{id, title, workspace, mode, route, state, updated, task}]`, newest first; `state` is `idle`, `running`, `waiting` (an approval) or `error` |
| `POST /api/conversations` | `{workspace?, mode?, route?, title?}` → the conversation |
| `GET /api/conversations/<id>` | the conversation with its `items` |
| `PATCH /api/conversations/<id>` | `{title?, mode?, route?, workspace?}` |
| `DELETE /api/conversations/<id>` | |
| `POST /api/conversations/<id>/messages` | `{text, think?, images?}` → `202 {item}`; the run goes on in the server and reports on `/api/events`. `409 busy` while one runs |
| `POST /api/conversations/<id>/edit` | `{item, text}`: drops `item` and everything after it, then sends `text` |
| `POST /api/conversations/<id>/approval` | `{item, decision: "approve" \| "deny"}` |
| `POST /api/conversations/<id>/stop` | stops the run and its helpers; answers once it has stopped |
| `GET /api/conversations/<id>/file?path=` | a workspace file's bytes for the preview, with its content type |
| `GET /api/skills` | `[{name, title, description, source: "bundled" \| "user" \| "workspace", enabled, body}]` |
| `POST /api/skills`, `PUT /api/skills/<name>` | `{name, description, body}` |
| `PATCH /api/skills/<name>` | `{enabled}` |
| `DELETE /api/skills/<name>` | a person's own skill only |
| `GET /api/tasks`, `POST /api/tasks`, `PUT /api/tasks/<id>`, `DELETE /api/tasks/<id>` | `{id, title, prompt, workspace, mode, route, schedule: {kind, at, every_minutes, weekdays}, enabled, last_run: {at, conversation, outcome}, next_run}` |
| `POST /api/tasks/<id>/run` | runs it now |
| `GET /api/providers` | `[{id, name, base_url, models, has_key}]` |
| `POST /api/providers`, `PUT /api/providers/<id>` | `{name, base_url, models, api_key?}` |
| `DELETE /api/providers/<id>` | forgets it and its key |
| `GET /api/connect` | `[{id, name, how, snippet, file, writable}]` |
| `POST /api/connect/<id>` | writes the settings: `{file, backup}` |

## Events

On `/api/events`, beside the existing ones:

| event | data |
|---|---|
| `conversation` | `{id, title, state, updated}`, whenever one of those changes |
| `item` | `{conversation, item}`: an item was added or changed, whole |
| `delta` | `{conversation, item, text?, reasoning?}`: what a streaming assistant item adds |
| `task` | the task, after a run starts or ends |
