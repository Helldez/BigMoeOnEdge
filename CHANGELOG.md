# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
Semantic Versioning.

## [Unreleased]

### Added
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
