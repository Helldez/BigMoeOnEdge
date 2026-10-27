# The desktop app, its installer and its updates

One installer per operating system. It installs the app, and the app fetches the compute backend
for the machine's graphics card the first time it needs it. This page says what each piece is, where
it comes from, and how a download is checked.

## The app

`bmoe-desktop` is a native window around the web UI: the system's own web view (WebView2 on
Windows, WKWebView on macOS, WebKitGTK on Linux), through the `webview` library (MIT,
`third_party/webview`). It starts `bmoe-server` as a child process, so a driver that ends the
engine does not end the window, and restarts it when it must (after a backend is installed, after a
change of port). Closing the window keeps the app in the tray (the menu bar on macOS) when the
person chose that; Quit stops the server.

The window's page is the web UI the server serves, so the app and a browser show the same thing.
The app adds a few things a browser cannot do, as a JavaScript object `window.bmoeDesktop` that the
UI uses where it exists:

| Call | What it does |
|---|---|
| `info()` | `{version, target, prefs}` |
| `pickFolder()` | the system's folder dialog; the chosen path or `null` |
| `notify(title, body)` | a desktop notification |
| `openExternal(url)` | the default browser |
| `setPrefs(prefs)` | `{closeToTray, port, notifications, uiScale}`; a new port restarts the server |
| `restartServer()` | after a backend install |
| `applyUpdate(path)` | runs a downloaded, checked installer and quits |

## The installer

| System | File | Installs to |
|---|---|---|
| Windows (x64, ARM64) | `BigMoeOnEdge-<v>-windows-<arch>-setup.exe` | the user's programs folder, no administrator rights |
| macOS (Apple silicon) | `BigMoeOnEdge-<v>-macos-arm64.dmg` | drag to Applications |
| Linux (x86_64, aarch64) | `BigMoeOnEdge-<v>-linux-<arch>.AppImage` | runs where it is |

Each carries the app, the server, the CPU backend and the backends that need nothing but the
system (Metal on macOS; Vulkan and, on Windows ARM64, OpenCL elsewhere). A backend that needs a
vendor's runtime (CUDA, HIP) is a **backend pack**, fetched on first start.

For a terminal, scripts, servers and coding agents, one line installs the command-line tools:

```sh
curl -fsSL https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/install.sh | sh
```

```powershell
irm https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/install.ps1 | iex
```

Both read the feed, pick the archive for the machine, check its SHA-256 and unpack it into the
user's own folder; neither needs administrator rights.

## The feed

Every release carries `feed.json` and its signature `feed.json.sig` (Ed25519, base64). The release
workflow signs the feed with the key in the repository's secrets; the server checks it against the
public key compiled in (`server/feed_key.h`) before it believes anything the feed says, and every
file it downloads against the SHA-256 and size the feed gives.

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

`target` is the build's own (`macos-arm64`, `linux-x86_64`, `windows-x86_64`, ...). `needs_any`
lists the driver libraries one of which must load for the pack to be of use: that is how the server
knows a pack applies to this machine without naming a vendor in code.

## Backend packs

`GET /api/backends` says which backends registered, which packs the feed offers for this target,
which of them apply here, and which are installed. `POST /api/backends/<id>/install` downloads a
pack, checks it, and unpacks it into `<data>/backends/<id>/`; the answer says the server must
restart, and the app restarts it. The engine loads backends from the executable's folder and from
each folder under `<data>/backends/`, so a read-only install (an AppImage, an app bundle) takes
packs too.

## Updates

Nothing checks in the background. `GET /api/update` reads the feed when a person asks (a button in
Settings) and answers `{current, latest, available, notes}`; `POST /api/update/download` fetches the
installer for this target and checks it; the app then runs it and quits. Starting the app, loading
a model and chatting never use the network.
