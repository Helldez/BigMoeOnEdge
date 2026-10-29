# BigMoeOnEdge for Android

The phone app: chat with a mixture-of-experts model larger than the phone's memory, streamed from
storage, and hand the phone's hardware to any OpenAI-compatible client on the network.

It is a client of `bmoe-server`, the same engine process the desktop runs
([docs/application-framework.md](../../docs/application-framework.md)). The APK carries the server
as `libbmoe-server.so`; a foreground service starts it on loopback with the app's data directory,
and its screens (Chat, Models, Plan, Settings, and Metrics from Settings) speak its HTTP API
([docs/server-api.md](../../docs/server-api.md)), exactly as the web UI does on a laptop. A process
of its own is the crash boundary: a driver that ends the engine does not end the app.

Nothing about a prompt or an answer leaves the phone unless **Serve on the network** is on.

The app still needs the network permission, offline too: Android ties every IP socket to it,
loopback included, so the app cannot reach its own engine without it. Being offline is fine (the
loopback interface is always there, airplane mode included); being *denied* the network is not.
Stock Android grants it at install and nobody can revoke it, but GrapheneOS's per-app **Network**
toggle and firewall apps can. The app then says so in the status line, with a button to its system
settings, and a download fails with the same reason instead of retrying; a model can still be
imported from a file.

## Install

Each [release](https://github.com/Helldez/BigMoeOnEdge/releases/latest) carries
`app-dev-release.apk`, signed with the project's stable key, so each version installs over the
previous one. Two ways to get it:

- **[Obtainium][obtainium]**: the link adds this repository to Obtainium, which then offers every
  new release as an update. The entry picks `app-dev-release.apk` (its APK
  filter is `-release\.apk$`) and leaves out `app-dev-debug.apk`, a debug build signed with another
  key that cannot update the release one.
- **By hand**: download `app-dev-release.apk` from the release page and open it; Android asks once to
  allow installs from the browser.

[obtainium]: https://apps.obtainium.imranr.dev/redirect?r=obtainium://app/%7B%22id%22%3A%22io.bigmoeonedge.example.dev%22%2C%22url%22%3A%22https%3A%2F%2Fgithub.com%2FHelldez%2FBigMoeOnEdge%22%2C%22author%22%3A%22Helldez%22%2C%22name%22%3A%22BigMoeOnEdge%22%2C%22additionalSettings%22%3A%22%7B%5C%22apkFilterRegEx%5C%22%3A%5C%22-release%5C%5C%5C%5C.apk%24%5C%22%7D%22%7D

## Build

1. Cross-compile and stage the engine binaries (needs the Android NDK):

   ```powershell
   pwsh ../../scripts/build-android.ps1
   ```

   This fills `app/src/main/jniLibs/arm64-v8a/` with `libbmoe-server.so`, the engine the app
   starts as a child process, and the `libllama`/`libggml` shared libraries it links.
   `libbmoe-cli.so` is still staged beside them, but the app does not start it.

   The release build adds the Hexagon, Vulkan and OpenCL backends. It runs inside upstream's
   Snapdragon toolchain container (it carries the Hexagon SDK; `scripts/android-gpu-deps.sh`
   fetches what Vulkan and OpenCL need), then stages the same way:

   ```bash
   docker run --rm -v <repo>:/workspace ghcr.io/snapdragon-toolchain/arm64-android:v0.7 \
       bash /workspace/scripts/build-hexagon-android.sh
   powershell -File ../../scripts/stage-hexagon-jnilibs.ps1
   ```

   The release APK is built this way by CI, with a skel for every NPU generation the backend
   supports (v73 to v81) and with both GPU backends, so one APK fits every phone and the planner
   measures whichever devices the phone has. A local APK built with `build-android.ps1` has the
   CPU alone.

2. Build and install the APK. Open this folder in Android Studio, or use the committed Gradle
   wrapper directly. The app has two distribution flavors (see below); build the one you want:

   ```bash
   ./gradlew assembleDevDebug
   adb install app/build/outputs/apk/dev/debug/app-dev-debug.apk
   ```

   The app's pure-Kotlin rules (the event-stream parser, the chat protocol, the parameter table,
   the settings form, the catalog status, the merged model list, the plan's figures, the context
   warning, the transcript the conversation events build, Answer again, the conversations' JSON
   shapes and the one-time move of the conversations the app used to keep) have JVM unit tests
   under `app/src/test`; CI runs
   them, and so does `./gradlew testDevDebugUnitTest`.

   Published sideload builds are signed with a stable key instead, so an update installs over
   the previous one rather than being refused. That needs a `keystore.properties` next to `app/`
   (gitignored); without it, builds fall back to debug signing. The APKs attached to a GitHub
   release are built by the `release-apk` workflow from a clean checkout of the tag.

## Flavors

Two build flavors differ only in how a model reaches the device:

- **dev** — sideloaded (this is what CI attaches to releases). Keeps all-files access, so it can
  also read a model adb-pushed to shared storage. Application id `….example.dev`.
- **play** — Play-Store-compliant. No broad storage permission: models come only through the
  in-app downloader or the file picker. `./gradlew assemblePlayDebug`.

Both declare `android:appCategory="game"`. That is a performance decision rather than a claim
about what the app is: vendor layers read the attribute to pick a CPU governor profile, and on the
OxygenOS test device it lifted the foreground ceiling from 1.9/1.65 GHz to the hardware maximum of
3.32/3.80 GHz. Decode is the most CPU-hungry thing a phone does outside a game. The effect is the
vendor's, not Android's, so treat any figure as a per-device measurement.

## Getting a model onto the device

The Models screen is one list, **Your models**: what the engine sees and what the catalog offers,
merged by file, so a model the catalog downloaded is one row (`docs/screens.md`, "Models"). Each row
has one state and one action for it: Available (Download), Downloading (progress and Cancel),
Partly downloaded (Resume, Delete partial), On this device (Load), Loaded (Reload, Unload, Go to
Chat), Shards missing (Download, where an entry has the rest). Delete removes a model's files, a
split set as a unit, after asking (the server deletes in its models directory, the app elsewhere
where it may write); Forget takes an entry added from a repository or a link off the list and
leaves its files. Catalog entries with nothing on the phone sit at the bottom as suggestions, and
files that are not models (a projector, a draft head, by their headers) are listed apart with no
Load. With nothing on the phone the screen is three steps (find, check it fits, download then
load), and the app opens on it instead of an empty Chat.

The list covers the engine's own directory (the app's internal storage, where in-app downloads and
imports land) and the other places a model can sit on the phone, which the app hands the server at
launch (`--scan-dir`: the app's external files directory, and on the dev flavor
`/data/local/tmp/bmoe` and Downloads), each with the folders directly inside it; **Where models
live**, at the foot of the screen, names them. The server groups split sets, reads each model's
architecture, says whether this build streams it and whether the file is a model at all; the app
reads no gguf itself. Nothing below needs a storage permission except the last option.

1. **Built-in catalog** (both flavors): the models this engine is measured on, one tap each. The
   catalog is the project's one data file, `catalog/models.json`, shipped in the APK and read by
   the server; the app does the transfers itself (a phone has no `curl` for the server to run), in
   a foreground worker that survives the app being killed and resumes an interrupted transfer.
   Its notification shows the progress as the row does. A file whose catalog row names a `sha256`
   is checked against it before it becomes the model; a mismatch deletes the download and says so.
   An entry whose file name is not a bare file name is not offered.
2. **From Hugging Face**: a model's name, a repository id or its URL, in four steps shown as such:
   Find, Pick, Check (the server reads the header before any download and answers with a verdict
   the card shows as its headline, coloured by the server's code), Download (the model becomes a
   catalog entry and its row in Your models carries the download).
3. **Any other model**: paste a direct https link to a gguf, or to any shard of a split set. The
   server makes it a catalog entry (`POST /api/models/url`), and the app downloads its files like
   any entry's, with Resume and one progress bar over the set. The server has no `curl` on the
   phone to ask the host for sizes, so they are left unknown and a file counts whole once it is
   there. Nothing checks the link before the download, so a link to nothing ends at once with the
   host's answer on its row (a definite `4xx` is never retried). A link the server refuses shows its
   reason. Or pick a `.gguf` already on the device to import it.
4. **adb push** (dev flavor only, needs all-files access, which the dev build requests on Refresh):

   ```bash
   adb push Qwen3-30B-A3B-Q4_K_M.gguf /data/local/tmp/bmoe/   # a real filesystem: O_DIRECT works
   adb push Qwen3-30B-A3B-Q4_K_M.gguf /sdcard/Download/        # emulated: buffered I/O
   ```

In-app downloads and imports land on a real f2fs/ext4 volume, so the streamed expert reads use
O_DIRECT at full speed; a model read from the emulated external directories falls back to buffered
I/O. A download needs free space equal to the model size, with no temporary second copy.

**Resume.** A download writes `<name>.part` and renames it once whole, so a stopped one keeps its
bytes. A dropped connection is retried on its own with backoff, each attempt continuing from the
`.part` with a `Range` request; a host that answers a definite `4xx` (no such file, a token it
wants) ends the download at once, the reason on its row. Whatever stopped it (the retries ran out, you pressed **Cancel**, or
the app or the phone restarted), the row then reads **Resume (X of Y GB)** with the bytes already
on disk, and pressing it continues from there; the free-space check counts only the bytes still to
fetch. **Delete partial download** on the same row is the one action that throws them away (the
server deletes the entry's `.part` files, in its models directory only). These are the desktop web
UI's words and behaviour too (`docs/server-api.md`, "Models"). A link an older build downloaded on
its own, with no entry, has no row to resume from, so its **Cancel** deletes its partial file.

### Sharded models (gpt-oss-120b, DeepSeek V4 Flash, Qwen3.8-Flash-Next)

Models above Hugging Face's 50 GB per-file limit ship as several shard files
(`-00001-of-0000N.gguf`). The engine streams a split set natively: the repository listing shows
the set as one model by its first shard, the shards download one at a time (each resumable), the
row shows one progress bar over the set, and the model list offers the set once, by that first
shard, which is the file the engine opens. For adb-pushed models push every
shard to the same directory. Mind the space: DeepSeek V4 Flash UD-IQ2_M is ~91 GB on disk,
Qwen3.8-Flash-Next UD-IQ3_XXS ~82 GB and its Q2_K build ~80 GB.

Qwen3.8-Flash-Next needs **Dense weights = pinned** (pinned, dma-buf) in Settings. Its dense side is
4.3 GB and every token walks it: with `anon` the kernel swaps it to zram and single tokens stall
for 10-20 s, with `mmap` it is refaulted from flash every token. Its 51B n-gram table is held back
automatically and stays mmap'd whatever the setting. The planner chooses this where it can; the
catalog's Q2_K build has a 2.4 GB dense side instead, which is ~2 GB the expert cache can have back.

## Loading, and the plan

**Load** on the Models screen opens the model. With **Plan before every load** on (the Plan
screen; on by default), the engine first measures the phone (storage read rate, what memory it can
hold and lock, each compute device in a process of its own) and chooses the streaming settings for
that model, then loads. The Plan screen leads with what the plan decided in one sentence, the
machine, which device takes the prompt and which the tokens, and three figures: decode speed
expected and, after the first answer, measured with the difference; the cache hit rate the plan
credited and the one measured; and the memory held against what the process may hold. The app
reads the plan when a session opens and when an answer ends, whichever screen is open, so the
figures are there when you look. Apply and Measure again sit under them (an error at the top has
its own Measure again); Apply lists the settings it changed, each a link to its row in Settings. Then where the
memory goes, what each compute device does and why, and the decisions grouped, each with the fact
behind it and a link to its setting. Copy diagnostic puts the command and the planner's whole
account on the clipboard for a bug report. The screen shows which stage of the measurement is
running; a value you set yourself in Settings is never overridden. With nothing loaded and no plan
measured, it shows the decisions the settings carry from the last applied plan. A model is unloaded after ten
idle minutes to give the phone its memory back; the next message reloads it.

The status line above the navigation bar, on every screen, says what the engine is doing and shows
the running downloads ("Downloading 42%", or "2 downloads 42%"), which lead to Models. With "Serve
on the network" on, or once a request was refused for the context, a loaded model whose context is
below what a coding agent's first request needs gets a warning under it: the context, the server's
recommendation, that a larger context takes memory from the expert cache, a link to the setting
that raises it, and a ✕ that dismisses it for that model and context (a refusal brings it back).
The same line stays under "Serve on the network" in Settings. A chat refused for a
full context offers the same link beside New chat. The number and the setting come from the
server (`model.ctx_advice`).

## The chat

The app starts its server with `--chat-only`: the conversations are held by the engine process
([docs/agent.md](../../docs/agent.md)), not by the app, so an answer goes on while the app is in
the background and the same conversations are there for any client of the phone's server, but they
are plain chat, with no tools, no workspaces and no tasks. The agent is the desktop's: a small model
on a phone is not one. The drawer lists the conversations, newest first, with a mark on one that is
being answered; a row's menu renames or deletes it.

- **The transcript.** Your messages on the right; long-press one to copy it or to edit it and send
  it again (everything after it is dropped). The model's answers as GitHub-flavoured Markdown
  (tables bordered and scrolling sideways when wide, drawn by multiplatform-markdown-renderer in
  the app's tokens; images are not loaded and HTML is not run), its thinking folded above them,
  with Copy under each and **Answer again** under the last one, which sends the last question
  again as it was (the server drops the old answer). A notice says, in words, why
  something did not happen. Stop ends the answer.
- **The composer.** The prompt, Send, or Stop while an answer is written; the + beside the prompt
  holds **Choose from options** (below). With no model loaded the empty chat says to load one from
  Models.

The conversations earlier versions kept on the phone are handed to the server once, on the first
launch that reaches it: each becomes a server conversation with its messages, and the phone's copy
is deleted only once the server's answer shows every message arrived (an older server that ignored
them keeps nothing and the phone keeps its copy).

## The look

Slate and cyan, light and dark following the system, Material 3 with an 8 dp corner, 1 dp lines and
a soft shadow on cards. The colour values sit in one place, `ui/Theme.kt` (`LightTokens`,
`DarkTokens`), mapped onto Material's colour roles. Text is set in Inter and code in JetBrains Mono,
static instances bundled in `res/font`, both under the SIL Open Font License (its text is in
`THIRD_PARTY_LICENSES`, which the APK carries). The pixel logo is the one mark that keeps its own
style.

## Settings

Every engine tunable comes from the engine's parameter table (`GET /api/params`): the groups, the
rows, their bounds and help text. The app names none of them, so a knob added to the engine appears
here with no app change, and the same setting means the same thing on the desktop. The page is one
card per group, in the table's order, with the group's one line of help; every card starts folded
and lists, in the table's order, the rows the table marks for a form (`form`; a server that does not
send it offers its basic rows), the same rows the web UI and the iOS app show. A row whose value you
set is shown too, so it can be seen and reset. Every other parameter is still the engine's, set
from the command line or the API. Each row carries one chip for where its value came from (default,
from the plan, set by you); a value you set can be reset, or, where the plan decided
another, replaced with **Use plan value**. **Reset all my values** removes yours after asking,
leaving what the engine was launched with and the model. A number shows its unit, the name of a
special value and the usual range. "Needs reload" marks only a row whose value differs from the
loaded model's, and a banner offers the Reload. A link from the Plan screen or the context warning
lands on its row.

**Serve on the network** is the first card on the page, folded like the rest, with its state in
its header even when closed: Off, On with the phone's address, or On but not on a network. Switched
on, the engine listens on every interface, and the card shows the phone's address on its Wi-Fi or
Ethernet network (`http://<phone IP>:8765`, ready to copy and kept current when the network
changes), the API key (masked, with Show, Copy and New key) and the base URL a client takes,
`http://<phone IP>:8765/v1`. Any OpenAI-compatible client then uses it with
`Authorization: Bearer <key>`: a laptop's coding agent, Open WebUI, a script. Prompts and answers
cross the network unencrypted, as the card says; use it on a network you trust. A coding agent needs
a larger Context than a chat: its first request alone is about 10k tokens, so set Context to 16384
or more before pointing one at the phone, at the cost of expert cache
([sizing the context](../../docs/server-api.md#sizing-the-context-for-a-client)); the card shows the
context warning with its Raise button. Changing it restarts the engine, and a model that was loaded
is loaded again once the engine is back.

The engine requires a key even when it is not shared: on a phone, loopback is reachable by every
installed app, and an open server would let any of them read a conversation off the event stream.
The app starts it with a random key per launch, written to a file in its private storage that only
it can read (`--api-key-file`, mode 0600, never on the command line), and sends it on every request;
shared, the key is the persistent one Settings shows, so a client keeps working across restarts.
Before it trusts the server on the port, the app checks it is the one it started: that server
refuses a random key and accepts the launch's. Another program's server on 8765 (an open one, or
one with a key of its own) is reported as holding the port, and nothing is sent to it. The server
is stopped and restarted on a background thread, one step at a time, so a restart never races the
old process for the port and a stop never freezes the screen; a stopped server's late exit report
changes nothing. The event stream has a watchdog: the server pings every 15 s, and a stream silent
for two pings is reconnected.

An **Appearance** card follows **Serve on the network**, folded too:

- **Theme**: System, Light or Dark (the web UI's three). System follows the phone; the choice
  applies at once, status and navigation bars included.
- **Metrics** (off by default): record one per-token CSV file per loaded model. While it is on,
  Settings shows a **Metrics** entry that opens the Metrics screen; back returns to Settings.

The engine cards come next, and a folded **This app** card with Stop and Start engine closes the page.

The prefill device (`prefill-device`) is a row like any other, and the planner chooses it: it
measures every compute device the build carries (the GPU through Vulkan or OpenCL, a Snapdragon's
NPU through Hexagon, all three in the release APK) on the prefill's wide matmul and arms the
fastest, for the prefill only; decode stays on the cores. The Plan screen says which it chose and
why. Nothing in the app names a device. See `../../docs/npu-prefill.md`.

## Metrics

The Metrics screen, opened from Settings while **Metrics** is on, shows the live card for the current or last generation (the rate, and each
token's time split into compute, flash wait, cache management and what nothing claims), then the
CSV files: view one, share one, correlate its columns, or compare two on one axis. The chat shows
the same rate in one line above the prompt while an answer streams. The flash term and the threads
the CPU figures divide by are the server's (`flash_wait_ms`, `compute_threads`, `busy_threads`),
never read from the settings. The fields are documented in `../../docs/telemetry.md` and in the
screen's glossary, whose settings half is the engine's parameter table: each setting a file was
recorded with is shown under the table's label, typed and with its unit, and a run's identity names
every setting that changes the reply and is off its default.

## Expected numbers

On a phone with UFS 4.x storage and ~12 GB RAM, streaming Qwen3-30B-A3B-Q4_K_M with the expert
cache at 4000 MiB, 4 I/O lanes and 4 compute threads, decode settles around **0.55–0.6 s/token
(~1.8 tok/s)**, a model ~1.7× the device RAM, lossless. See `../../docs/benchmark-method.md` for
the procedure and the cache/thread sweep; `../../README.md` has the table across models.

## Choose from options

**Choose from options**, in the + menu beside the prompt, turns it into a question and adds a field
for options, one per line.
The model picks one without writing an answer: the options are lettered, the whole request is one
prefill with no decode (`POST /api/decide`), and each option comes back with the probability the
model put on it (they need not add up to 100%; the rest is the model being unsure). A decision is
not a conversation turn: its answer stays on the chat screen until dismissed and is never sent to
the server's conversation. The engine side is
[docs/decide.md](../../docs/decide.md).
