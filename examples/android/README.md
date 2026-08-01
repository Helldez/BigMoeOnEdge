# BigMoeOnEdge — Android example

A minimal chat app that validates the throughput claim on a real phone: pick a pushed
`.gguf`, type a prompt, and watch the answer stream in while a live panel shows tok/s and
the per-token compute-vs-flash-I/O split and cache hit rate.

It runs the engine as the `bmoe-cli` binary (shipped as `libbmoe-cli.so`) via
`ProcessBuilder` from a foreground service — no JNI. This is the same pattern used by the
research harness and keeps the app a thin driver over the CLI.

## Build

1. Cross-compile and stage the engine binaries (needs the Android NDK):

   ```powershell
   pwsh ../../scripts/build-android.ps1
   ```

   This fills `app/src/main/jniLibs/arm64-v8a/` with `libbmoe-cli.so` and the
   `libllama`/`libggml` shared libraries.

2. Build and install the APK. Open this folder in Android Studio, or use the committed
   Gradle wrapper directly. The app has two distribution flavors (see below); build the one
   you want:

   ```bash
   ./gradlew assembleDevDebug
   adb install app/build/outputs/apk/dev/debug/app-dev-debug.apk
   ```

   Published sideload builds are signed with a stable key instead, so an update installs over
   the previous one rather than being refused. That needs a `keystore.properties` next to `app/`
   (gitignored — it points at the keystore and holds its passwords); without it, builds fall back
   to debug signing. The APKs attached to a GitHub release are built by the `release-apk`
   workflow from a clean checkout of the tag when the release is published, signed with the same
   stable key from repository secrets — no locally built artifact is uploaded by hand.

## Flavors

Two build flavors differ only in how a model reaches the device:

- **dev** — sideloaded (this is what CI attaches to releases). Keeps all-files access, so it
  can also read a model adb-pushed to shared storage. Application id `…​.example.dev`.
- **play** — Play-Store-compliant. No broad storage permission: models come only through the
  in-app downloader or the file picker. `./gradlew assemblePlayDebug`.

Both declare `android:appCategory="game"`. That is a performance decision rather than a claim
about what the app is: vendor layers read the attribute to pick a CPU governor profile, and on the
OxygenOS test device it lifted the foreground ceiling from 1.9/1.65 GHz to the hardware maximum of
3.32/3.80 GHz. Decode is the most CPU-hungry thing a phone does outside a game. The effect is the
vendor's, not Android's — neutral on stock builds, and Samsung's game service has historically
throttled apps it classifies this way — so treat any figure as a per-device measurement. It cannot
be toggled at runtime; a manifest attribute is fixed at install, and the only lever would be a
per-flavor manifest. **Numbers measured in the app before this landed are not comparable with
numbers measured after it.**

## Getting a model onto the device

The picker lists every MoE `.gguf` it finds (dense models are filtered out by a gguf-header
check). Nothing below needs a storage permission except the last option.

1. **Built-in catalog** (both flavors) — the "Get a model" card offers the models this engine
   is measured on, each a single tap: **Qwen3-30B-A3B-Q4_K_M** (~18.6 GB, the reference model),
   **Qwen3.6-35B-A3B-Q4_K_M** (~22.3 GB, a hybrid attention/SSM MoE, comfortably past device RAM)
   and **Gemma-4-26B-A4B-it-Q4_K_M** (~17 GB). Downloads run in a foreground worker, survive the
   app being killed, resume an interrupted transfer instead of restarting, and appear in the
   picker when done.
2. **Any other model** — under **Other model**, paste a direct gguf URL (e.g. a Hugging Face
   `…/resolve/main/model.gguf` link), or pick a `.gguf` already on the device to import it.

   You do **not** need a special file for **Guess ahead → Model's own head (MTP)**: the catalog's
   Qwen3.6 entry already carries the `nextn` block the MTP head lives in, as do Qwen3.6's ordinary
   quantisations generally. A gguf named `-MTP-` is the same head at a different quantisation.
   On a model with no head — anything that is not Qwen3.5/3.6 — the engine refuses to open rather
   than silently decoding one token at a time, so a wrong file fails immediately and says why.
   **Guess ahead → Repeated text (n-gram)** has no such requirement: it guesses from the text
   rather than from the weights, so it works on every model in the catalog.

   In-app downloads and picker imports both land in the app's internal storage (`filesDir`, a
   real f2fs/ext4 volume), so the streamed expert reads use O_DIRECT at full speed. Only models
   read from the emulated external dirs (adb-pushed to `/sdcard/Download`) fall back to buffered
   I/O. A download needs free space equal to the model size — no temporary second copy.
3. **adb push** (dev flavor only — needs all-files access, which the dev build requests):

   ```bash
   adb push Qwen3-30B-A3B-Q4_K_M.gguf /sdcard/Download/
   # /data/local/tmp/bmoe avoids duplicating a model too big to copy, and is on a real
   # filesystem where O_DIRECT works (the emulated dirs fall back to buffered I/O)
   adb push Qwen3-30B-A3B-Q4_K_M.gguf /data/local/tmp/bmoe/
   ```

   This directory was named `shardllm` before v0.8.0. To keep models already pushed there:

   ```bash
   adb shell mv /data/local/tmp/shardllm /data/local/tmp/bmoe
   ```

### Sharded models (gpt-oss-120b, DeepSeek V4 Flash)

Models above Hugging Face's 50 GB per-file limit ship as several shard files
(`-00001-of-0000N.gguf`). The engine streams a split set natively, so these download in-app
like any other catalog entry: the shards are fetched one at a time (each resumable), the row
shows one progress bar over the whole set, and the model list offers the FIRST shard, which is
the file the engine opens; it finds the siblings next to it. A merged single-file gpt-oss from
an earlier release keeps working and still shows as on-device.

For adb-pushed models the same rule applies: push all shards to the same directory and pass
the first one:

```bash
adb push DeepSeek-V4-Flash-0731-UD-IQ2_M-0000*-of-00003.gguf /data/local/tmp/bmoe/
```

Mind the space: DeepSeek V4 Flash UD-IQ2_M is ~91 GB on disk.

## Expected numbers

On a phone with UFS 4.x storage and ~12 GB RAM, streaming Qwen3-30B-A3B-Q4_K_M with the
expert cache at 4000 MiB, 4 I/O lanes and 4 compute threads, decode settles around
**0.55–0.6 s/token (~1.8 tok/s)** — a model ~1.7× the device RAM, lossless. That 4000 MiB is a
sweep point from the benchmark protocol, not the app default: the app ships a fixed 2000 MiB
expert cache. See `../../docs/benchmark-method.md` for the full procedure and the cache/thread
sweep.

## How Settings are organised

Each category shows the recommended configuration first and folds everything else into a collapsed
**Experimental** group: the levers measured on one device, measured once, or still owed a
measurement. They ship in the release build deliberately, because testing them on hardware other
than the one test phone is what this app is for.

Descriptions in the UI say what a setting does, without measured figures or flag names, because a
number needs the device, the model and the day beside it to be worth anything. The mapping to the
CLI flags and the evidence behind each one lives in the docs, and the **metrics screen** keeps the
flag names so a reading there can be matched against a CSV column.

Two worth knowing before you turn them on:

- **"Ask the next layer what it wants"** (`--predict-prefetch`) predicts each layer's experts one
  layer early and fetches or retains what the prediction names. It is markedly more accurate than
  the previous-token guess it replaces, and a better guess still did not buy throughput: in
  thermally matched pairs the read-ahead **lost**, because the flash is already saturated. See
  `../../docs/expert-prediction.md` before drawing conclusions from a run.
- **"Decide the experts early"** (`--route-ahead`) commits each layer's routing before that layer
  runs, so the reads can never be wasted. It changes the reply, and it is refused alongside guessing
  ahead. See `../../docs/route-ahead.md`.
