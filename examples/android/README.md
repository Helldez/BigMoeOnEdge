# BigMoeOnEdge for Android

The phone app: chat with a mixture-of-experts model larger than the phone's memory, streamed from
storage, and hand the phone's hardware to any OpenAI-compatible client on the network.

It is a client of `bmoe-server`, the same engine process the desktop runs
([docs/application-framework.md](../../docs/application-framework.md)). The APK carries the server
as `libbmoe-server.so`; a foreground service starts it on loopback with the app's data directory,
and the five screens (Chat, Models, Plan, Settings, Metrics) speak its HTTP API
([docs/server-api.md](../../docs/server-api.md)), exactly as the web UI does on a laptop. A process
of its own is the crash boundary: a driver that ends the engine does not end the app.

Nothing about a prompt or an answer leaves the phone unless **Serve on the network** is on.

## Build

1. Cross-compile and stage the engine binaries (needs the Android NDK):

   ```powershell
   pwsh ../../scripts/build-android.ps1
   ```

   This fills `app/src/main/jniLibs/arm64-v8a/` with `libbmoe-server.so`, `libbmoe-cli.so` and
   the `libllama`/`libggml` shared libraries.

   For the NPU prefill the engine needs the Hexagon backend, which builds inside upstream's
   Snapdragon toolchain container instead (it carries the Hexagon SDK), then stages the same way:

   ```bash
   docker run --rm -v <repo>:/workspace ghcr.io/snapdragon-toolchain/arm64-android:v0.7 \
       bash /workspace/scripts/build-hexagon-android.sh
   powershell -File ../../scripts/stage-hexagon-jnilibs.ps1
   ```

   The release APK is built this way by CI, with a skel for every NPU generation the backend
   supports (v73 to v81). A local APK built with `build-android.ps1` has no Hexagon backend, and
   Settings says so.

2. Build and install the APK. Open this folder in Android Studio, or use the committed Gradle
   wrapper directly. The app has two distribution flavors (see below); build the one you want:

   ```bash
   ./gradlew assembleDevDebug
   adb install app/build/outputs/apk/dev/debug/app-dev-debug.apk
   ```

   The app's pure-Kotlin rules (the event-stream parser, the chat protocol, the parameter table,
   the catalog status, the conversation files) have JVM unit tests under `app/src/test`; CI runs
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

The Models screen lists what the engine sees: its own directory (the app's internal storage, where
in-app downloads and imports land) and the other places a model can sit on the phone, which the app
hands the server at launch (`--scan-dir`: the app's external files directory, and on the dev flavor
`/data/local/tmp/bmoe` and Downloads). The server groups split sets, reads each model's
architecture and says whether this build streams it; the app reads no gguf itself. Nothing below
needs a storage permission except the last option.

1. **Built-in catalog** (both flavors): the models this engine is measured on, one tap each. The
   catalog is the project's one data file, `catalog/models.json`, shipped in the APK and read by
   the server; the app does the transfers itself (a phone has no `curl` for the server to run), in
   a foreground worker that survives the app being killed and resumes an interrupted transfer.
2. **Any other model**: under **Other model**, paste a direct gguf URL (a Hugging Face
   `…/resolve/main/model.gguf` link), or pick a `.gguf` already on the device to import it.
3. **adb push** (dev flavor only, needs all-files access, which the dev build requests on Refresh):

   ```bash
   adb push Qwen3-30B-A3B-Q4_K_M.gguf /data/local/tmp/bmoe/   # a real filesystem: O_DIRECT works
   adb push Qwen3-30B-A3B-Q4_K_M.gguf /sdcard/Download/        # emulated: buffered I/O
   ```

In-app downloads and imports land on a real f2fs/ext4 volume, so the streamed expert reads use
O_DIRECT at full speed; a model read from the emulated external directories falls back to buffered
I/O. A download needs free space equal to the model size, with no temporary second copy.

### Sharded models (gpt-oss-120b, DeepSeek V4 Flash, Qwen3.8-Flash-Next)

Models above Hugging Face's 50 GB per-file limit ship as several shard files
(`-00001-of-0000N.gguf`). The engine streams a split set natively: the shards download one at a
time (each resumable), the row shows one progress bar over the set, and the model list offers the
set once, by its first shard, which is the file the engine opens. For adb-pushed models push every
shard to the same directory. Mind the space: DeepSeek V4 Flash UD-IQ2_M is ~91 GB on disk,
Qwen3.8-Flash-Next UD-IQ3_XXS ~82 GB and its Q2_K build ~80 GB.

Qwen3.8-Flash-Next needs **Dense weights = ahwb** (pinned, dma-buf) in Settings. Its dense side is
4.3 GB and every token walks it: with `anon` the kernel swaps it to zram and single tokens stall
for 10-20 s, with `mmap` it is refaulted from flash every token. Its 51B n-gram table is held back
automatically and stays mmap'd whatever the setting. The planner chooses this where it can; the
catalog's Q2_K build has a 2.4 GB dense side instead, which is ~2 GB the expert cache can have back.

## Loading, and the plan

**Load** on the Models screen opens the model. With **Plan before every load** on (the Plan
screen; on by default), the engine first measures the phone (storage read rate, what memory it can
hold and lock, each compute device in a process of its own) and chooses the streaming settings for
that model, then loads. The Plan screen shows each decision with the fact behind it, lets you
measure again by hand and apply the result; a value you set yourself in Settings is never
overridden. A model is unloaded after ten idle minutes to give the phone its memory back; the next
message reloads it.

## Settings

Every engine tunable comes from the engine's parameter table (`GET /api/params`): the groups, the
rows, their bounds and help text. The app names none of them, so a knob added to the engine appears
here with no app change, and the same setting means the same thing on the desktop. Each group shows
its basic rows, the recommended configuration; everything advanced, experimental or debug sits in
the **Advanced** section at the end, folded, grouped the same way and marked with its level. They
ship in the release build on purpose, because measuring them on hardware other than the one test
phone is part of what this app is for. A value you set is marked and can be reset to the engine's
default; one that is fixed while a model is loaded asks for a reload.

The app's own settings close the page:

- **Serve on the network**: the engine listens on every interface with a key the page shows. Any
  OpenAI-compatible client then uses the phone at `http://<phone IP>:8765/v1` with
  `Authorization: Bearer <key>`: a laptop's coding agent, Open WebUI, a script. Prompts and answers
  cross the network in clear; use it on a network you trust. Changing it restarts the engine.

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
- **Metrics CSV**: one per-token file per loaded model, for the Metrics screen and for sharing.

The prefill device (`prefill-device`) is a row like any other, and the planner chooses it: it
measures every compute device the build carries (a GPU, a Snapdragon's NPU through the Hexagon
backend of the release APK) on the prefill's wide matmul and arms the fastest, for the prefill
only; decode stays on the cores. The Plan screen says which it chose and why. Nothing in the app
names a device. See `../../docs/npu-prefill.md`.

## Metrics

The Metrics screen shows the live card for the current or last generation (the rate, and each
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

The switch above the prompt turns it into a question and adds a field for options, one per line.
The model picks one without writing an answer: the options are lettered, the whole request is one
prefill with no decode (`POST /api/decide`), and each option comes back with the probability the
model put on it (they need not add up to 100%; the rest is the model being unsure). A decision is
kept in the conversation but never sent back to the model as history. The engine side is
[docs/decide.md](../../docs/decide.md).
