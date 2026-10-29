# BigMoeOnEdge on iOS

Two apps, one engine:

- **The app** (`app/`, `BmoeKit/`): the five screens every UI of this project has (chat, models,
  plan, settings, metrics, [docs/screens.md](../../docs/screens.md)) in SwiftUI, over
  `bmoe-server`. An app on iOS may not start a process, so the app links the server as a library
  (`server/server.h`) and runs it on a thread of its own, then speaks the same HTTP API to it on
  loopback that the web UI and the Android app speak ([docs/application-framework.md](../../docs/application-framework.md)).
- **The harness** (`harness/`): `bmoe-cli` compiled into an app, one field for the command line
  and the CLI's own output. A measuring instrument, not a chat app.

**Status: both build and run in the simulator (CI compiles them there), and both have run on a
12 GB iPhone.** A contributor built the app from the v0.33.0 tag, signed by a free Personal Team with
the memory limit entitlement alone, and on the phone it loaded Qwen3.6-35B-A3B Q4_K_M with the plan
(about 65 s), drew the Plan screen and chatted at 2.6-3.0 tok/s against the 2.9 the plan predicted,
with no crash (#235). Earlier harness runs on the same phone are what #304, #312, #318 and #320
corrected. It is experimental: two turns of chat are not a pressure-stability guarantee.

## The app

```
app/
  Bridge/       EngineBridge.mm: run() on a thread, stop(), and the server's network for
                Hugging Face and a link's size on URLSession (the desktop runs curl). The only
                Objective-C++.
  Sources/
    App/        the entry point, the four tabs and the links between them, the status line
    Engine/     the server's life in the app: paths, launch, key, the state the screens observe
    Chat/       the conversations over the server, the transcript, the composer, the answer's
                Markdown theme, choose-from-options
    Models/     one list of what is here and what the catalog offers, Hugging Face, a link, a file
    Plan/       the verdict and its three figures, the planner's measurements and decisions
    Settings/   the engine parameters a form offers, one card per group of GET /api/params, and
                the app's own
    Metrics/    the live card, the recorded runs, the engine's log: opened from Settings while
                the Metrics switch is on
    Design/     the visual language (docs/design.md): the tokens, Inter and JetBrains Mono, the
                boxes, the logo
  MarkdownUI/   the image providers that load nothing, compiled into the vendored MarkdownUI
                in place of its network ones
  Resources/    the app icon, rendered from the logo by make-icon.swift; Fonts/, the bundled
                faces with their SIL OFL licences
BmoeKit/        the client side with no UI: the API client, the event stream, the conversations'
                shapes, the transcript the events build and Answer again, the move of the old
                local conversations, the decide request, downloads and their check, the models
                list, the settings cards, the plan's figures, metrics files. `swift test` runs it.
```

The screens follow [docs/screens.md](../../docs/screens.md) as the web UI does: Models is one list
where the files the server lists and the catalog's entries are merged by file, each row with one
state and one primary action, and with nothing on the device a first visit opens there on three
steps; Hugging Face is Find, Pick, Check, Download with the server's verdict as the headline; a
direct https link becomes a catalog entry on the server (`POST /api/models/url`, its sizes from a
`HEAD` the bridge asks) and its files download here like any other entry's. Delete goes through the
server (`DELETE /api/models/local/<name>`), which removes a split set whole. The status line shows
the running downloads and, when the context is below what a client of the server sends and the
server serves the network or has refused a request, a warning with a link that lands on that
setting and a ✕ that dismisses it for that model and context (a refusal brings it back); the same
line stays in the "Serve on the network" card. Plan opens on its verdict and three figures with
Apply and Measure again; Settings opens with **Serve on the network**, a card whose header says the
state even while it is closed (Off, or On and the first address, or On but not on a network); inside
are the switch, each address with Copy, the API key masked with Show, Copy and New key (which
restarts the engine on the new key), the base URL a client takes, and that prompts and answers
cross the network unencrypted. Then **Appearance**: the theme (System, Light or Dark, applied at
once; the web UI's choice) and the Metrics switch. Then one card per engine group, every card
closed until opened, holding the rows the parameter table offers on a form (`form`) and any the
person set, with a chip per row for where its value came from, and links from Plan that open a
row's card and land on it.
Metrics is not a tab: the **Metrics** switch in Appearance (off by default) records a per-token CSV
for every loaded model and adds the Metrics entry to Settings. "This app" starts and stops the
engine.

### The chat

The app starts its server with `--chat-only`: the conversations are the server's
(`/api/conversations`, [docs/agent.md](../../docs/agent.md)), the answer is written in the server,
and the screen follows it on the event stream (`conversation`, `item`, `delta`), but they are plain
chat, with no tools, no workspaces and no tasks. The agent is the desktop's: a small model on a
phone is not one. The transcript shows the person's messages, the model's answers as Markdown with
their thinking folded above, and the server's notices. The Markdown is GitHub-flavoured (tables,
task lists, strikethrough, autolinks), rendered by [MarkdownUI](https://github.com/gonzalezreal/swift-markdown-ui)
over cmark-gfm in the app's tokens: Inter, JetBrains Mono for code on the second surface, tables
with borders in the line token that scroll sideways when wider than the screen, as code blocks do.
Nothing in an answer runs or fetches: raw HTML is shown as text, an image is not loaded (MarkdownUI
is built without its NetworkImage dependency; `app/MarkdownUI/` draws images as nothing, their alt
text kept for VoiceOver), and a link opens only when tapped. Holding a message offers Copy and Edit and
send again; under each answer is Copy, and under the last one **Answer again**, which sends the
last question again as it was (the server drops the old answer). Stop stops the answer. The
conversations sheet lists them newest first; swipe a row to rename or delete it. With no model
loaded the empty chat says to load one, with the way to Models. Choose from options stays: the
model picks one option from one read of the prompt, shown as a card under the conversation and not
kept, since a decision is not a turn. An answer holds the background time iOS grants, so one nearly
done when the person switches apps still finishes.

- **The conversations this app used to keep** on the device are moved to the server once, at the
  first start that reaches it (`POST /api/conversations` with their messages as items); a local
  copy is deleted only when the server's answer carries every item, so nothing is lost on a server
  that does not take them.

What the app does itself, and why: it downloads the catalog's files with the system's background
transfers (they go on while the app is away; the server has no `curl` here and only lists what
lands; a stopped one keeps the system's resume data, so its row reads Resume (X of Y GB) beside
Delete partial download), checks a finished file against its catalog `sha256` before it becomes the
model (read back in chunks, off the transfers' queue; the file waits under a name of its check's own
meanwhile, and a mismatch deletes it and says so; a start is refused when the entry does not fit in
the free space), writes a metrics CSV per loaded model from the event stream while Metrics is on,
and keeps the network key in the keychain. Nothing in it names an engine parameter: the settings
are rendered from the parameter table, the plan from the planner.

Where things live: models and metrics files in the app's documents folder, which the Files app and
the Finder show (copy a `.gguf` in, a CSV out; it is kept out of the device backup); the server's
settings and its conversations in the app's private Application Support.

### The look

Slate and cyan, light and dark following the system or the Theme in Settings (the tokens are one
file, `Sources/Design/Theme.swift`, and [docs/design.md](../../docs/design.md) is their source):
Inter for every word, JetBrains Mono for code, paths and keys, both bundled from their official
releases with their licences; 8-point rounded boxes with a 1-point line and a soft shadow on cards;
the system's navigation, tab bars, lists, forms, sheets and menus, tinted. The pixel logo is unchanged.

The server listens on `127.0.0.1:8765` with a random key per launch, since every app on the
device can reach loopback. **Serve on the network** in Settings restarts it on all addresses
with a key kept in the keychain and shown there, for an OpenAI client on another machine; it
serves while the app is open, since iOS suspends an app in the background. A coding agent needs a
larger Context than a chat (its first request alone is about 10k tokens): set Context to 16384 or
more before pointing one at the phone, at the cost of expert cache
([sizing the context](../../docs/server-api.md#sizing-the-context-for-a-client)). That address and
key are what another app needs to connect; the desktop's settings writer for coding agents is not
offered on a phone.

## Build

Needs Xcode (the command line tools alone carry no iOS SDK) and CMake.

```bash
scripts/build-ios.sh            # the simulator, unsigned: BigMoeOnEdge.app and bmoe-harness.app
BMOE_IOS_TEAM=<team id> BMOE_IOS_APP_BUNDLE_ID=<your.own.id> scripts/build-ios.sh device
swift test --package-path examples/ios/BmoeKit   # the app's client side, on the Mac
```

The chat's Markdown libraries are vendored and built as CMake targets beside the app, since the app
is not a SwiftPM package: MarkdownUI 2.4.1 (`third_party/swift-markdown-ui`, MIT) as a Swift static
library, and swift-cmark 0.5.0 (`third_party/swift-cmark`, cmark-gfm, BSD-2-Clause) as a C static
library whose module maps MarkdownUI imports. Both are upstream's files unmodified, at the tags and
commits `app/CMakeLists.txt` records; `THIRD_PARTY_LICENSES` carries their licences.

The script generates an Xcode project (`build-ios-sim/bigmoeonedge.xcodeproj`), which opens in
Xcode to run, debug and profile either app. `BMOE_IOS_TARGETS=bmoe-app` builds one of the two.

A device build is signed by your own Apple development team, and each bundle identifier
(`BMOE_IOS_APP_BUNDLE_ID`, `BMOE_IOS_BUNDLE_ID` for the harness) has to be unique to it.
Entitlements must be granted to the team, or the build does not sign; the app and the harness ask
for the same ones:

- `BMOE_IOS_MEMORY_ENTITLEMENTS=ON` asks for the increased memory limit (`memory.entitlements`). A
  free Personal Team is granted it (with a seven-day profile), and it is what a streamed 35B needed
  on a 12 GB phone.
- `BMOE_IOS_EXTENDED_ADDRESSING=ON` also asks for the extended address space
  (`memory-extended.entitlements`), which a Personal Team is refused. Nothing measured so far has
  needed it.

The iOS sandbox can deny the wire-limit queries available on macOS. An unreadable limit now stays
unmeasured: the automatic plan keeps its ordinary memory budget and margin, requests cache locks,
and enables fitting to the kernel's actual grants. Known finite limits, including zero, still apply.
The plan reports the pending request separately from a protected cache. A refused lock can still
leave ordinary memory; the plan alone does not establish that a run will stay resident.

The model-derived cache floor is initialized before startup's complete prefill-device confirmation,
so fitting can handle a partial grant there as well as during generation.

## Run in the simulator

```bash
xcrun simctl boot <device>                        # one of `xcrun simctl list devices`
xcrun simctl install booted <BigMoeOnEdge.app>    # the path the build script printed
cp model.gguf "$(xcrun simctl get_app_container booted io.bigmoeonedge.app data)/Documents/"
xcrun simctl launch --console-pty booted io.bigmoeonedge.app
```

The simulator shares the Mac's loopback: if a desktop `bmoe-server` holds port 8765, the app
says so on its status line. Launch it on another port with `-BmoePort 8799` after the bundle id.

**The simulator proves that it builds and runs, not what a device would do.** It runs on the Mac's
kernel with the Mac's memory, storage and GPU, and the planner reads that kernel's facts (no
per-process share, a compressor): it plans for the Mac. Do not read a plan or a throughput figure
from it.

## The harness

```bash
cp model.gguf "$(xcrun simctl get_app_container booted io.bigmoeonedge.harness data)/Documents/"
# Everything after --bmoe goes to bmoe-cli; relative paths resolve against the documents folder.
xcrun simctl launch --console booted io.bigmoeonedge.harness --bmoe --auto --plan -m model.gguf
```

On a device, install the signed build from Xcode or with `xcrun devicectl device install app`,
copy a model in with the Files app or the Finder, and type the command line in the app or script
it: `xcrun devicectl device process launch --console --device <id> <your.own.id> --bmoe --auto --plan -m model.gguf`.

## The runs a report needs, in one command

`scripts/ios-runs.sh` runs the plan, a run of it, and the same run with the dense set left mapped,
each in a fresh process of the harness, and brings the console output and per-token CSV of each
back to the Mac with a `summary.txt` on top (tok/s, the decode split, major faults, dense residency):

```bash
BMOE_IOS_DEVICE=<id> BMOE_IOS_BUNDLE_ID=<your.own.id> scripts/ios-runs.sh device model.gguf
RUNS="plan auto mmap t2 t4" ...   # add the same run at 2 and 4 threads
RUNS="c1000 c1500 c2000" N_PREDICT=128 ...   # the plan's run with the cache fixed at each size
```

A `c<MiB>` run keeps everything the plan chose except the expert cache, which it sets to that size
(`--cache-mb`, `--force-cache`). Run at a few sizes, it shows where a larger cache stops paying and
where the device starts taking memory back.

`scripts/ios-runs.sh sim model.gguf` does the same in the booted simulator, which checks the
harness and nothing else.

The batch continues through every requested run, including attempts to retrieve partial CSVs after
a launch failure, then exits `1` if any launch command failed and `0` if all launches succeeded.
Each run's exact launch exit status remains in its console log and `summary.txt`. CSV retrieval
is best effort and does not set the batch status; a plan-only run needs no CSV. The status is the
launcher command's result, so inspect the console log for the harness's own exit marker as well.

## Limits

- One process: the device probes run in it (there is no second process to isolate a failing
  driver in), and an engine crash ends the app.
- The harness: `--session` reads requests from a standard input an app does not have, and a
  command line the CLI rejects by exiting (a flag with its value missing) ends the app.
- The process's region list is not offered to an app, so the dense residency sensor reads the
  mapping at each tensor's own address instead, which works here as everywhere else.
