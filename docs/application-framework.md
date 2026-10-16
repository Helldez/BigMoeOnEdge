# The application framework

One engine, one application layer, a native UI per operating system. This page says what the
application layer is, how each platform reaches it, and what that decides for every UI.

## The decision: the server is the framework

`bmoe-server` already holds everything an application needs that is not the engine itself: the
engine host with its one session and its turn line, the model library with the catalog and the
downloads, the settings in layers rendered from the parameter table, the planner adapter, the event
stream, the OpenAI-compatible chat protocol, and the access rules. All of it is policy over the
engine's public headers, and all of it is reachable through one documented API,
[server-api.md](server-api.md).

So the framework is not a second thing beside the server: it *is* the server, built as a library
(`bmoe_server`, `server/server.h`) with the executable as the process around it. Every UI of this
project speaks that API, and nothing else:

| Platform | How the UI reaches the server | Why that way |
|---|---|---|
| Windows, macOS, Linux | `bmoe-server` as its own process; the web UI and any OpenAI client over HTTP on loopback, or on the network with `--api-key` | A process of its own is the crash boundary: a driver that ends the engine does not end the UI |
| Android | The same binary, shipped in the APK as `libbmoe-server.so` and started by the app as a child process, the way the CLI is today; the app is an HTTP client of loopback, always with a key (on a phone every installed app reaches loopback) | Same crash boundary, same API, and the phone serves the network with the same flag the desktop uses |
| iOS | An app may not start a process: the app links `bmoe_server` and calls `run()` on a thread, then speaks to loopback inside its own process | The one platform without the boundary still gets the one API, with no second implementation |

What follows from one API with one implementation:

- **The OpenAI-compatible endpoint exists on every platform at no extra cost.** A phone serving a
  laptop's coding agent is the same product as a laptop serving its own.
- **A knob added to the engine appears in every UI with no UI change.** A UI knows no engine
  parameter by name; it renders what `GET /api/params` describes, as the web UI does today.
- **One catalog, one settings model, one security statement.** `catalog/models.json` is the model
  list everywhere; settings live in the same layers (defaults, plan, launch, user) with the same
  rule that a plan never overrides a person; who may reach the API is decided once, in
  `server/access.h`, and stated once, in [server-api.md](server-api.md#security-model).
- **One set of tests for the application layer.** The policy is tested without HTTP, a session or
  a model (`tests/server_test.cpp`); a UI's own tests cover its screens, not the engine's rules.

Why HTTP on loopback rather than a C interface each UI binds: the API already exists and is
documented; the wire format is what makes the endpoint free everywhere; and a socket is the only
boundary that survives a crash on the other side. The cost is one local HTTP exchange per request
and one event stream per UI, which is microseconds against the tens of milliseconds a token takes.

## What stays platform-specific, and where

The server has one file with platform `#if`s (`server/platform.cpp`): the data directory, the
executable's own directory, opening a browser, and what the compute backends are told before they
load (on Android, the Hexagon backend's polling and host buffer, set only where the environment has
not set them). Everything else platform-shaped is an adapter the
host app supplies or the server does without:

- **Downloads.** The desktop server runs the system `curl`. Android has no `curl`, and the system
  owns background transfers there (a download must survive the app being put away), so the app
  keeps its own downloader and the server only lists what is on disk; iOS is the same with
  `URLSession`. The catalog is still the server's data file.
- **Process isolation for the planner's device probes.** Where a process may start another, each
  compute device is asked in a child process first; iOS probes in-process.
- **Where files live.** The app passes `--data-dir`, `--models-dir` and `--catalog`, and a
  `--scan-dir` for each other place a model can sit on that device; the server compiles no path in,
  and lists what every one of them holds.

## The UIs

Three native UIs, one experience: the five screens (chat, models, plan, settings, metrics) are
specified once, in what they show and what a setting means, and each toolkit renders them its own
way: [screens.md](screens.md). The web UI and the Android app are its first two implementations.

## The order of the work

1. **The server as a library, with a key, built for every platform.** Done: `bmoe_server`,
   `--api-key`, the access policy with its tests, the Android builds carrying `libbmoe-server.so`.
2. **The Android app on the framework** (issue #233). Done in code: the service starts
   `libbmoe-server.so` instead of the CLI and the app is an HTTP client of loopback; the Kotlin
   reimplementations of settings, catalog and telemetry parsing are deleted, not ported; the
   planner, the plan screen and the network switch came with it. What it still owes is a run on a
   phone, and the decide endpoint (#245) for the one feature the move left behind.
3. **iOS on the framework** (issue #235): `run()` on a thread, the same client over loopback.
4. **Desktop and Windows at parity** (issue #234): an installer per OS, the device probes isolated
   in the server, the cache lock and the gates on Windows.

What is not on this list: a second engine host, a chat history held by the server (the
conversation is the client's, and the KV cache is reused by prefix), or a transport other than
HTTP for the phone.
