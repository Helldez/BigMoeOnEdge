# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
Semantic Versioning.

## [Unreleased]

### Changed
- **Android: release APKs are signed with a stable key.** Sideload builds were debug-signed, and a
  debug key is generated per machine, so every published APK had a different signature — Android then
  refuses to update in place (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`) and forces an uninstall, which
  wipes the models in `filesDir`. Release builds now use a stable keystore (kept out of the repo via
  `keystore.properties`, gitignored), so an update installs over the previous release. The one-time
  move from the old debug-signed install to the stable key still needs a single uninstall; updates
  after that install cleanly. The distributed artifact is now `app-dev-release.apk`.

### Added
- **Android: in-app model downloads now land on O_DIRECT-capable internal storage.** The catalog and
  paste-URL downloads used the system `DownloadManager`, which can only write to the app's external
  files dir — an emulated/FUSE volume where `O_DIRECT` silently returns wrong data, so the engine
  fell back to buffered I/O for every downloaded model (measured on device: `o_direct=0`, the exact
  loss the streaming design exists to avoid). `DownloadManager` cannot target internal storage by
  design, so it is replaced by `DownloadWorker`: a foreground WorkManager `CoroutineWorker` that
  streams the gguf over HTTP straight into `filesDir/models` (real f2fs, where `O_DIRECT` works — the
  same dir the file picker imports to). It resumes an interrupted `.part` with a `Range` request
  (following Hugging Face's resolve → CDN redirect manually so the header survives) and renames on
  completion, and it needs free space equal to the model size — no temporary second copy. This is the
  mechanism Google's own AI Edge Gallery uses. Fixes #67.
- **Decode traces** (`--compute-trace PATH`, `--io-trace PATH`): returning `true` for every node from
  the eval callback forces ggml to isolate and synchronise each one, so the wall delta between
  callbacks is that node's real compute and the major-fault delta attributes a >RAM stall to the node
  that paid for it. This is what turns `compute_ms` from a residual into a measurement. `--io-trace`
  emits one row per `read_slice` (latency, aligned window, layer/expert/projection/lane). Both are
  diagnostics that perturb the run they measure, so only shares are meaningful — read them with
  `scripts/decode-analyze.py`. Done from outside llama.cpp through the public `cb_eval`, no patch.
- **Android: 500 and 1000 MiB expert-cache rungs.** Settings previously offered 0 or >= 2000, because
  the engine rejects a fixed budget under its 1500 MiB floor. That floor says a cache smaller than one
  token's routed working set can only thrash — sound, but measured on models whose cache pays for
  itself. On gpt-oss-120b at top-2 (~886 MB routed per token, an 8–13% hit from a 2000–3000 MiB
  budget covering ~5% of a 56.8 GB expert bank) the question is live, so the small rungs route through
  the floor's own escape hatch (`--force-cache`) and the help text says what they are for.

### Fixed
- **Android: a superseded session no longer starves the one replacing it.** Changing the model or
  settings started a new engine while the old process still held its model and expert cache, so the
  replacement sized its cache against a `MemAvailable` still deflated by the dying one — the app was
  triggering "two engines at once" on itself at every settings change, silently starving the very
  cache being retuned, and the combined footprint could be OOM-killed. The new session now reaps the
  old process off the main thread before probing memory. The delayed force-kill also became a
  cancellable field: a shutdown followed quickly by a new prompt could previously let a stale kill
  land on the fresh process (`exited 137`).

### Documentation
- **`docs/android-memory.md`**: what reclaims a >RAM engine's memory on a phone, which levers exist
  (almost none), and why the cache hit rate is the signal the kernel judges you by — the LRU promotes
  a page only on a *second* reference, and a cache hit is that reference. Records the watermarks, the
  vendor's swappiness-160 override, the 65536-byte `RLIMIT_MEMLOCK` ceiling that makes `mlock`
  unusable here, and the anon/file asymmetry that is the unnoticed cost of the O_DIRECT design.
- `docs/benchmarks.md` split into the Android matrix and `docs/benchmarks-gpt-oss.md`.

### Changed
- **Prompt-tail retention across prefill**: the whole prompt is one prefill ubatch, so `load_layer`
  sees every prompt token's routed experts at once, token-major. The in-batch dedup guard skipped
  the LRU promotion for an expert already staged in that batch, anchoring it at the position of the
  *first* token that used it. Promote on every touch instead, so the LRU order at the end of prefill
  reflects the *last* prompt tokens — the experts most likely to be routed again for the first
  generated tokens — keeping them resident rather than first in line for eviction. Bookkeeping only:
  reads are still scheduled once per expert, and in decode the top-k ids within a token are distinct,
  so the path is a no-op there. Byte-identity gates unaffected. **Not yet measured on device**: the
  whole first-10-token warm-up excess is ~1.0 s (Qwen) / ~0.7 s (Gemma) over steady state, so any
  gain is bounded by that and is expected to matter for short chat turns rather than long runs.

### Added
- **Per-token compute decomposition** (`majflt`, `cpu_ms`): the `compute_ms` residual silently
  absorbed page-fault stalls and scheduler idle, so a large "compute" figure could mean genuine
  matmul, a dense-weight fault storm on a >RAM model, or a throttled CPU — indistinguishable. Two
  directly-measured counters now decompose it, sampled around `llama_decode` with no submodule
  patch: `majflt` (major page faults this token — a mmap-resident dense weight re-faulted from flash
  *inside* the decode) and `cpu_ms` (CPU time summed across threads; divided by wall×threads it gives
  occupancy — near 100 % is compute-bound, well below flags a throttled or preempted core). Surfaced
  per token in `BMOE_PROGRESS`, as run averages in `BMOE_DONE`, as `majflt`/`cpu_ms` CSV columns and
  `majflt/tok`/`cpu_s/tok` summary keys, and as a `compute:` line in the one-shot summary. Both read
  `0` where the platform cannot measure them (the Windows host build). `docs/telemetry.md` also now
  documents why `stall_ms` has a structural floor above zero (the router-to-fetch dependency). See
  `docs/telemetry.md`.
- **Wall-additive Android telemetry panel**: the decode meters now show the three terms that *sum to*
  the token's wall time — `compute`, `flash wait` (the read time overlap could not hide) and `cache
  mgmt` — under a `<ms>/token → <tok/s>` headline, so the rate is read directly instead of being
  reconciled from a compute residual against a parallel, overlapped flash-I/O figure. A diagnostic
  line reports CPU occupancy, major faults/token and cache hit; raw byte throughput moves to a
  secondary line. The summary token count now reflects tokens actually generated, not the `n_predict`
  target. Backed by new `mgmt_ms`/`majflt`/`cpu_ms` fields on the session telemetry lines.
- **Direct answers on gpt-oss (`--no-think`)**: the harmony chat template always opens an
  `analysis` (chain-of-thought) channel, so `--no-think` previously had no visible effect on
  gpt-oss — the model still reasoned before answering. It now primes the `final` channel directly
  when thinking is disabled, so gpt-oss answers immediately with no analysis tokens. Keyed on
  harmony's unique `"<|start|>assistant"` generation-prompt suffix, so Qwen/Gemma `--no-think` is
  unchanged; done through the public chat-template API, no llama.cpp patch. Trade-off: forcing the
  final channel removes the model's scratch space, so reasoning-dependent answers degrade — a
  latency/throughput mode, not a free win (see the gpt-oss quality note in `docs/benchmarks.md`).
- **gpt-oss-120b on-device benchmark**: streaming a 58.46 GB MoE (128 experts, top-4) on the 11.3 GB
  OnePlus 15R — **5.2× device RAM** — at up to 7.7× a plain-mmap load (top-k 2). A top-k × lanes ×
  prefetch sweep plus an mmap baseline, with the k=4-interruption and 24-token-probe caveats and a
  quality note, in `docs/benchmarks.md`; raw data under `docs/bench-data/2026-07-14/`, drivers
  `scripts/gptoss-matrix.sh` and `scripts/gptoss-mmap.sh`.
- **Richer Android telemetry**: the live panel now also shows prefill rate (tok/s), time-to-first-token
  (model load + prompt prefill), the flash streamed this turn (MB) and the expert-cache footprint
  (resident/budget MiB), plus a live **device temperature** read from the battery sensor
  (`BatteryManager`, no permission) as a proxy for thermal headroom under a long generation. The
  engine already computed the first four; a `read_mib` field was added to the `BMOE_DONE` session
  line to carry the streamed total (see `docs/telemetry.md`). Temperature is read on the Android side
  and does not travel through the engine.
- **gpt-oss recipe** (OpenAI MoE, e.g. gpt-oss-20b/120b: 128 experts, top-4): a purely routed
  MoE registered as a single row with the standard `ffn_{gate,up,down}_exps` split suffixes.
  Unlike gemma4 it keeps no shared/dense expert resident, so the streamed fraction is as high as
  qwen3moe's. Weights ship in MXFP4; the streamer is quant-agnostic (the per-expert stride is read
  from the tensor's `nb[2]`, whatever the block layout), so the native MXFP4 layout needs no special
  handling and the existing split-layout gate already covers this streaming path. The Android
  example's active-experts (top-k) dropdown gains 3 and 2, so gpt-oss can be run below its native
  top-4 to trade quality for a smaller streamed working set.
- **Temporal prefetch** (`--prefetch K`, env `BMOE_PREFETCH`): while a token computes layer *l*,
  the experts the previous token routed at layers *l+1…l+K* are read speculatively on the idle I/O
  lanes, so a correct guess turns the next layer's read into a cache hit. Requires the LRU cache.
  The speculative path never delays real work (workers drain it only as spare capacity and yield
  to real batches; all cache-state mutation stays on the eval thread) and never changes output (a
  speculative read is the identical read a real miss would issue). Gates G5a/b/c prove
  byte-identity, including the integrate-then-hit path. A `moe-prefetch:` summary line reports the
  speculative bytes and useful-hit rate; an Android settings row exposes the depth. See
  `docs/prefetch.md`.
- **Session mode**: the engine can now load a model once and serve many prompts against it, with
  the expert LRU cache staying warm between prompts, instead of re-paying the model load and the
  cold-cache ramp on every generation. `run()` splits into a `Session` (`open` / `generate` /
  `close`); `generate()` can be called repeatedly and cancelled mid-flight via the abort callback.
  `bmoe-cli --session` drives it over a stdin/stdout JSON line protocol, and the Android example
  runs one persistent session per model (reusing the warm process across prompts, freeing it on an
  idle timeout). Independent prompts by default (KV cleared, cache warm); multi-turn chat is a
  `clear_kv=false` follow-up. Byte-identity gates S1/S2 prove a warm generate matches the cold
  one-shot reference. See `docs/session.md`.
- Cache-management time is now surfaced as its own telemetry term (`mgmt_ms` per token,
  `cache mgmt` in the `moe-stream:` summary, `mgmt_ms` CSV column, `mgmt_s/tok` in the summary
  line). It times the virtual-memory commit, eviction and LRU bookkeeping that were previously
  hidden inside the `compute_ms` residual — high on the first tokens after prefill, near zero at
  steady state. `compute_ms` is documented as a residual (`wall − io − mgmt`, or `wall − stall −
  mgmt` under overlap), not a measured matmul time. Bytes served are unchanged (gates G1–G4).
- Intra-layer I/O–compute overlap (`--overlap`): expert reads for a layer run on the I/O
  pool while the same layer's routed experts are computed, hiding flash latency behind FFN
  compute. Opt-in and byte-identical to the serial path (gates G4a/b/c). Requires one
  ~25-line per-expert readiness hook in the CPU `mul_mat_id` kernel, carried as a 1-commit
  fork branch (`bmoe/expert-ready-hook`) on `Helldez/llama.cpp` with an explicit sunset;
  the serial streaming path still builds and runs against stock upstream. See
  `docs/seam.md` § 3.
- Model-agnostic reasoning control (`--no-think`): renders the chat template with
  `enable_thinking=false`, suppressing a reasoning model's thinking channel at the source
  for Qwen3, Gemma and any template that honours the kwarg — replacing the Qwen-only
  `/no_think` prompt suffix.
- Android example: an **mmap baseline (no streaming)** settings toggle to compare against,
  plus an **I/O–compute overlap** toggle; the streaming controls disable when mmap is on.

### Removed
- Dropped the `llada-moe` recipe. LLaDA is a diffusion model, and expert streaming only pays
  off for single-token (n=1) decode; the diffusion canvas processes many tokens at once, so it
  does not benefit. It was out of scope for the mobile autoregressive target and is removed to
  keep the supported set to what the project actually optimises for. The registry can take the
  row back in one line if a validated use case appears.

### Fixed
- Storage where O_DIRECT lies is now handled: some emulated / FUSE-backed volumes (an app-private
  dir under `/storage/emulated`, where imported and downloaded models land) let the O_DIRECT open
  succeed but return garbage on read, silently corrupting expert weights into nonsense output. The
  streamer now verifies a direct read against a buffered read at init and falls back to buffered
  I/O when they disagree; real filesystems (adb-pushed models, desktop) keep O_DIRECT.
- Android: **Stop** no longer terminates the whole app — the stderr drain thread is guarded,
  so the stream close from `Process.destroy()` no longer throws on its own thread; a separate
  wakelock under-lock race on Stop is also fixed.
- Android: all-files access is requested on demand (an explicit storage rescan), not at
  startup — downloaded, imported and picked models need no permission.
- Android: the headline tok/s reports the aggregate average once generation finishes, rather
  than the last token's instantaneous rate.

## [0.1.0] - 2026-07-11

### Added
- MoE expert-selective streaming for `qwen3moe` (Qwen3-30B-A3B and siblings), `qwen2moe`,
  and `llada-moe`: stream only the routed experts per token from flash, with an optional
  LRU cache and a parallel read pool. Lossless — byte-identical to a full in-memory run,
  proven by the synthetic gates and confirmed on a real 64-expert 4 GiB model on the
  desktop host.
- Zero-fork llama.cpp integration: expert streaming is driven entirely through the public
  eval-callback and gguf accessors; `third_party/llama.cpp` is a stock upstream submodule.
- `bmoe-cli` host tool with machine telemetry (`--progress`) and a CSV sink.
- Byte-identity gates (`bmoe_moe_gates`) with a tiny synthetic model generator
  (`scripts/make-tiny-moe.py`).
- Android example app (`examples/android`): minimal chat with a live telemetry panel,
  packaged as a debug APK built and published as a CI artifact.
- Documentation: architecture, the seam, MoE streaming, adding a model, telemetry,
  benchmark method, limitations, roadmap.
