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

   Published sideload builds are release-signed with a stable key instead, so an update installs
   over the previous one rather than being refused. That needs a `keystore.properties` next to
   `app/` (gitignored — it points at the keystore and holds its passwords); without it the release
   build falls back to debug signing.

## Flavors

Two build flavors differ only in how a model reaches the device:

- **dev** — sideloaded (this is what CI attaches to releases). Keeps all-files access, so it
  can also read a model adb-pushed to shared storage. Application id `…​.example.dev`.
- **play** — Play-Store-compliant. No broad storage permission: models come only through the
  in-app downloader or the file picker. `./gradlew assemblePlayDebug`.

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

### gpt-oss-120b

Listed in the catalog but not downloadable in-app: Hugging Face ships the Q4_K_M quant as two
shards (the 50 GB per-file limit), and expert streaming reads tensors by byte offset from a
single file. Merge the shards on a PC, then transfer the result:

```bash
llama-gguf-split --merge gpt-oss-120b-Q4_K_M-00001-of-00002.gguf gpt-oss-120b-Q4_K_M.gguf
adb push gpt-oss-120b-Q4_K_M.gguf /data/local/tmp/bmoe/   # or import it with the file picker
```

## Expected numbers

On a phone with UFS 4.x storage and ~12 GB RAM, streaming Qwen3-30B-A3B-Q4_K_M with the
expert cache at 4000 MiB, 4 I/O lanes and 4 compute threads, decode settles around
**0.55–0.6 s/token (~1.8 tok/s)** — a model ~1.7× the device RAM, lossless. That 4000 MiB is a
sweep point from the benchmark protocol, not the app default: the app ships a fixed 2000 MiB
expert cache. See `../../docs/benchmark-method.md` for the full procedure and the cache/thread
sweep.

The Streaming section also exposes the **predictive prefetch** (experimental, off by default):
the engine predicts each layer's experts one layer early and reads ahead / retains what the
prediction names (`--predict-prefetch`, with "predicted misses to read ahead" mapping to
`--predict-spec-max`; 0 = retention only). It needs the cache on and replaces the temporal
prefetch — the two are mutually exclusive. Its throughput is **not yet proven** on device; see
`../../docs/expert-prediction.md` for the measured state before drawing conclusions from a run.
