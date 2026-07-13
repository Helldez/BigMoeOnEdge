# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
Semantic Versioning.

## [Unreleased]

### Added
- **Speculative gating** (`--spec-gate`, env `BMOE_SPEC_GATE`): predict the next MoE layer's
  experts by running its (tiny, mmap-resident) router on the current layer's hidden state — the
  residual stream changes slowly between layers — and prefetch the top-k. Sharper than temporal
  prefetch and composes with it. Recipe-driven per architecture (qwen3moe/qwen2moe post-norm
  router; gemma4's `rms_norm·1/√n_embd·scale` transform on attn_out, with interleaved dense layers
  handled); `n_expert_used` and the rms epsilon come from gguf metadata; a quantized router
  disables it. Predictions only feed the byte-safe prefetch queue, so a mispredict wastes a read
  but never changes output — gates G6a/b/c prove byte-identity. A `moe-spec-gate:` line reports
  router prediction recall; an Android settings toggle exposes it. See `docs/spec-gating.md`.
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
