# The desktop app, its installer and its updates

One installer per operating system. It installs the app, and the app fetches the compute backend
for the machine's graphics card when a person asks for it. This page says what each piece is, where
it comes from, and how a download is checked.

## The app

`bmoe-desktop` (`desktop/`) is a native window around the web UI: the system's own web view
(WebView2 on Windows, WKWebView on macOS, WebKitGTK 4.1 on Linux), through the `webview` library
(MIT, `third_party/webview`, a pinned submodule used header-only). It starts `bmoe-server` as a child
process, so a driver that ends the engine does not end the window, and it restarts it when it must
(after a backend is installed, after a change of port, and on its own when the server ends, up to
three times in two minutes before it waits for a person). The server is told the app's process id
(`--parent-pid`) and stops when the app is gone, however the app ended.

The port comes from the app's preferences, the server's default (8765) when none is set. When that
port is taken the app tries the next ones, starts the server on the first free one, and says so (a
notification, and `notice` in `info()`). It waits for `/api/info` to answer, then shows the UI the
server serves, so the app and a browser show the same thing. The server's output goes to
`<data>/logs/server.log` (the run before is kept as `server.log.1`). One copy of the app runs per
user: a second start brings the first one's window forward and exits.

Closing the window keeps the app in the tray (the menu bar on macOS) when the person chose that
(`closeToTray`, on by default); Quit, in the tray menu, the app menu or Cmd-Q, stops the server. The
tray menu also has **Restart Server**. Per system:

| | macOS | Windows | Linux |
|---|---|---|---|
| Tray | a menu-bar status item; the Dock icon reopens the window | a notification-area icon | an indicator through libayatana-appindicator3, when the build found it; without it there is no tray and closing the window quits |
| Notifications | the notification center (from the app bundle) | a balloon from the tray icon | `notify-send`, where installed |
| Folder dialog | `NSOpenPanel` | `IFileOpenDialog` | GTK's |
| Update | opens the disk image in Finder | runs the setup program | puts the new AppImage in place of the running one and starts it |

The app's preferences are kept in `<data>/desktop.json` (the server's data folder:
`platform::default_data_dir()`, the one function both use).

**Serve on the network** (`serveOnNetwork`, off by default): the server is started with `--host
0.0.0.0 --api-key-file <data>/network-key`, so another computer or a phone uses the engine as an
OpenAI-compatible server with the key. The key is 32 random bytes as hex, made when first needed,
written readable by this user only, and replaced on request (the server then restarts, ending the old
key's access). The window's own page sends the key too, read from `info()`. What reaches files,
commands and keys stays loopback-only whatever the server listens on
([server-api.md](server-api.md), "Security model"); prompts and answers cross the network
unencrypted, which the card says.

### `window.bmoeDesktop`

The app adds a few things a browser cannot do, as a JavaScript object that the UI uses where it
exists. Every call returns a promise; a refused call rejects it with the reason. The native side
answers only a page of the server's own origin (`http://127.0.0.1:<port>`), read from the web view
itself, so a page from anywhere else that found its way into the window gets nothing. Links to other
sites open in the default browser instead of the window.

| Call | What it does |
|---|---|
| `info()` | `{version, target, prefs, port, url, notice, tray, dataDir, log, network}`; `network` is `null`, or `{addresses, key}` while the network is served: each `http://<address>:<port>` on this machine's local networks, and the key |
| `pickFolder()` | the system's folder dialog; the chosen path or `null` |
| `notify(title, body)` | a desktop notification, unless `prefs.notifications` is off |
| `openExternal(url)` | the default browser; `http` and `https` only |
| `setPrefs(prefs)` | `{closeToTray, port, notifications, uiScale, serveOnNetwork}`, any of them; answers the new preferences. `port` is 0 (the default) or 1024-65535; a new port or a change of `serveOnNetwork` restarts the server, the page then moving to it; `uiScale` is 0.9-1.5, applied by the app as the web view's zoom |
| `newNetworkKey()` | replaces the network key; a server serving the network restarts with it |
| `restartServer()` | after a backend install; the page is replaced while the server restarts |
| `applyUpdate(path)` | runs the installer the server downloaded and checked, and quits. Only a file directly inside `<data>/updates/` is run |

## The installer

| System | File | Installs to |
|---|---|---|
| Windows (x64, ARM64) | `BigMoeOnEdge-<tag>-windows-<arch>-setup.exe` | the user's programs folder (`%LOCALAPPDATA%\Programs\BigMoeOnEdge`), no administrator rights; Start-menu entry |
| macOS (Apple silicon) | `BigMoeOnEdge-<tag>-macos-arm64.dmg` | drag to Applications |
| Linux (x86_64, aarch64) | `BigMoeOnEdge-<tag>-linux-<arch>.AppImage` | runs where it is |

Each carries the app, the server, the web UI, the model catalog, the bundled skills (`skills/`), the
CPU backend and the backends that need nothing but the system: Metal on macOS; Vulkan elsewhere,
and OpenCL on Windows ARM64. A backend that needs a vendor's runtime (CUDA, HIP) is a **backend
pack**, installed by the app where it applies. The macOS bundle's main executable is `bmoe-desktop`
(under the bundle's name), with the server beside it in `Contents/MacOS` and the UI, catalog and
skills in `Contents/Resources`, which the app hands the server (`--ui-dir`, `--catalog`,
`--skills-dir`). The AppImage's entry is `bmoe-desktop`; it needs the system's WebKitGTK 4.1
(installed on the common desktops), and without it runs the server and opens the UI in the browser
as before. `--server [options]` runs the server alone, and so does a start with no display.

The Hexagon NPU build for Windows ARM64 (`windows-arm64-npu`) has no installer: its setup is done
by hand (`docs/npu-windows.md`), from its archive.

For a terminal, scripts, servers and coding agents, one line installs the command-line tools
(`bmoe-cli`, and `bmoe-server` with its web UI):

```sh
curl -fsSL https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/install.sh | sh
```

```powershell
irm https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/install.ps1 | iex
```

Both read the feed, pick the archive for the machine, check its size and SHA-256 and unpack it into
the user's own folder (`~/.local/lib/bigmoeonedge`, linked into `~/.local/bin`;
`%LOCALAPPDATA%\Programs\bmoe-cli`), and put the commands on the user's `PATH`; neither needs
administrator rights, and running either again updates. `install.sh` also checks the feed's
signature where the machine has OpenSSL 3; PowerShell has no Ed25519, so `install.ps1` trusts the
feed as HTTPS delivered it and holds the archive to its digest.

## The feed

Every release carries `feed.json` and its signature `feed.json.sig` (Ed25519 over the file's exact
bytes, base64). The release workflow writes the feed from the release's own files
(`scripts/make-feed.py`, with the packs from `packaging/packs.json`) and signs it with the key in the
repository's secrets; the server checks it against the public key compiled in
(`server/feed_key.h`) before it believes anything the feed says, and every file it downloads
against the SHA-256 and size the feed gives. A build with no key reads no feed (`503`,
`code: "no_feed_key"`). The verification is Monocypher's (`third_party/monocypher`, vendored), in
the server only.

```json
{
  "version": "0.39.0",
  "published": "2026-10-28",
  "notes": "https://github.com/Helldez/BigMoeOnEdge/releases/tag/v0.39.0",
  "installers": { "<target>": { "url": "...", "sha256": "...", "size": 0 } },
  "archives":   { "<target>": { "url": "...", "sha256": "...", "size": 0 } },
  "packs": [
    {
      "id": "cuda", "title": "NVIDIA graphics (CUDA)", "target": "<target>",
      "needs_any": ["libcuda.so.1"],
      "url": "...", "sha256": "...", "size": 0
    }
  ]
}
```

`target` is the build's own (`macos-arm64`, `linux-x86_64`, `linux-aarch64`, `windows-x86_64`,
`windows-arm64`), a compile definition of the server and the app. `needs_any` lists the driver
libraries, by the name the system's loader takes, one of which must load for the pack to be of use:
that is how the server knows a pack applies to this machine without naming a vendor in code. The
server refuses a feed whose URLs are not `https`, whose digests are not 64 lowercase hex digits,
whose sizes are not positive, whose pack ids are not lowercase letters, digits and `-`, or whose
library names are paths. The file is written one entry a line, which `install.sh` reads without a
JSON parser.

### Signing the feed

Done once by a maintainer, on a machine of their own:

```sh
openssl genpkey -algorithm ed25519 -out feed-signing-key.pem
openssl pkey -in feed-signing-key.pem -pubout -outform DER | tail -c 32 | base64
```

The second line prints the public key: it goes into `server/feed_key.h` (`kFeedPublicKey`) in a
commit of its own. The private key goes into the repository's secrets as `FEED_SIGNING_KEY` (the
whole PEM file: `gh secret set FEED_SIGNING_KEY < feed-signing-key.pem`), and is then kept offline
or destroyed: a new key can always be made, at the cost of builds before it no longer reading the
feeds after it. The release workflow's `feed` job refuses to go on without the secret, and when the
secret's public half is not the one in `server/feed_key.h`; `publish` then attaches the release's
files and fails rather than publish it without a signed feed. A fork that publishes its own releases
makes its own pair and builds with `-DBMOE_FEED_PUBLIC_KEY=<its public key>` (or edits
`feed_key.h`), and points `kFeedUrl` (or `--feed-url`) at its releases.

## Backend packs

`GET /api/backends` says which backends registered, which packs the last verified feed offers for
this target, which of them apply here, and which are installed; with `?refresh=1` it reads the feed
first (`docs/server-api.md`). `POST /api/backends/<id>/install` downloads a pack, checks it, and
unpacks it into `<data>/backends/<id>/`: the archive is listed with the system's `tar` and refused if
an entry would leave the folder (an absolute path, a drive, a backslash, `..`, a hard link, a device,
anything under a link), unpacked into a staging folder, checked again as files (every link points
inside), and only then moved into place, with a `pack.json` saying which release it came from. A
pack replaced while its libraries are loaded (Windows) is put in place at the next start. The answer
comes once the pack is in place and says the server must restart; the app restarts it. The engine
loads backends from the executable's folder and from each folder under `<data>/backends/`
(`docs/backends.md`), so a read-only install (an AppImage, an app bundle) takes packs too.

Pack archives are flat (their files at the top), so `bmoe-cli`, which loads modules from its own
folder only, takes one when it is extracted there by hand.

## Updates

Nothing checks in the background. `GET /api/update` reads the feed when a person asks (a button in
Settings) and answers `{current, latest, available, notes, published, target, installer}`;
`POST /api/update/download` fetches the installer for this target into `<data>/updates/`, checks it,
and answers with its `path`; the app then runs it (`applyUpdate(path)`) and quits. Starting the app,
loading a model and chatting never use the network.

These routes, and the pack routes, answer only a client on this machine (`403`,
`code: "local_only"`), whatever the server's `--host` and key.

## What is verified where

The rules (feed parsing, signatures, archive checks, pack applicability, the local-only routes, the
module folders, the app's preferences, ports and bridge guard) are unit-tested on every platform
(`tests/feed_test.cpp`, `tests/desktop_test.cpp`). The macOS app was run end to end from a built
bundle (the server started on the default port and on the next one when it was taken, the bridge
answering the UI, the server stopping with the app however it ended). The Windows and Linux system
code, the release workflow's signing and the install scripts on a real release are checked by the
first release run; the release smoke tests start each package.
