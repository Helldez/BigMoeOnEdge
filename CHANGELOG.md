# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
Semantic Versioning.

## [0.15.1] - 2026-07-23

### Added
- **A warning when cache-aware dropping meets a narrow routing.** The threshold is a fraction of the
  uniform share `1/top-k`, so `--drop-cold-experts 0.75` means "skip below 9.4% of the routing" at
  top-k 8 — where every number in 0.15.0 was measured — but "below 18.8%" at top-k 4 and "below
  37.5%" at top-k 2, where a miss discards the whole minority expert. The engine now says so once at
  load when the effective top-k is 4 or fewer, quoting the real share for the model in hand, and the
  app shows the same caveat inline under the setting. **gpt-oss is the case this exists for**: it
  routes 4 of 128, and the app default is 75%, so 0.15.0 shipped that combination with nothing
  saying it was outside the measured range.
  It warns rather than clamping: the engine cannot know whether the trade is acceptable for a given
  model, and silently adjusting a number the caller chose would be worse than a loud caveat.
- `BMOE_READY` gains `n_expert_used`, the effective routing width after any override (0 on a non-MoE
  model), so a UI can interpret the setting at all; `Session::n_expert_used()` exposes the same to
  embedders. Additive — older consumers ignore it.

### Fixed
- The 0.15.0 entry for the app default still called the quality cost unquantified, contradicting the
  GSM8K result recorded in the same section. Corrected in place.

## [0.15.0] - 2026-07-23

### Added
- **`--drop-cold-experts F` — cache-aware expert dropping.** Skips a
  routed expert when it is a cache **miss** *and* the router weighted it below `F × (1/top-k)`. An
  expert already resident costs no flash read, so it always runs however small its weight: quality
  is spent only where it buys I/O. Replayed over the committed route traces at `F = 1.0`, decode
  phase, this avoids **66% of flash reads for 9.5% of the router's weight mass**, where
  `--n-expert-used 5` avoids 23% for a comparable 10.6% — roughly 3x the reads at the same quality
  cost. (At equal *reads* instead, `--n-expert-used 3` avoids 59% but discards 37% of the mass.)
  `--drop-no-renorm` and `--drop-in-prefill` are the A/B switches. Requires the LRU cache:
  `validate()` rejects it with `--cache-mb 0`, where every expert reads as a miss and the policy
  would silently degenerate into an unconditional weight cut.
  Unlike turbo top-k the output is **not reproducible** — what gets dropped depends on what the
  cache held — so it carries no rows in the README benchmark tables, which are a deterministic
  protocol. See [docs/expert-dropping.md](docs/expert-dropping.md).
- `scripts/route-drop-replay.py`: the offline model the numbers above come from, including the
  static-`k` baseline replayed on the same rows so the two policies are comparable at equal I/O.
- Route trace gains a `dropped` column, and the metrics summary `experts_routed` /
  `experts_dropped` — the flag fixes a threshold, not a drop rate, so only these say what a run
  actually traded. New CLI summary line `moe-drop:`.
- Example app: **Speed / quality → Drop cold experts** (off / 50% / 75% / 100% of the uniform
  share), **defaulting to 75%**, disabled in mmap mode and with the cache off. 75% rather than 100%
  takes the larger part of the win for half the discarded routings — the conservative end of a
  measured range (see the GSM8K check below). The CLI keeps defaulting to off — the byte-identity
  gates need a deterministic default.
- Gates **G8a/G8a'/G8b/G8c**: a threshold below any producible weight leaves the output
  byte-identical to the undropped stream (the deferred load and the learned terminal weight node are
  transparent) and is asserted to have examined routings while dropping none; at full strength
  against a constantly-evicting cache generation still completes, so no matmul reaches a
  reserved-but-uncommitted slot; and at `--n-expert-used 1` dropping is a proven no-op, which pins
  both the top-expert guarantee and the threshold being taken against the *effective* top-k.

### Measured
- On device, in-app, Qwen3.6-35B-A3B-Q4_K_M (top-k 8 of 256), cache 3000, one variable changed:
  **2.549 tok/s off → 3.938 at `F = 0.75` (+55%) → 4.702 at `F = 1.0` (+84%)**, with flash reads
  falling 248 → 163 → 48 GiB. Per-token bootstrap intervals separate every pair except off vs 0.50,
  which overlaps — at half the uniform share the policy drops 2.7% of routings and buys nothing,
  which doubles as the negative control that the machinery is free when it does not fire.
  Data: [docs/bench-data/2026-07-22-drop-cold-experts/](docs/bench-data/2026-07-22-drop-cold-experts/findings.md).
- Run order was 1.0, off, 0.5, 0.75, so the two fastest cells are the first and the **last**: thermal
  drift would have made the last the worst. The mechanism orders by threshold even though the run
  order does not, which is what run order cannot fake.
- **The replay was conservative, not optimistic.** It is documented as an upper bound because it
  cannot model the cache changing; at `F = 0.75` it was accurate (37% predicted, 34% measured), at
  `F = 1.0` it understated (66% predicted, **81%** measured). Avoided reads free cache capacity,
  which raises the hit rate, which leaves fewer misses to drop.
- **Quality: no loss detected.** 15 GSM8K questions (verbatim from the test split) through the same
  configuration: **12/15 with dropping off, 13/15 at every threshold including 1.0**, where 28% of
  routings are discarded. Twelve of fifteen answers are identical across all four cells; the
  variation sits on two questions and flips in both directions rather than worsening with the
  threshold, and reply length is flat. 13 against 12 is not an improvement — one question is 6.7
  points here, and 15 questions cannot exclude a regression under ~13 points. It rules out a
  collapse, not a subtle cost. Harness, per-question replies and grading rule:
  [docs/bench-data/2026-07-22-drop-quality/](docs/bench-data/2026-07-22-drop-quality/findings.md).

- **A warning when dropping meets a narrow routing.** The threshold is a fraction of the uniform
  share `1/top-k`, so `0.75` means "below 9.4% of the routing" at top-k 8 but "below 37.5%" at
  top-k 2 — a regime nothing here has measured. The engine now says so once at load when the
  effective top-k is 4 or fewer, and the app shows the same caveat inline under the setting. It
  warns rather than clamping: the engine cannot know whether the trade is acceptable for a given
  model, and silently adjusting a number the caller chose would be worse than a loud caveat.
  gpt-oss is the case to watch — it routes 4 of 128.
- `BMOE_READY` gains `n_expert_used`, the effective routing width after any override (0 on a
  non-MoE model), so a UI can interpret the setting at all. Additive; older consumers ignore it.
  `Session::n_expert_used()` exposes the same value to embedders.

### Changed
- With the policy armed, `load_layer()` moves from the topk node to the terminal node of the layer's
  weight chain — the decision needs the final router weights. Which node that is depends on the
  model's gating, so the hook **learns** it from the graph rather than carrying an architecture
  table; until it is known a layer loads at its topk node undropped, exactly as before. No behaviour
  changes when `--drop-cold-experts` is off.
- README no longer calls turbo top-k "the one lossy knob" — it is now the *measured* one.
- Docs that assumed a deterministic engine are scoped: `prefetch.md` ("cannot change output" holds
  only with the lossy knobs off — under dropping, a correct guess un-drops an expert),
  `moe-streaming.md`, `architecture.md`, `limitations.md` (new entry for non-reproducibility) and
  `runtime.h`'s contract.
- `cache_hit_pct`, `token_demand_MiB` and `layer_demand_MiB` shift meaning under dropping — a
  dropped routing is a miss that is never looked up, so the hit rate rises without the cache serving
  more, and the demand figures measure what was *staged* rather than routed. Documented in
  `telemetry.md`, `pressure.md` (which tells you to size the cache first, dropping off) and
  `metrics.h`; `benchmark-method.md` gains the axis plus a warning that its reverse-the-run-order
  check cannot distinguish a moved drop rate from a contaminated cell.
- `scripts/route-analyze.py` reports when a trace was recorded with dropping on, so its
  working-set figures are not misread as flash traffic.
- README: the two quality-trading knobs (turbo top-k and cache-aware dropping) are now one
  Features bullet and one section, **Trading quality for speed**, instead of two long bullets and
  a top-k-only section — same numbers and caveats, said once.

### Fixed
- `validate()` now rejects a NaN `--drop-cold-experts` threshold instead of accepting it and
  silently arming nothing: the range check is written as a negated inclusive range, since NaN
  compares false against every bound and slipped past both the `[0, 1]` check and the
  cache-required check. Found by a release audit; covered in `tests/config_test.cpp`.

## [0.14.0] - 2026-07-21

### Added
- **`--dense-weights ahwb` — the dense weights in memory Android is not allowed to reclaim, measured
  at +17.9% decode.** The buffer comes from a locked `AHardwareBuffer` BLOB (a dma-buf, pinned for
  its lifetime because a device may DMA from it) instead of the heap; everything else is `anon`'s
  path unchanged, so the A/B between them moves exactly one variable. Exposed as
  `pio::pinned_alloc` and as a **Dense weights → Pinned** setting in the example app.
  Android-only: on any other platform the mode refuses to start rather than silently falling back,
  which would let a comparison become a mode against itself.
- `dense_resident_frac` works under the new mode (mincore does report on a dma-buf mapping), where
  it doubles as the falsification test — and it reports exactly **1.000, minimum included**, in
  every pinned run. Reclaim-exemption is now measured, not inferred.

### Measured
- In-app, Qwen3.6-35B-A3B-Q4_K_M, k=8, cache 3000, 1354-token generation, same session and binary:
  **2.588 → 3.053 tok/s (+17.9%), bootstrap intervals disjoint**. Data in
  [docs/bench-data/2026-07-21-pinned-dense-ab/](docs/bench-data/2026-07-21-pinned-dense-ab/findings.md).
- **The mechanism is not the predicted one.** Major faults are *equal* between the modes (265 vs
  257): `anon` already keeps the dense weights off the flash. What it does not prevent is the kernel
  taking ~15% of them into **zram**, where a later touch costs a minor fault plus a decompression —
  a cost that shows up in no I/O counter and no fault counter, and therefore lands in `compute_ms`.
  The whole delta appears there (298 → 241 ms) while `io_ms`, `stall_ms` and cache hit rate are
  unchanged to within 1%, and swap falls 562 → 294 MiB. **`anon` protects from flash; `ahwb` also
  protects from zram.**
- The feared trade did not occur: the expert cache is untouched (hit rate identical to the decimal),
  because the dense set (~1.6 GiB) is small next to a 3000 MiB cache budget.

### Notes
- **Default stays `anon`.** In the decisive pair `ahwb` ran first and an order effect cannot be
  excluded — the reversed pair is owed — and this is one device, one model, one config.
- Short turns cannot see this: three 67–74 token pairs were all inconclusive (per-token CV 33–71%,
  every interval overlapping). Reclaim is a standing condition that accumulates, so the effect is
  only resolvable over a conversation-length generation.
- A cross-day comparison of the same pair read +63.6% and is **not** usable: `anon` alone moved
  +38.8% between the two days. It is committed so the correction is checkable.
- Transferable: `compute_ms` is a residual that has been absorbing zram decompression all along, so
  earlier "compute-bound" conclusions deserve re-examination.

## [0.13.5] - 2026-07-21

Diagnostics only — no engine, CLI or app behaviour changes, so the Android version is unchanged.

### Added
- **`bmoe-membench`**: measures read bandwidth of the allocations the dense weights can live in,
  comparing an anonymous mapping against a locked `AHardwareBuffer` BLOB. It exists to gate an idea
  rather than to tune one: dma-buf pages are the only allocation an unprivileged Android app can
  make that reclaim cannot touch, but gralloc decides per allocation whether a buffer is
  CPU-cacheable, and an uncached mapping would read at or below the flash bandwidth it is meant to
  save. `--probe-max` reports the largest usable buffer; `--repeat` interleaves the comparison.
- **Measured: reclaim-exempt memory is full-speed, and capped at 2047 MiB.** A locked BLOB reads
  within 0.5% of anonymous memory on both CPU clusters and at 4 threads, and the `CPU_READ_OFTEN`
  hint makes no difference. Allocation reaches the 4 GiB format cap, but `AHardwareBuffer_lock`
  fails with `EINVAL` at exactly 2^31 bytes, so larger working sets must be split across buffers.
  The bandwidth gate passes; **whether pinning the dense weights helps remains unmeasured**, and
  reclaim-exempt memory does not create memory. Data in
  [docs/bench-data/2026-07-21-pinned-memory/](docs/bench-data/2026-07-21-pinned-memory/findings.md);
  the lever table in `docs/android-memory.md` now carries the dma-buf row it was missing.

### Fixed
- `bmoe-membench --probe-max` initially probed allocation only and reported ~2× the usable size,
  because the lock that turns a BLOB into a CPU pointer fails long before allocation does. It now
  locks every candidate. Same class of defect as 0.13.3's: a diagnostic returning a confident
  wrong number rather than an error.

## [0.13.4] - 2026-07-20

Diagnostics and a recorded negative result — no engine, CLI or app behaviour changes.

### Added
- **`bmoe-iobench --scatter N`**: split every logical read into N preads at independent offsets,
  so two layouts can be compared at equal traffic volume. It showed sub-MiB scattered reads DO
  lose bandwidth at low lane counts (refining 0.13.3's "flat above 256 KiB", which held only at
  saturating lanes).
- **Recorded negative result: the contiguous per-expert sidecar** (one read per routed expert
  instead of one per projection). Implemented, byte-identity-proven, and measured across three
  interleaved on-device A/Bs: **+16%** serial with the cache off, **−20%** on LFM2.5 and
  **−23%** on Qwen3-30B k=8 in the shipping overlap+cache configurations — the overlap kernel
  consumes projection-major, and whole-entry reads triple the latency to an expert's first
  slice. Closed unmerged (PR #90, tag `expert-sidecar-refuted`); data and analysis in
  [docs/bench-data/2026-07-20-sidecar/](docs/bench-data/2026-07-20-sidecar/findings.md).

## [0.13.3] - 2026-07-20

Diagnostics only — no engine, CLI or app behaviour changes, so the Android version is unchanged.
Both tools ship measurements that are quoted as evidence in `docs/roadmap.md`, and both had a way
to produce a confident wrong number rather than an error.

### Fixed
- **`route-replay.py` no longer reports a fabricated ~100 % hit rate when the trace preamble is
  incomplete.** `cost()` defaulted a layer with no recorded `expert_bytes` to **zero bytes**: such a
  layer was admitted for free, never counted against the budget and was never evicted. A trace
  missing its per-layer preamble therefore did not fail — it printed a full, plausible table in
  which *every* policy scored the same near-perfect number. Measured on a gate-model trace with the
  preamble stripped: the old code prints `96.9 %` across all six policies; it now exits with the
  reason and names the layer. The recorded traces behind the published curves all carry complete
  preambles, so no result in `docs/` changes.
- **An unknown `--policies` name is rejected instead of silently running LRU.** A typo (`lur`) fell
  through every branch of `victim()` to the LRU default and was tabulated under its own column
  header, so the output claimed to compare a policy that never ran.
- **`bmoe-iobench` asks the OS for its alignment instead of assuming 4096.** Page size is the very
  variable this tool exists to characterise; on a device with a 16 KiB page the sweep was measuring
  the wrong alignment. It now uses `pio::vm_page()`, the same source the engine uses.
- **`bmoe-iobench --slice-kb` is documented in the unit it actually takes.** The usage text said
  "bytes per read, default 4096" for a value multiplied by 1024 — every figure the tool printed was
  open to being read off by a factor of 1024. It is KiB, and the default is 4 MiB.

## [0.13.2] - 2026-07-20

### Fixed
- **An explicitly passed flag now really does beat the environment variable.** `bmoe-cli` documents
  that a flag always wins over the matching `BMOE_*` override, but it decided "was this flag passed?"
  by asking whether the field still held its default. So `--cache-mb 0` (cache deliberately off),
  `--io-threads 4`, `--prefetch 0` and `--n-expert-used 0` were indistinguishable from an untouched
  config and got overridden anyway — the app passes two of those explicitly. The CLI now records
  which flags were typed and consults that, so passing a flag its default value is still a choice
  the engine honours. Values arriving from the environment are validated exactly as before.

### Removed
- **The app's unused gguf architecture probe.** `GgufHeader.arch()` was written "to pick the right
  chat turn format" and never called: `--chatml` already renders the model's *own* template, so the
  format is chosen by the gguf, not by a name the app reads. Wiring it up would have reintroduced
  the model-name list the engine's own design note rules out; it and its two private helpers are
  gone. MoE detection (`isMoe`), the one entry point in use, is untouched.

## [0.13.1] - 2026-07-19

### Fixed
- **A non-reasoning model is no longer told it "always reasons."** `think_ctl` reported `none` for
  any model whose template ignores `enable_thinking` while its handler declares reasoning tags — but
  handlers publish those tags for a whole *family*, so the non-reasoning members (LFM2-8B-A1B,
  LFM2.5-Instruct) advertise a `<think>` they never emit. The app disabled their Thinking switch and
  explained it with a sentence that was simply false. `none` now requires positive evidence that the
  model reasons: the tag is declared **and** the template actually uses it — the same test llama.cpp
  applies before wiring up reasoning extraction. Models with nothing to suppress report `template`,
  which is what they did before any of this existed. Same for a template with no reasoning at all,
  which used to fall into `none` through a second path.

## [0.13.0] - 2026-07-19

### Fixed
- **Thinking off no longer silently does nothing** (#82). Turning Thinking off set the template
  variable `enable_thinking` and stopped there — but that variable is only a *request* to the
  model's chat template, and many templates never read it. LFM2.5's is one: the rendered prompt came
  out byte-identical either way, the model reasoned on, and nothing reported that the setting had
  been dropped. The engine now renders the template at load and reports what it found as `think_ctl`
  on `BMOE_READY` (see `docs/telemetry.md`). Where the flag is inert but reasoning is a *structural*
  section of the format, the turn now starts past that section, built by llama.cpp's own
  continuation hook so every family's markers come from upstream rather than from this engine.
  Where the model owns its reasoning span and simply cannot be asked to skip it — LFM2.5 — that is
  reported instead of papered over, and the app shows the Thinking switch disabled with the reason.
  Measured, not assumed: handing LFM2.5 a pre-closed empty reasoning span makes it reason *untagged
  into the answer*, worse than leaving the setting alone, so the engine does not do it.

### Changed
- **The harmony/gpt-oss marker strings are gone from the decode path.** Priming gpt-oss to answer
  without reasoning used to be a literal `<|start|>assistant` suffix test and a literal
  `<|channel|>final<|message|>` appended in `session.cpp`. It is now the same generic mechanism as
  every other family, so the engine names no model's markers and a submodule bump that changes them
  needs no engine change.
- **README front page reworked around the result.** A copyable result banner under the title, an
  autoplay hero GIF of gpt-oss-120b generating on-device (real time, cut from the existing demo
  footage), and a "Try it on your phone" quickstart that starts from the release APK and the
  in-app catalog instead of a source build. No numbers changed.

## [0.12.0] - 2026-07-19

### Added
- **Liquid AI LFM2 / LFM2.5 MoE support** (arch `lfm2moe`, e.g. LFM2.5-8B-A1B, LFM2-24B-A2B). A
  hybrid short-convolution/attention stack whose routed experts use the standard split expert
  layout, so it streams through the existing path unchanged — one registry row, no engine change.
  Two structural notes recorded in `docs/limitations.md`: the leading dense blocks name no expert
  tensors and stay resident, and the router's per-expert bias (`ffn_exp_probs_b`) applies before the
  top-k, so the node the engine reads is unaffected. Not added to the in-app download catalog: at
  ~8B total the model fits in phone RAM, which is not the case the engine exists for. Validated
  on-device against LFM2.5-8B-A1B (Q5_K_M): experts stream with O_DIRECT over the discovered bank of
  32, no engine change needed. Not a benchmarked configuration — the published tables are unchanged.

## [0.11.1] - 2026-07-19

### Changed
- **Internal cleanup, no behaviour change.** The per-token metrics block moved out of the middle of
  `Session::generate()` into a `GenTally` that owns the streaming cursors and run totals (the
  byte-identity gates pass unchanged). In the Android example, download tracking moved out of the
  composable: `ModelDownloader.events()` streams progress and outcomes over WorkManager's own flow —
  finalizing a landed `.part` before reporting it — instead of the UI polling on a timer and
  re-seeding by hand, and the telemetry panel's compute/flash-wait/CPU-occupancy arithmetic is now a
  pure `breakdown()` next to the contract it implements.

### Documentation
- Benchmark tables standardized across the README and `docs/benchmarks.md` (expert count in-table,
  `k` = `n_expert_used` defined in the key and linked to the Turbo top-k section).
- `docs/adaptive-cache.md` renamed to `docs/cache-sizing.md`, and the stale "tracks free memory"
  description of `--cache-mb auto` corrected: it is a one-shot sizing at load, not a running
  governor. The dense anon set is stated to sit outside the cache budget.
- This changelog is versioned again: every release from 0.1.1 to 0.11.0 now has its own dated
  section instead of accumulating under `[Unreleased]`.
- Stale references corrected in the Android example: the catalog's `DOWNLOADING` status and the
  add-model section still named the system DownloadManager, retired when in-app downloads moved to
  WorkManager, and `MANUAL_ONLY` pointed at an `Entry.notes` field that is named `install`.
- `RunInfo::dense_weights` no longer advertises a `"warm"` default the engine stopped defaulting to;
  it now reads `"anon"`, matching `RunConfig`. The member is always overwritten in `Session::open()`,
  so no emitted telemetry changes.

## [0.11.0] - 2026-07-19

### Added
- **Qwen3.6-35B-A3B support** (arch `qwen35moe`). A hybrid attention/SSM MoE (256 experts, top-8,
  41 blocks) whose routed experts stream through the existing `qwen3moe` path unchanged — no engine
  change, one registry row. Added to the in-app one-tap download catalog. At ~2× device RAM it needs
  streaming: mmap baseline 0.1 tok/s (fault storm) vs **5.0 tok/s** streamed at the model's own
  width (cache 3000 MiB, byte-identical output), or **5.8 tok/s** with turbo top-k (`k=6`, lossy).
  Measured on the OnePlus 15R (indicative 96-token run, not yet the full 256-token protocol); see
  the README benchmarks section.

## [0.10.0] - 2026-07-19

### Added
- **Layer-granularity compute trace** (`--compute-trace-layers PATH`). The per-node trace
  (`--compute-trace`) pays ~3000 barriers per token, which serializes the graph against the expert
  stream — on a model that streams heavily it mostly measures its own serialization (Qwen3-30B:
  9.4 s/token traced vs 0.39 untraced), so its absolutes cannot be compared across models. Layer
  granularity isolates only the first node of each layer (~`n_layer` barriers per token), so
  operator coalescing and the async expert prefetch survive and the traced numbers stay close to
  an untraced run — cheap enough to compare models head-to-head, per layer, with major faults
  attributed per segment. `scripts/decode-analyze.py compute` detects the granularity and prints
  the per-segment table; see docs/telemetry.md.

### Changed
- **Default generation and cache parameters reviewed** (#71). `n_predict` now defaults to **128**
  on every surface (core `RunConfig`, the CLI session fallback, and the Android app — previously
  32 / 32 / 48): the old budgets truncated most answers mid-sentence, which reads as broken rather
  than slow. The app's expert cache defaults to a **fixed 2000 MiB** instead of Auto (ceil 3000):
  a fixed budget is reproducible across runs, while Auto sizes to whatever RAM happens to be free
  at load. Auto stays selectable in Settings; existing installs keep their saved preferences.

## [0.9.2] - 2026-07-19

### Fixed
- **A reasoning model's thinking is shown instead of hidden.** Wiring the chat reasoning parser
  correctly (the prior fix) started stripping the reasoning span from the answer *unconditionally* —
  even with thinking enabled — so a Thinking-on run sat on a blank answer while the model reasoned and
  only the final answer ever appeared, reading as a hang on a slow streamed decode. The reasoning was
  parsed and then discarded. It is now surfaced alongside the answer, kept apart from it end to end:
  `TokenMetrics`/`RunResult` carry a `reasoning` field, the line protocol adds `reasoning` to
  `BMOE_PROGRESS`/`BMOE_DONE`, and the Android app renders it as a dimmed, collapsible "Thinking"
  block above the reply — open while it streams, collapsed once the turn is committed. The answer
  itself is unchanged (reasoning still stripped from it), so the byte-identity gates are untouched.
  The Thinking setting description is now model-agnostic. Fixes #70.

## [0.9.1] - 2026-07-18

### Changed
- **Android: release APKs are signed with a stable key.** Sideload builds were debug-signed, and a
  debug key is generated per machine, so every published APK had a different signature — Android then
  refuses to update in place (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`) and forces an uninstall, which
  wipes the models in `filesDir`. Release builds now use a stable keystore (kept out of the repo via
  `keystore.properties`, gitignored), so an update installs over the previous release. The one-time
  move from the old debug-signed install to the stable key still needs a single uninstall; updates
  after that install cleanly. The distributed artifact is now `app-dev-release.apk`.

## [0.9.0] - 2026-07-18

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

## [0.8.3] - 2026-07-18

### Fixed
- Android: the soft keyboard is dismissed on send, and the answer is kept clear of the IME instead of
  being hidden behind it.

## [0.8.2] - 2026-07-18

### Added
- **Opt-in sampling** (`--temp`, `--top-p`, `--top-k`, `--seed`) (#51). Decoding stayed greedy
  (argmax) by default so the byte-identity gates keep a deterministic reference; sampling engages
  only when a temperature above zero is asked for.
- Android: on-device models can be deleted from the catalog, so a multi-GB file no longer needs a
  file manager to remove (#52).

### Changed
- The CLI defaults `--dense-weights` to `anon`, matching the Android app (#55).

### Fixed
- Reasoning spans are stripped from chat answers, so a thinking model's scratch text no longer leaks
  into the reply (#49). (Superseded in 0.9.2, which surfaces the span instead of discarding it.)
- Android: the metrics glossary was corrected along with three telemetry miscounts (#50).
- Android: every metric row stays reachable in the CSV view (#53).

### Documentation
- `docs/telemetry.md` notes that the `compute_ms` clamp breaks the wall-additive identity (#47).

## [0.8.1] - 2026-07-17

### Changed
- Android: dense (non-expert) weights default to the `anon` policy — read once through O_DIRECT into
  anonymous memory rather than left to the page cache, which is what survives a >RAM fault storm.

## [0.8.0] - 2026-07-17

### Added
- **Android: a built-in model catalog with one-tap downloads.** Getting a multi-GB gguf onto the
  phone previously meant adb or a file manager; the app now lists the supported models with their
  sizes and downloads a chosen one directly, no broad storage permission required.
- **`--dense-weights` — a dense (non-expert) weight residency policy.** The streamer only ever
  governed routed experts; the dense remainder was left to mmap and the page cache, which is exactly
  what collapses past RAM. The flag makes that residency an explicit choice (`mmap`, `warm`, `anon`).
- **On-device memory telemetry and pressure sensing**: available-memory, resident-set and dense
  residency signals, so a >RAM run can be told apart from a throttled or reclaimed one.

### Changed
- **The adaptive cache governor was retired.** Runtime cache-budget adaptation under memory pressure
  (added in 0.3.0) was removed in favour of a fixed LRU budget (`--cache-mb N`) plus a one-shot
  `--cache-mb auto` sizing at load: the governor's feedback loop fought the kernel's own reclaim
  instead of complementing it, and it cost more modularity than it bought. `docs/pressure.md` keeps
  the measurements as a history note.
- The dev shared model directory was renamed `shardllm` → `bmoe`.
- `FileReader` and `DenseWeights` were split into their own modules, giving each consumer its own
  O_DIRECT handle instead of sharing one.

### Fixed
- Android: `INTERNET` is declared, without which no download ever ran.
- Android: when a model exists in two places, the O_DIRECT-capable copy is preferred.
- Android: a refused download is reported in the row that caused it, in GB; catalog rows no longer
  wrap, and the card can be closed.

## [0.7.0] - 2026-07-15

### Added
- **Decode traces** (`--compute-trace PATH`, `--io-trace PATH`): returning `true` for every node from
  the eval callback forces ggml to isolate and synchronise each one, so the wall delta between
  callbacks is that node's real compute and the major-fault delta attributes a >RAM stall to the node
  that paid for it. This is what turns `compute_ms` from a residual into a measurement. `--io-trace`
  emits one row per `read_slice` (latency, aligned window, layer/expert/projection/lane). Both are
  diagnostics that perturb the run they measure, so only shares are meaningful — read them with
  `scripts/decode-analyze.py`. Done from outside llama.cpp through the public `cb_eval`, no patch.
- **Per-step per-layer MoE route trace** (`--route-trace PATH`), with `scripts/route-analyze.py` and
  `scripts/route-viewer.py` to read a capture without a spreadsheet.
- **Android: 500 and 1000 MiB expert-cache rungs.** Settings previously offered 0 or >= 2000, because
  the engine rejects a fixed budget under its 1500 MiB floor. That floor says a cache smaller than one
  token's routed working set can only thrash — sound, but measured on models whose cache pays for
  itself. On gpt-oss-120b at top-2 (~886 MB routed per token, an 8–13% hit from a 2000–3000 MiB
  budget covering ~5% of a 56.8 GB expert bank) the question is live, so the small rungs route through
  the floor's own escape hatch (`--force-cache`) and the help text says what they are for.
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
- Android: answers render as Markdown, and the scroll no longer fights the user (#24).

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

### Fixed
- **Android: a superseded session no longer starves the one replacing it.** Changing the model or
  settings started a new engine while the old process still held its model and expert cache, so the
  replacement sized its cache against a `MemAvailable` still deflated by the dying one — the app was
  triggering "two engines at once" on itself at every settings change, silently starving the very
  cache being retuned, and the combined footprint could be OOM-killed. The new session now reaps the
  old process off the main thread before probing memory. The delayed force-kill also became a
  cancellable field: a shutdown followed quickly by a new prompt could previously let a stale kill
  land on the fresh process (`exited 137`).
- Android: the I/O mode is cleared when the session is replaced, instead of reporting the previous
  session's mode.

### Documentation
- **`docs/android-memory.md`**: what reclaims a >RAM engine's memory on a phone, which levers exist
  (almost none), and why the cache hit rate is the signal the kernel judges you by — the LRU promotes
  a page only on a *second* reference, and a cache hit is that reference. Records the watermarks, the
  vendor's swappiness-160 override, the 65536-byte `RLIMIT_MEMLOCK` ceiling that makes `mlock`
  unusable here, and the anon/file asymmetry that is the unnoticed cost of the O_DIRECT design.
- `docs/benchmarks.md` split into the Android matrix and `docs/benchmarks-gpt-oss.md`.
- An index for `docs/`, linked from the README.

## [0.6.0] - 2026-07-14

### Added
- **Dense weights are warmed into the page cache at load**, which removes the >RAM fault storm that
  otherwise pays for every dense weight again inside each decode. Exposed as an Android settings
  toggle.

### Documentation
- Per-token warm-up analysis for Qwen, Gemma and gpt-oss: where the first tokens' excess time goes,
  and what a warm-up fix can and cannot claim (`docs/warmup-analysis.md`).

## [0.5.0] - 2026-07-14

### Added
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
  (resident/budget MiB), plus a live **device temperature** as a proxy for thermal headroom under a
  long generation. The engine already computed the first four; a `read_mib` field was added to the
  `BMOE_DONE` session line to carry the streamed total (see `docs/telemetry.md`). Temperature is read
  on the Android side and does not travel through the engine.
- **gpt-oss recipe** (OpenAI MoE, e.g. gpt-oss-20b/120b: 128 experts, top-4): a purely routed
  MoE registered as a single row with the standard `ffn_{gate,up,down}_exps` split suffixes.
  Unlike gemma4 it keeps no shared/dense expert resident, so the streamed fraction is as high as
  qwen3moe's. Weights ship in MXFP4; the streamer is quant-agnostic (the per-expert stride is read
  from the tensor's `nb[2]`, whatever the block layout), so the native MXFP4 layout needs no special
  handling and the existing split-layout gate already covers this streaming path. The Android
  example's active-experts (top-k) dropdown gains 3 and 2, so gpt-oss can be run below its native
  top-4 to trade quality for a smaller streamed working set.

### Changed
- Android: the temperature reading moved from the battery sensor to a CPU thermal zone, which tracks
  the sustained load a long generation actually creates.

### Removed
- **Speculative gating was removed** to restore the modular seam. Predicting the next layer's experts
  from its router (added in 0.3.0) required reaching further into the graph than the public
  eval-callback comfortably allows; the temporal prefetch keeps the useful part of the idea without
  that cost.

## [0.4.0] - 2026-07-13

### Added
- **A recipe for a hybrid attention/SSM MoE family** (arch `qwen35moe`), registered as one registry
  row — the routed experts stream through the existing `qwen3moe` path unchanged.

### Changed
- Android: the expert cache defaults to Auto with a 3000 MiB ceiling, a 2000 MiB ceiling option is
  added, and the load-all debug toggle is dropped.

### Removed
- Dropped the `llada-moe` recipe. LLaDA is a diffusion model, and expert streaming only pays
  off for single-token (n=1) decode; the diffusion canvas processes many tokens at once, so it
  does not benefit. It was out of scope for the mobile autoregressive target and is removed to
  keep the supported set to what the project actually optimises for. The registry can take the
  row back in one line if a validated use case appears.

## [0.3.0] - 2026-07-13

### Added
- **Session mode**: the engine can now load a model once and serve many prompts against it, with
  the expert LRU cache staying warm between prompts, instead of re-paying the model load and the
  cold-cache ramp on every generation. `run()` splits into a `Session` (`open` / `generate` /
  `close`); `generate()` can be called repeatedly and cancelled mid-flight via the abort callback.
  `bmoe-cli --session` drives it over a stdin/stdout JSON line protocol, and the Android example
  runs one persistent session per model (reusing the warm process across prompts, freeing it on an
  idle timeout). Independent prompts by default (KV cleared, cache warm); multi-turn chat is a
  `clear_kv=false` follow-up. Byte-identity gates S1/S2 prove a warm generate matches the cold
  one-shot reference. See `docs/session.md`.
- **Temporal prefetch** (`--prefetch K`, env `BMOE_PREFETCH`): while a token computes layer *l*,
  the experts the previous token routed at layers *l+1…l+K* are read speculatively on the idle I/O
  lanes, so a correct guess turns the next layer's read into a cache hit. Requires the LRU cache.
  The speculative path never delays real work (workers drain it only as spare capacity and yield
  to real batches; all cache-state mutation stays on the eval thread) and never changes output (a
  speculative read is the identical read a real miss would issue). Gates G5a/b/c prove
  byte-identity, including the integrate-then-hit path. A `moe-prefetch:` summary line reports the
  speculative bytes and useful-hit rate; an Android settings row exposes the depth. See
  `docs/prefetch.md`.
- **An auto cache budget** (`--cache-mb auto`), sized once at load from an available-memory probe,
  with `--cache-ceil-mb` as an upper bound and an Android Auto cache-size choice.
- **`--n-expert-used`** to override the active experts per token (turbo top-k), trading quality for
  a smaller streamed working set.
- **Multi-turn chat with KV prefix reuse**, plus mode-aware Android telemetry and top-k /
  cache-ceiling settings rows.
- **Speculative gating**: predict the next layer's experts from its router, with off-thread
  prediction, NEON dot kernels, cold inserts at the LRU tail, and an auto-off when router recall
  stays low. (Removed again in 0.5.0.)
- Cache-management time is now surfaced as its own telemetry term (`mgmt_ms` per token,
  `cache mgmt` in the `moe-stream:` summary, `mgmt_ms` CSV column, `mgmt_s/tok` in the summary
  line). It times the virtual-memory commit, eviction and LRU bookkeeping that were previously
  hidden inside the `compute_ms` residual — high on the first tokens after prefill, near zero at
  steady state. `compute_ms` is documented as a residual (`wall − io − mgmt`, or `wall − stall −
  mgmt` under overlap), not a measured matmul time. Bytes served are unchanged (gates G1–G4).

### Fixed
- Android: a session-reload race, plus device-agnostic defaults and telemetry.
- Android: `i8mm` dropped from the APK CPU baseline so the CLI runs on pre-armv8.6 SoCs.

## [0.1.1] - 2026-07-12

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
- The model family's own chat template is applied, not just Qwen ChatML.
- Android: models are imported into internal storage so O_DIRECT stays fast, and
  `/data/local/tmp` is also scanned for adb-pushed models.
- The run summary reports prefill, model-load and TTFT.

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
</content>
