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

## Flavors

Two build flavors differ only in how a model reaches the device:

- **dev** — sideloaded (this is what CI attaches to releases). Keeps all-files access, so it
  can also read a model adb-pushed to shared storage. Application id `…​.example.dev`.
- **play** — Play-Store-compliant. No broad storage permission: models come only through the
  in-app downloader or the file picker. `./gradlew assemblePlayDebug`.

## Getting a model onto the device

Any of these — the picker lists every MoE `.gguf` it finds (dense models are filtered out by
a gguf-header check):

1. **In-app URL download** (both flavors). Tap **Add → Download** and paste a direct gguf URL
   (e.g. a Hugging Face `…/resolve/main/model.gguf` link). It downloads in the background to
   the app's files dir — no permission needed — and appears in the picker when done.
2. **In-app file picker** (both flavors). Tap **Add → Pick file** and choose a `.gguf` already
   on the device; it is imported into the app's files dir.
3. **adb push** (dev flavor only — needs all-files access, which the dev build requests):

   ```bash
   adb push Qwen3-30B-A3B-Q4_K_M.gguf /sdcard/Download/
   # or the app's own dir, or /data/local/tmp/shardllm for a model too big to duplicate
   ```

## Expected numbers

On a phone with UFS 4.x storage and ~12 GB RAM, streaming Qwen3-30B-A3B-Q4_K_M with the
expert cache at 4000 MiB, 4 I/O lanes and 4 compute threads, decode settles around
**0.55–0.6 s/token (~1.8 tok/s)** — a model ~1.7× the device RAM, lossless. See
`../../docs/benchmark-method.md` for the full procedure and the cache/thread sweep.
