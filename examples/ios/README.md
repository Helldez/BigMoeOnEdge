# BigMoeOnEdge on iOS

Two apps, one engine:

- **The app** (`app/`, `BmoeKit/`): the five screens every UI of this project has (chat, models,
  plan, settings, metrics, [docs/screens.md](../../docs/screens.md)) in SwiftUI, over
  `bmoe-server`. An app on iOS may not start a process, so the app links the server as a library
  (`server/server.h`) and runs it on a thread of its own, then speaks the same HTTP API to it on
  loopback that the web UI and the Android app speak ([docs/application-framework.md](../../docs/application-framework.md)).
- **The harness** (`harness/`): `bmoe-cli` compiled into an app, one field for the command line
  and the CLI's own output. A measuring instrument, not a chat app.

**Status: both build and run in the simulator (CI compiles them there); neither has run on a
device.** What the planner reads on real hardware (the process's memory share, whether memory can
be locked, whether a model larger than memory can be mapped at all) is unmeasured.

## The app

```
app/
  Bridge/       EngineBridge.mm: run() on a thread, stop(), and the server's network for
                Hugging Face on URLSession (the desktop runs curl). The only Objective-C++.
  Sources/
    App/        the entry point, the five tabs, the status line
    Engine/     the server's life in the app: paths, launch, key, the state the screens observe
    Chat/       conversations, the turn in flight, Markdown, choose-from-options
    Models/     on the device, the catalog and its downloads, Hugging Face, import a file
    Plan/       the planner's measurements and decisions
    Settings/   every engine parameter, rendered from GET /api/params, and the app's own
    Metrics/    the live card, the recorded runs, the engine's log
    Design/     the visual language (docs/design.md): palette, type, boxes, the logo
  Resources/    the app icon, rendered from the logo by make-icon.swift
BmoeKit/        the client side with no UI: the API client, the event stream, the chat protocol,
                the decide request, conversations, downloads, metrics files. `swift test` runs it.
```

What the app does itself, and why: it holds the conversations (the server keeps none), downloads
the catalog's files with the system's background transfers (they go on while the app is away;
the server has no `curl` here and only lists what lands), writes a metrics CSV per loaded model
from the event stream, and keeps the network key in the keychain. Nothing in it names an engine
parameter: the settings are rendered from the parameter table, the plan from the planner.

Where things live: models and metrics files in the app's documents folder, which the Files app
and the Finder show (copy a `.gguf` in, a CSV out; it is kept out of the device backup); the
server's settings and the conversations in the app's private Application Support.

The server listens on `127.0.0.1:8765` with a random key per launch, since every app on the
device can reach loopback. **Serve on the network** in Settings restarts it on all addresses
with a key kept in the keychain and shown there, for an OpenAI client on another machine; it
serves while the app is open, since iOS suspends an app in the background.

## Build

Needs Xcode (the command line tools alone carry no iOS SDK) and CMake.

```bash
scripts/build-ios.sh            # the simulator, unsigned: BigMoeOnEdge.app and bmoe-harness.app
BMOE_IOS_TEAM=<team id> BMOE_IOS_APP_BUNDLE_ID=<your.own.id> scripts/build-ios.sh device
swift test --package-path examples/ios/BmoeKit   # the app's client side, on the Mac
```

The script generates an Xcode project (`build-ios-sim/bigmoeonedge.xcodeproj`), which opens in
Xcode to run, debug and profile either app. `BMOE_IOS_TARGETS=bmoe-app` builds one of the two.

A device build is signed by your own Apple development team, and each bundle identifier
(`BMOE_IOS_APP_BUNDLE_ID`, `BMOE_IOS_BUNDLE_ID` for the harness) has to be unique to it.
`BMOE_IOS_MEMORY_ENTITLEMENTS=ON` asks for the increased memory limit and the extended address
space (`memory.entitlements`); both must be granted to the team, or the build does not sign.

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
kernel with the Mac's memory, storage and GPU, while the build still describes the machine as one
that ends a process for holding too much. Do not read a plan or a throughput figure from it.

## The harness

```bash
cp model.gguf "$(xcrun simctl get_app_container booted io.bigmoeonedge.harness data)/Documents/"
# Everything after --bmoe goes to bmoe-cli; relative paths resolve against the documents folder.
xcrun simctl launch --console booted io.bigmoeonedge.harness --bmoe --auto --plan -m model.gguf
```

On a device, install the signed build from Xcode or with `xcrun devicectl device install app`,
copy a model in with the Files app or the Finder, and type the command line in the app or script
it: `xcrun devicectl device process launch --console --device <id> <your.own.id> --bmoe --auto --plan -m model.gguf`.

## Limits

- One process: the device probes run in it (there is no second process to isolate a failing
  driver in), and an engine crash ends the app.
- The harness: `--session` reads requests from a standard input an app does not have, and a
  command line the CLI rejects by exiting (a flag with its value missing) ends the app.
- Which regions of the model's mapping are resident is not reported here.
