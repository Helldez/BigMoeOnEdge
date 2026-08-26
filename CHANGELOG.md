# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
Semantic Versioning.

## [0.22.0] - 2026-08-26

### Added
- **Qwen3.8-Flash-Next (`qwen4exp`) recipe.** The Qwen4 architecture preview (125B total, ~6B
  active) routes over 512 experts at top-10 plus one always-on shared expert that stays resident;
  the experts name the standard split suffixes, so streaming is one registry row. The hybrid
  gated-delta SSM / sparse attention stack, its indexer and the per-block hyper-connection
  tensors are dense-side llama.cpp code, invisible to the streaming seam. What is new is one
  resident tensor: `per_layer_token_embd`, the n-gram embedding table, a single 2-D tensor of
  320,001,536 rows carrying 51.2 B parameters (~28.8 GB at IQ4_NL, about 43 % of a released
  file), read sixteen 160-wide rows per token. Not indexed by expert, so not streamed; see the
  guard below for why it no longer kills the run. Runs on the 12 GB test phone at 1.79 tok/s
  (UD-IQ3_XXS, dense weights pinned, 1250 MiB cache): the first model this engine has met that is
  compute-bound on device, 81 % of a token in compute against 11 % waiting on flash, because its
  4.3 GB dense side is walked every token. Pinned dense weights are a requirement here, not a
  tuning: `anon` swaps 6 GB to zram and stalls for 12-20 s on single tokens, `mmap` refaults the
  whole dense set every token (0.05 tok/s). Listed in the app catalog as a three-shard download
  (~82 GB on disk).
- **Engine reports 0.22.0.** `project(VERSION)` in `CMakeLists.txt` is moved with the app version
  this time, so no metrics CSV from this release names the previous engine.
- **Dense tensors larger than available memory stay mmap'd.** The dense policy assumed the
  largest dense tensor was an embedding or lm_head and read every dense tensor whole into its
  own buffer. On `qwen4exp` that meant Anonymous asked for a 28.8 GB allocation and Pinned hit
  the dma-buf ceiling, either way dying at load, for a table the graph touches a kilobyte at a
  time. A dense tensor larger than the kernel's `MemAvailable` is now held back under every mode:
  it stays mmap'd, leaves the warm sweep and the residency sensor (which would otherwise report a
  dense set that can never be resident), is not counted by the auto cache budget as a conversion
  to reserve for, and gets `MADV_RANDOM` so a fault maps one page instead of a readahead window
  the gather never touches. The rest of the dense weights still get the mode the run asked for,
  and one stderr line names what was held back and why. The bound is size, not access shape: a
  row-gathered table that fits keeps its mode, because demand-faulting one that fits measured
  −16 % decode (#135). Inert on every other supported model: Qwen3.6-35B reads the same 1785 MiB
  into the same 613 buffers under `anon`, and `warm` and `mmap` match their baselines to the
  decimal. See [docs/android-memory.md](docs/android-memory.md).

### Changed
- **llama.cpp submodule bumped** to `035e22731`, the head of upstream PR
  [ggml-org/llama.cpp#27742](https://github.com/ggml-org/llama.cpp/pull/27742) (Qwen3.8-Flash-Next
  support), with the expert-ready hook cherry-picked on top, still a single-commit delta. The pin
  is provisional: that PR is not merged, so when it lands the hook is re-applied onto master and
  the pin moves again. Byte-identity gates pass on the new base and, unlike the previous bump,
  nothing on our side needed adapting. App version 0.22.0 (versionCode 37).

## [0.21.0] - 2026-08-25

### Added
- **The expert cache says when it cannot hit.** One token's worst-case routed bytes are priced at
  load from the model's shape alone — every bound layer's expert-entry bytes times
  `min(top_k, n_expert)`, at the top-k the run actually applies — and the engine prints one stderr
  line when the resolved budget falls under it. Below that cycle global LRU evicts precisely what
  it is about to read, so the hit rate is 0 % while the run still pays the management and the RAM.
  It warns rather than refuses: the budget is legal and the output byte-identical, so the engine
  states the fact and leaves the choice alone. The number is not the fixed `cache_min_mb` floor,
  which is what made this invisible: a budget can clear the floor and still sit under *this* model's
  cycle, and `--n-expert-used` widens the cycle without touching the budget. Validated on device on
  Gemma 4 26B (cycle 902 MiB: 800 MiB measures a 0.0 % hit against 26.8 % at 950 MiB, decode
  1.42 → 2.01 tok/s across that gap) and Qwen3.6-35B (cycle 582 MiB: 500 MiB measures 0.0 % against
  36.0 % at 650 MiB). By @gjjkbssg (#165, #167).
- **`cache_cycle_mb` in the metrics preamble.** The same number, recorded next to the budget it
  should be judged against, so a committed CSV says on its own whether its cache could ever have
  hit — no second run of the same model and top-k to compare with. Named and explained in the app's
  metrics view like every other cache field, rather than falling through as a raw key. See
  [docs/telemetry.md](docs/telemetry.md) and [docs/cache-sizing.md](docs/cache-sizing.md).

### Changed
- **Finer expert-cache rungs below 2000 MiB in the app.** The ladder went 500 → 1000 → 2000, and
  that x2 is where the choice is sharp: on an 8 GB phone 1000 MiB runs and 2000 MiB gets the app
  killed by the OS, so the step handed the user a cliff instead of a setting. 1250, 1500 and 1750
  fill it; above 2000 the existing 1000 MiB step is a small fraction of the budget and is unchanged.
  Reported by @eiffel31 (#146).

### Fixed
- **The build reports its own version again.** `project(VERSION)` in the top-level `CMakeLists.txt`
  still said `0.19.0` after 0.20.0 was cut, and since that is the single source of `BMOE_VERSION`,
  every 0.20.0 build self-reports as 0.19.0 — in `bmoe-cli --version` and in the `engine=` line of
  every metrics CSV it wrote. Any CSV committed between 2026-08-17 and this release names the wrong
  engine; nothing else was affected. Reported by @gjjkbssg (#163).

## [0.20.0] - 2026-08-17

### Added
- **Ling 3.0 (`bailingmoe3`) recipe.** Ling-3.0-flash (127B total, ~5B active) routes over 512
  experts with a per-expert bias (the `lfm2moe` pattern) plus one always-on shared expert that
  stays resident; the experts name the standard split suffixes, so streaming is one registry row.
  The leading dense blocks never bind, and the trailing NextN/MTP block names expert tensors but
  is not even loaded (llama.cpp defaults `load_mtp=false`), so it never streams. The hybrid
  KDA/MLA attention stack is dense-side llama.cpp code, invisible to the streaming seam.

### Changed
- **llama.cpp submodule bumped** to upstream `3733366` (BailingMoE3 support) with the expert-ready
  hook rebased on top, still a single-commit delta over stock upstream. Byte-identity gates pass
  on the new base. App version 0.20.0 (versionCode 35).

### Fixed
- **`--mtp` kept working across the bump.** Upstream now skips the nextn/MTP tensors at load
  unless `load_mtp` is set, and the flag defaults to off, so the second context `--mtp` builds
  over that block would have found its tensors missing. Worse as a failure mode: `n_layer_nextn`
  is read from the gguf metadata and stays non-zero regardless, so the "this model has no trained
  MTP head" check would have passed and the trouble surfaced later. The block is now requested
  exactly when speculation asks for it. Verified on Qwen3.6-35B-A3B: 17/19 drafts accepted,
  3.43 tokens per verify decode.

## [0.19.0] - 2026-08-01

### Added
- **Split (multi-shard) ggufs stream natively.** Hugging Face rejects single files above 50 GB, so
  every large model ships as `-00001-of-0000N.gguf` shards — and until now the streamer assumed one
  file, forcing a merge with double the disk. `gguf_offsets` now fans the first shard out to the
  whole set and resolves every tensor to its (shard, offset); the expert streamer and the dense
  loader open one positioned reader per shard and route each read by the tensor's shard index. Pass
  the first shard, as with llama.cpp itself; a missing sibling fails the load with the shard named.
  The byte-identity gates gained a 4-shard fixture (metadata-only first shard, the layout large
  quants actually use) proving streamed == resident across shard boundaries.
- **The app downloads sharded models.** A catalog entry can list several shard files; they are
  fetched sequentially (each resumable, one aggregate progress bar, free space checked once
  against the whole remaining set) and the model list offers the first shard — the file the
  engine opens. gpt-oss-120b turns from a "merge it on a PC" recipe into a one-tap download, and
  DeepSeek V4 Flash UD-IQ2_M (~91 GB, three shards) joins the catalog. Deleting a sharded model
  deletes the whole set. App version 0.19.0 (versionCode 34).
- **DeepSeek V4 Flash (`deepseek4`) recipe.** V3.2-style expert routing — 256 routed experts with a
  per-expert bias (the `lfm2moe` pattern) plus an always-on shared expert that stays resident — over
  the standard split expert suffixes, so streaming is one registry row. The V4 attention machinery
  (compressed sparse attention, the lightning indexer and its dedicated KV cache) is dense-side
  llama.cpp code, invisible to the streaming seam. Requires a llama.cpp with DeepSeek V4 support
  (the pinned submodule has it).
- **`--ngram` — a second draft source for the same verify loop, drafting from repeated text instead
  of from a trained head (off by default).** It takes the last few tokens, finds where that exact run
  occurred before in the prompt or in what has been generated, and proposes whatever followed. No
  head, no draft context, no decode, no expert read — and it works on any model, including the ones
  `--mtp` refuses for want of a `nextn` block (verified on Qwen3-30B-A3B). Verification is unchanged,
  so it is exact in the same sense `--mtp` is and carries the same non-bit-reproducibility caveat;
  neither belongs in a byte-identity gate.
  It was built because of what the flash-split counter said about MTP, not on a hunch: at draft 3 on
  the host, **the head's own routing was 2.9% of the extra bytes a speculated run streams — the other
  97.1% was the widened verify batch**, which every source pays. So a cheaper draft producer is worth
  almost nothing, and the one property that could matter is that this source can *decline to draft at
  zero cost*, which the head cannot: below `--ngram-min-match` it proposes nothing, the batch is never
  widened, and the step is exactly a plain single-token decode.
  **Measured, that floor holds per step but not per run, and the feature does not win.** Host,
  Qwen3.6-35B-A3B-MXFP4 streamed, 256 greedy tokens, back-to-back with `off` run twice: on free prose
  6.51 tok/s against a 5.80/6.59 noise band — inside it, because 92.6% of steps drafted nothing. On a
  copy-heavy prompt 5.24 against a 5.45/5.65 floor — *below* it, because the 15% of steps that did
  draft widened the read set to 67.2 MiB/token (48–58 at baseline) and at 44.2% acceptance bought only
  1.20 tokens per decode. On the same prompt the MTP head accepted 82.4% and gained 15% effective.
  Acceptance is what pays for the widening, and a trained head has it. Drafting cost measures
  `0.0000 s/token` in every n-gram cell, exactly as claimed — it was simply never the constraint.
  A `--ngram-min-match` sweep settles it rather than leaving it open: raising the gate 3 → 5 → 8 lifts
  acceptance 44% → 75% while coverage collapses 15% → 3.4%, and narrowing to `--draft 1` reaches
  82.6% acceptance — the head's own figure on this prompt. Every cell climbs toward baseline **from
  below** and none crosses it; the best configuration found (`--draft 1 --ngram-min-match 5`, 5.56)
  lands on the floor. A drafting step is net-negative or neutral here, so tightening the knob only
  shrinks the exposure and its limit is the flag being off. Even at 82.6% a drafted step bought 1.08
  tokens per decode: a match rare enough to trust carries one or two tokens of evidence, while the
  batch it widens pays every position's independent routing.
  The device A/B agrees and adds one cost the host could not show. Test phone, same model streamed on
  the shipping recipe, cells thermally gated (a 120 s settle, then a battery-temperature gate, so all
  six start between 35.3 and 36.4 °C): prose 4.90 inside a 4.59–5.17 band, copy-heavy **3.14 against
  4.43** — a 29% loss, worse than MTP's 18%. The counter that explains it is new: **major faults per
  token go 126 → 1427** for a source that allocates no draft context at all. Those are the rollback
  snapshots. `n_rs_seq = draft_max` is asked for by *any* speculation, since rejecting a draft means
  rewinding the KV, and on a hybrid attention/SSM model that snapshot is a real allocation scaling
  with the context — so the n-gram source escapes MTP's draft context but not the loop's own memory,
  and on device that memory is the expert cache's. The prose cell, with a 16-token prompt, barely
  notices; the copy cell, with 224 tokens to snapshot, storms. The same run also re-measured MTP with
  the thermal confound removed — 3.64 effective against 4.43, so the earlier device verdict was not an
  artefact of benching without a cooldown gate — and reproduced the flash split at **3.7%** head
  against 96.3% widened verify batch, matching the host's 2.9–3.0%.
  Kept and shipped off: it is the only speculation available on a headless model, the per-step floor
  is a real property, and `--ngram-min-match` is an untested lever on precisely the failure above.
  The matcher (`core/include/bmoe/ngram_draft.h`) is pure policy over token ids with **no llama.cpp
  at all** — not even `llama.h` — so it sits on the clean side of the seam, adds no `common`
  dependency, and is unit-tested with no model (`tests/ngram_test.cpp`: tie-breaks, clipping,
  self-match exclusion, gate boundary). See `docs/ngram.md`.
- **`--mtp` — decode through the model's own MTP head, verifying a whole group per pass (off by
  default).** Qwen3.5/3.6 ship a trained multi-token-prediction block inside the gguf. With the flag
  on, that head drafts `--mtp-draft` continuation tokens and the target verifies all of them in one
  wider decode, confirming the longest prefix whose argmax equals what the target itself would have
  produced. Nothing is approximated and no weight is skipped, so the quality is the full model's —
  but it is **not** byte-identical the way `--overlap` and `--prefetch` are, and it must not be used
  in a byte-identity gate: the verify pass evaluates `1 + N` positions in one batch, and a batched
  matmul is not bit-identical to N single-token ones, so a near-tie can flip. Measured on
  Qwen3.6-35B-A3B-MXFP4 over 128 greedy tokens: exactly one token differs from an unspeculated run,
  draft widths 1 and 3 agree with each other exactly, and a plain-vs-plain control is exact.
  The prize is that a decode's dominant cost, moving the dense weights and the routed expert slices,
  is paid once per group instead of once per token; the counterweight is that the verify positions
  route independently, so a layer's read set widens toward `N × k` wherever adjacent tokens disagree,
  and the draft pass routes through the MTP block's own expert layer on top. Measured on the desktop
  host (DRAM-bandwidth-bound, model streamed at ~1.4× RAM): **+15.1%** at draft 3 with the host's
  best recipe (7.12 → 8.19 tok/s) and **+29%** without the lossy drop knob, with acceptance falling
  from 71% at draft 2 to 52% at draft 4 and flash bytes per token rising 19.7 → 33.7 MiB as the
  widening predicts. Draft 3 is the optimum here; 4 is worse than 2. On a flash-I/O-bound phone that
  balance can invert, so the flag ships off pending the device A/B. The orchestration is llama.cpp's own
  (`common/speculative.h`, public headers only): no fork, no patch, no submodule bump. The MTP block
  is streamed like any other layer — the hook and the expert source are sized `n_layer +
  n_layer_nextn`, the capture warm-up runs on the draft context too (the MTP graph exists nowhere
  else), and prefill is fed through the driver so the draft context's KV reaches the last prompt
  position. The loop also accepts **before** catching the draft context up, so the catch-up runs on
  the accepted prefix instead of the whole verify batch: acceptance depends only on the target's
  logits, which are already in hand, and the rejected tail was being computed only to be deleted a
  few statements later. Identical state, `n_draft − n_accepted` fewer positions through the MTP
  block per group — and since that block carries its own MoE FFN, on a streamed device those are
  expert reads that no longer happen. New telemetry: `mtp:` summary line, `mtp_batch` per-token column (a verify decode's
  whole cost is charged to its group's first row), `mtp_drafted` / `mtp_accepted` / `mtp_decodes` in
  the CSV trailer, and `mtp` / `mtp_draft_max` in the preamble. Needs a gguf carrying the `nextn`
  block — which Qwen3.6's ordinary quantisations do, verified from their headers, so no `-MTP-`
  conversion is required — and greedy decoding; both are rejected at load with a message rather than
  silently ignored. See `docs/mtp.md`.
- **`--mtp-p-min F` — stop drafting when the head is unsure.** The draft loop already knows the
  head's confidence in each token it proposes; this makes it stop below a floor instead of always
  filling the width. On a streamed device a draft not made is a pass through the MTP block — with
  its own MoE FFN — that never happens, *and* one fewer independently routed position in the verify
  batch. Default `0` (never stop), which is what the host numbers were measured at.

### Changed
- **The app's Settings are grouped by purpose, with an Experimental group folded under each.** Each
  category shows the recommended configuration first; the levers measured on one device, measured
  once, or still owed a measurement are one tap away rather than interleaved with the settings that
  are known to work. They stay in the release build on purpose: testing them on other hardware is
  what the demo app is for, and a lever nobody can reach is a lever nobody can refute. The caveat is
  stated once in the group header instead of leaking into some descriptions and not others.
  Every description was rewritten to say what a setting does for the person reading it, with the
  measured figures removed (a number needs the device, the model and the day beside it to mean
  anything, and none of that fits under a switch) and the implementation names with them:
  `O_DIRECT`, top-k, dma-buf, mmap and KV cache are not what someone deciding whether to turn
  something on needs to know. The metrics screen keeps the flag names deliberately, because there
  the reader is matching the UI against a CSV column.
- **The app's session signature is derived from the argv it would open.** It was a hand-written list
  beside `sessionArgv` with nothing keeping the two in step, and omitting a field there is a silent
  bug: the setting appears to change while the engine keeps running the old configuration. Three of
  the four merges in this release collided on exactly that list.
- **The demo app declares `android:appCategory="game"`.** Vendor performance layers read that
  attribute, and on the OxygenOS test device it moves the app onto the boosted path: with the app
  in the foreground the CPU ceiling went from 1.9/1.65 GHz to the hardware maximum of 3.32/3.80 GHz,
  measured before and after. Decode is the most CPU-hungry thing a phone does outside a game, so
  that is the right path to be on. It cannot be a runtime setting — a manifest attribute is fixed
  at install — and the effect belongs to the vendor rather than to Android: neutral on stock
  builds, and Samsung's game service has historically throttled what it classifies this way, so any
  per-device figure is a measurement rather than a rule. **In-app numbers taken before this change
  are not comparable with numbers taken after it.**
- **CI enforces the versions it claims to.** The format job installs `clang-format-18` explicitly
  instead of whatever the runner image ships, which happened to be 18 and would have started
  failing every PR against an unannounced version on the next image bump; the APK job builds with
  NDK r27c, the same release that produces published APKs, so CI stops validating a build nobody
  installs; and it passes `-DGGML_OPENCL=OFF`, the flag whose absence once shipped a stray backend
  into two releases. `actions/checkout` moves to v5 (v4 pins a deprecated Node runtime).
- **Third-party actions are pinned by commit SHA.** `nttld/setup-ndk` and `softprops/action-gh-release`
  ran from mutable major tags inside the APK job, which holds a `contents: write` token: whoever
  controls those tags upstream could have pointed them at unreviewed code with permission to rewrite
  this repo's release assets. Both now resolve to a fixed commit, with the human-readable version in a
  trailing comment. GitHub-owned actions stay on major tags, since pinning them would mean an SHA bump
  on every upstream release for a materially smaller risk. The signing keystore was never exposed to
  this: it lives only in `release-apk.yml`, which runs no third-party action and triggers only on
  events that already require write access.
- **`--help` lists every flag the CLI accepts.** `--io-trace` in particular was a fully documented,
  guarded diagnostic that the usage text never mentioned; `--version`, `-h`, `--prefetch-sync` and
  the two deprecated `--dense-weights` aliases are named now too. A flag that works but is not
  listed reads as an accident rather than a decision.
- **The speculation config generalised to a draft *source*, and the flags with it.** `MtpConfig`
  became `SpecConfig` with `DraftSource { none, mtp, ngram }`, and `--mtp-draft N` became `--draft N`
  because the width belongs to the verify batch, not to whoever filled it; `--mtp` and `--ngram`
  select the source and are rejected together rather than silently resolved by flag order. In the
  session the gate split in two — `spec_on` (wide batch, acceptance, rollback: both sources) versus
  `mtp_on` (draft context, `common/speculative.h`, the catch-up: the head only) — which is what lets
  the n-gram source reuse the whole verify half while allocating nothing. CSV preamble: `mtp=0|1
  mtp_draft_max=` became `spec=off|mtp|ngram spec_draft_max=` plus `ngram_min_match=`. The per-token
  and trailer counters keep their `mtp_*` names on purpose: they always described the loop rather than
  a source, `spec_*` already means the temporal prefetch in that trailer, and renaming would break
  every CSV already holding a measurement. The Android setting became a three-way "Guess ahead"
  picker, migrating the old boolean preference.
- **A speculative step that drafts nothing now takes the plain decode path.** It used to build the
  wide batch anyway — one token, all-logits, a logits row index of 0 — which was harmless but meant
  the abstain case was not free. It now falls through to `llama_batch_get_one` with row `-1`, byte
  for byte the unspeculated path. Required for `--ngram`, whose whole economics rest on it, and it
  also tightens `--mtp-p-min`'s zero-draft steps for free.
- **The MTP draft context's graph width drops from 256 to 32.** Compute buffers are reserved for the
  widest ubatch and the dominant term scales with `ubatch × vocabulary`; on device that reservation
  measured **493 MiB** for a context that evaluates one token per draft step and is handed at most
  `1 + draft_max` positions by the catch-up. On this engine memory is the expert cache's, and the
  device A/B showed exactly what it cost: major faults per token rose from ~69 without speculation
  to 633 at draft 3 as the kernel swapped and dropped file pages to find the room.

### Fixed
- **The cost of speculation is now measured rather than inferred.** Drafting happens between decodes,
  so it never entered `wall_ms` and `tok/s` never included it — a speculated run could report a rate
  the user was not experiencing. New `mtp_draft_ms` per-token column, `mtp_draft_s/tok` in the CSV
  trailer, `mtp_draft_s_tok` and `loop_overhead_s_tok` in `BMOE_DONE`, and a second `mtp:` summary
  line giving the effective rate next to the reported one. The Android app now reads the `mtp_*`
  keys it was already being sent and shows acceptance, tokens per pass and the real rate — before,
  the UI could not tell whether speculation had run at all.
- **The flash cost of drafting is now attributable.** A speculated run reads more bytes per token
  for two unrelated reasons — the head's own MoE FFN routes on every draft pass, and the verify
  batch widens the trunk's read set wherever adjacent positions disagree — and the route trace can
  see neither apart, since its framing brackets the target decode while the head only ever runs in
  the draft context. A third `mtp:` summary line now splits the run's streamed MiB between the two.
  They are attacked in opposite ways (a narrower draft shrinks the first, only better agreement
  shrinks the second), so a single total was not actionable. This is the counter that produced the
  2.9%/97.1% split `--ngram` was designed against, and it is why that design targets the abstain case
  rather than the drafting cost.
- **Coverage is now reported, so a speculative result can be read.** New `drafted_steps` in the CSV
  trailer and in `BMOE_DONE`, and an `ngram:` summary line giving it as a percentage of the verify
  decodes. The head drafts on every step, so for `--mtp` this is a constant; for `--ngram` it is the
  shape of the whole result, since the steps that abstained ran at exactly the unspeculated cost and
  the same delta means something quite different at 7% coverage than at 100%.
- The first device A/B is recorded in `docs/mtp.md`: on the test phone MTP **loses** at every draft
  width (5.59 at draft 2 and 4.38 at draft 3 against 5.82–6.14 baseline) because the shipping
  configuration is compute-bound — `stall_s/tok` is 0.025–0.027 whether speculation is on or off —
  while the read set widens 35–54% and CPU per token rises 28–67%. The flag stays off.
- **`--route-ahead N` — commit decode routing to the N-layers-early prediction (experimental,
  lossy, off by default).** Every prefetch lives under the same ceiling: layer L's routing needs
  layer L−1's output, so any predictor working earlier is approximate and every speculated read
  can miss. This flag inverts the bet — the expert selection of decode layer L is *replaced* by
  the ranking layer L's own gate produced on the hidden state N layers back in the same forward
  pass, so the selection is known N full layers of compute early and a prefetch of it could never
  be wrong (the training-free cousin of Pre-gated MoE, ISCA'24). The router still computes: its
  logits give the substituted experts their true renormalized weights via the graph's own weight
  chain, and each layer's gate input feeds the prediction for layer L+N. Layers 0..N−1, prefill,
  the first decode token and anything unreadable route normally and are counted as passed
  through. The `moe-route-ahead:` report line and the `route_ahead=` CSV preamble key record how
  many routings were committed and how far the committed selection sat from the router's own
  choice — the measured perturbation the run generated under. Mutually exclusive with
  `--predict-log` and both prefetchers; composes with the cache and expert dropping. With the
  LRU cache on, the committed selection is also acted on: layer L+N's ids are handed to the
  speculative read path the moment they are fixed (at layer L's load), uncapped — unlike every
  predictor before them these reads can never be wasted, so the `moe-prefetch: [route-ahead]`
  line reports a useful fraction that sits at ~100% by construction — and the issue list is
  drop-aware: committed experts predicted below the `--drop-cold-experts` threshold are left
  cold on purpose so the commit-time drop discards them unread, exactly as it does the router's
  own tail (measured on device before this filter: uncapped early reads un-dropped everything,
  3x the flash per token and half the tok/s at depth 4). The prediction itself runs
  on the barrier-less path predictive prefetch built — pointers stashed in the ask pass, the row
  copied at the topk callback, the GEMV on the shared worker thread, the sampled fresh-gate
  watchdog validating the read — so no graph barrier is added and the eval thread pays only a
  row copy per layer; a watchdog trip disarms the policy out loud, and a layer whose ranking is
  not ready when its topk fires keeps the router's own choice. On the streamer side, committed
  speculation is exempt from the destructive quiesce built for guessing predictors: a loading
  layer adopts its own committed jobs (finishing them instead of discarding and re-reading), and
  other layers' committed reads survive in the queue — without this, every early read at depth 2
  was destroyed before use. Host A/B under `--overlap` (fixed cache, same prompt): N=2 cuts the
  overlap stall 0.060 → 0.010 s/token (97% of early reads useful) for +20% tok/s end to end,
  with the generation still correct. **Device: components proven, end-to-end owed.** An I/O trace of every physical read
  (decode-only, equal length) shows the committed routing reads what the baseline reads — 133.6
  vs 134.4 MiB/token, the same 4% redundancy, zero speculative-then-demand pairs — so nothing is
  fetched twice, and the overlap stall falls consistently from 0.038-0.059 to 0.006-0.007
  s/token. **Device throughput, measured: +21%.** Interleaved cells (baseline / N=2, two passes,
  same prompt and length, so thermal drift hits both equally) put N=2 at 5.29 tok/s against 4.36
  for the baseline, with the overlap stall down 0.067 → 0.010 s/token and the compute residual
  essentially unchanged (0.135 → 0.146) — the win is the stall, not a trade. The same build
  measures only +4% inside the demo app, and the reason is not the engine: with the app in
  foreground the device caps its six main cores at 1.9 GHz (2188800 → 1900800 Hz, measured),
  which slows the compute the flag is trying to keep busy and shrinks the share of the token
  that was stall. Two real costs were found by instrumenting rather than arguing, and both are fixed:
  the issue path took the I/O lanes' mutex once per expert (~120 acquisitions a token; the same
  run with one lane did identical work at a quarter of the compute time), now 0.9 ms/token; and
  the gate GEMV was bandwidth-bound rather than compute-bound — ranking 256 experts re-reads a
  ~4 MB gate matrix per layer — now served from an int8 mirror with a quantized activation
  (aarch64 only; on x86 the float path is already vectorized and the detour costs more than it
  saves), 19 → 6 ms/token with the committed selection agreeing on 74.5% of slots against 74.7%
  for the exact weights. What is still missing is a clean throughput pair: after a long
  benchmarking session the phone's own baseline swung between 0.79 and 4.54 tok/s in the same
  cell — which is why the figure above comes from interleaved cells rather than absolutes. Every
  eval-thread cost the flag adds now reports itself in the CSV and the summary (`ra_issue_ms`,
  `ra_wd_ms`, `adopt_ms` inside mgmt, `drain_ms` inside compute, plus cache `evictions`/`rereads`),
  because five successive explanations of this feature's device cost were deduced from a residual
  and all five were wrong. One structural cost remains and is documented as the next step: the graph
  computes its own router matmul regardless, so this implementation runs two gate matmuls per
  layer where the design wants one substituted — removing the second one means changing the
  graph, i.e. the fork that already exists for the expert-ready hook. Quality is measured, not
  assumed: the committed generations match the baseline's on a four-prompt objective battery
  (4/4 correct in both, one answer byte-identical), on a 512-token essay, and on a second model
  of a different generation and quantization (+22% there); output is deterministic across
  repeated runs, which expert dropping is not. It is refused alongside self-speculation, and that
  exclusion is measured rather than reasoned: a verify decode is several positions wide, so the
  policy declines to commit on every one of them while still running its prediction and issuing
  early reads that have become ordinary speculation (a combined run committed 0 routings, passed
  249 through, and dropped its speculative usefulness from 100% to 81%). Cost without commitment
  is worse than either feature alone, so `validate()` rejects the pair instead of silently
  charging for it. See `docs/route-ahead.md`.

## [0.18.1] - 2026-07-29

### Fixed
- **A saved metrics CSV now states its whole configuration in the app (#136).** The engine has
  written every resolved knob into the `# bmoe_metrics v2` preamble for several releases, but the
  app displayed hand-picked subsets of it: the header card of an opened file listed eight fields,
  and the compare view rendered a 17-key whitelist that had quietly drifted behind the metrics
  sink. Expert dropping, predictive prefetch and its speculation width, the sampling parameters and
  the engine build that produced the rows were all in the file and none of them reachable — so a
  run could not say whether it dropped experts, let alone at what fraction, and an A/B whose only
  difference was one of those levers showed two configuration cards that looked identical. Both
  views now share one renderer over the whole preamble: the curated keys in order, then every
  remaining key under its own name, so a knob added to the sink shows up without an app change.
  `drop` and `predict` also joined the short run label, which is what a compare legend shows.
- **The main screen states the whole configuration too.** The reminder line under the prompt was a
  hand-picked subset of the same kind: it named the cache, the lanes and the threads but not the
  dense-weight policy, prefetch, predictive prefetch, or expert dropping — which is on by default at
  75%, so the default configuration changed the answers without the screen saying so. The line now
  carries the levers that make a run a different kind of run, and the full flag list is one tap
  below it, read back from the argv the session actually opens with rather than from a second list
  kept by hand.
- **The glossary explains the configuration too, in its own section.** The `?` reference covered the
  per-token columns only, so surfacing thirty-odd settings would have surfaced thirty-odd unexplained
  names. It now has two halves — the columns, and every preamble key described in the words its own
  definition uses — and it is reachable from Compare, where the configuration table matters most.
- **`loop_overhead_ms` is described at last.** The engine has written the column since 0.17.0 and the
  app never explained it: the time between two decodes, outside `wall_ms` and therefore outside the
  reported tok/s.
- **`predict_spec_max` no longer reads as a setting when the prediction was off.** The engine records
  its own default (2) whenever predictive prefetch is disabled — it is the one field of that block
  `session.cpp` does not neutralise — so a file claimed two speculated misses per layer for a run
  that speculated nothing. It now renders as inert. The engine-side fix is tracked separately;
  nothing about how those runs executed changes, since the value is never read with the feature off.
- App version bumped to 0.18.1 (versionCode 33).

## [0.18.0] - 2026-07-28

### Added
- **`--io-two-wave` — publish a layer's read batch in two waves (experimental, off by default).**
  A cold layer's batch used to become visible to the I/O lanes only after every miss took its page
  commits — roughly two dozen syscalls of bookkeeping sitting in front of the first byte of I/O on
  an eight-miss layer, which is exactly the latency-to-first-slice the sidecar refutation
  identified as the binding constraint (#118). With the flag on, the first projection's jobs (the
  ones `mul_mat_id` blocks on first) are committed and published immediately, and the remaining
  projections are committed and appended while the lanes already read. The drain protocol grew the
  one thing it needed — a worker that finished wave one comes back for a batch that grew in the
  same generation — and a new gate (`G4d`) holds the two-wave output byte-identical to serial
  streaming. Measured on device the same day (#120): stall is flat across cells, so the flag stays
  off; the mechanism and its gate remain for a cache-poor regime where cold layers dominate.
- **Release APKs are built by CI, not uploaded by hand.** A `release-apk` workflow runs when a
  release is published: clean checkout of the tag, NDK build of the CLI with the same flags and
  explicit staging list as `scripts/build-android.ps1`, APK build signed with the stable release
  key from repository secrets, a content check that fails on any stray library, and the assets
  attached to the release. Both build types now sign with the stable key when it is available, so
  the debug APK also updates in place instead of demanding an uninstall that wipes downloaded
  models.

### Changed
- **`BMOE_PROGRESS` carries the answer as a delta, not cumulatively.** Every per-token line
  repeated the whole answer and reasoning so far, so a generation of n tokens wrote, JSON-escaped
  and made the app parse O(n²) bytes — megabytes of pipe traffic for a thousand-token reasoning
  answer (#119). The line now carries `delta_reasoning`/`delta_text` (the tail since the previous
  line) and the reader appends; when the chat parser retroactively reclassifies answer text as
  reasoning (a closing think tag arrives) the line carries full snapshots with `"reset":1` and the
  reader replaces. The full final text still travels in `BMOE_DONE`, and the app parser
  accumulates in a builder instead of re-copying strings. Protocol readers: both emitters (one-shot
  `--progress` and `--session`) changed together, and [docs/telemetry.md](docs/telemetry.md)
  documents the new fields.
- **The overlap hook's per-expert bookkeeping got out of the kernel's way.** Three findings from
  the audit's leftovers (#123): the readiness lookup every compute thread ran for every routed
  expert was a hash-map probe — now a binary search over a flat sorted array, static after init;
  the pre-block spin was 2048 `sched_yield` syscalls — up to a millisecond of scheduler churn per
  genuinely slow slice, stealing CPU from the I/O lanes — now 256 single-instruction pauses
  (`isb`/`pause`) before registering as a waiter; and a cache hit on the overlap path no longer
  pays an LRU promotion that the token-major promote loop overwrote unconditionally two steps
  later. LRU order, readiness semantics and the gates are unchanged.
- **The gguf header is parsed once per run, not once per consumer.** The tensor-offset read and
  the model-info read each ran `gguf_init_from_file` — a full KV walk of a multi-GB file's
  header. One lazy parse now serves the top-k override, the route trace, the run info and the
  streamer's offsets.
- **With the drop policy armed, only the weight node that decides is isolated.** The router-weight
  chain is `ffn_moe_weights → (_softmax | _norm) → (_scaled)`, and the engine asked for **every**
  node of it so it could learn which one comes last. But once a layer's terminal node is known,
  that is the only one either consumer reads — the drop policy decides there, and the route trace's
  last-wins gather lands there — so the other two or three asks per layer bought nothing and cost a
  graph split plus a full compute-thread synchronization each, per layer, per token. They are now
  asked for only while a layer is still learning its chain shape (the first graph of a run, and any
  graph after a terminal that stopped appearing is forgotten), so the learning stays self-correcting
  and a graph that moves is detected exactly as before. This ran inside every `--drop-cold-experts`
  measurement to date, including the shipping app default.

### Fixed
- **The Android build script can no longer ship a backend nobody chose.** Staging swept every
  `libggml*.so` it found in the build tree into the app's `jniLibs`, and the build directory's cmake
  cache still carried `GGML_OPENCL=ON` from a GPU experiment — so a `libggml-opencl.so` no shipped
  configuration loads rode along into the v0.16.0 and v0.17.0 APKs (the release assets have been
  rebuilt without it). The script now forces `GGML_OPENCL=OFF`, wipes `jniLibs` before staging, and
  copies an explicit list of libraries; a missing one fails the build instead of a stray one
  shipping.

### Measured
- The whole release, on device against v0.16.0 at matched clock caps (#120): throughput inside the
  session's thermal noise — the shipping recipe is I/O-floor-bound, so removed work cannot move
  tok/s — while `cpu-s/token` fell 14–25% and `majflt/token` roughly halved. The removed work is
  real; it shows up as battery and thermal headroom rather than speed.

## [0.17.0] - 2026-07-28

### Added
- **`loop_overhead_ms` — the time the reported `tok/s` never counted.** `wall_ms` brackets
  `llama_decode` and nothing else, which is what makes `compute_ms` a clean residual; it also means
  everything *between* two decodes — sampling, detokenization, rendering the answer for a UI, the
  sink writes — falls outside `wall_ms`, outside `gen_seconds` and so outside `tok/s` entirely. A
  change that moved only that region was invisible in `s/tok` while being paid on every token. It is
  now a CSV column, plus `loop_overhead_s/tok` in the summary (which also carries the tail after the
  last token that no row can hold). Read next to `s/tok`: the two together are what a caller waits
  through.
- **The metrics CSV records the whole run configuration, and says which engine produced it.** The
  preamble had 18 keys and had gone stale against the last several releases: `--ubatch` (which sets
  the compute-buffer reservation, and so moves the very memory columns underneath it),
  `--predict-log`, `--predict-spec-max`, `--prefetch-sync`, `--drop-no-renorm`, `--drop-in-prefill`,
  `--cache-floor-mb`, `--load-all`, every sampling parameter and the trace granularity were all
  absent — so a file could not tell a probed run from a benchmark run, or a stochastic run from a
  greedy one. All are now recorded (`# bmoe_metrics v2`), along with `engine=<version>`.
  `think` is deliberately still absent: it belongs to a request, not a session.
  The preamble itself is now documented in [docs/telemetry.md](docs/telemetry.md), which described
  every other `#` block but not this one.
- **`bmoe-cli --version`**, and a project version in CMake for it to report. There was no version
  string anywhere in the engine; a committed benchmark CSV could only be dated by the commit that
  copied it in.

### Fixed
- **Router weights are located by the routing's shape, not by a test a top-1 routing defeats.** The
  helper that finds token `j`'s slot `k` inside a weight node told the 3-D `[1, nu, nt]` node from
  the norm variant's pre-reshape 2-D `[nu, nt]` by asking whether `ne[0] == 1`. With `n_expert_used`
  of 1 the 2-D node *is* `[1, nt]`, passes that test, and is read with the 3-D token stride — so
  every token after the first is read from the wrong row, and under `--drop-cold-experts` in prefill
  the policy's writes land on the wrong slot. It now matches the full extents against the routing's
  own width and batch size, which separates the two shapes exactly; where both fit (a single token
  at top-1) they name the same element. No shipped model in the catalog routes top-1, so this was
  latent rather than active.
- **A failed page commit can no longer blacklist an expert from speculation for the rest of the
  run.** `prefetch()` pushed a speculative read job per projection as it went, but recorded the
  entry's pending count only after the last one. If a commit failed part-way through an expert, its
  earlier jobs stayed queued against a count that was never set: a worker would decrement it below
  zero, so the entry could never complete, the quiesce never saw it listed to release its pages, and
  every later prefetch of that expert was skipped by the "already queued" test. An expert's jobs are
  now staged and published as a unit, and a part-way failure hands its pages back. The path is
  reachable only where page commit can fail (it is a no-op on POSIX), and only with prefetching on.
- **`prefetch()` no longer holds the I/O mutex across its page commits.** It runs on the eval thread
  immediately after a real batch was published, so a syscall per projection inside the lock stalled
  the very lanes trying to pull real read indices out of it — undercutting the invariant that
  speculation never delays real work. Pages are committed before the lock is taken; the lock now
  covers only the bookkeeping.
- **A failed bounce reallocation no longer kills the lane for good.** `FileReader::read` freed the
  old buffer before allocating the new one but left the recorded size behind, so after a transient
  allocation failure the next *smaller* read saw enough capacity, skipped the realloc, and read into
  a null pointer — every subsequent read on that lane failing forever. The size is now cleared with
  the pointer, so a failure costs one read instead of the lane.
- **The buffered fallback stops paying O_DIRECT's mechanics.** When the platform refuses O_DIRECT, or
  the open-time verify catches storage that mis-serves it (a FUSE-backed volume), reads still aligned
  their window outward, staged into the bounce and memcpy'd the interior out — an extra copy of every
  byte plus a leading partial-block over-read, in the mode that is already the slow one. Buffered
  reads now go straight into the caller's memory, and report the bytes they actually moved rather
  than a window they never pulled.
- **`--compute-trace` and `--io-trace` are no longer ignored in `--session` mode.** The CLI parsed
  the flags, opened the files and wrote their headers, then dropped both sinks on the floor when it
  entered the session loop — leaving an empty trace and nothing saying why. Both are now passed
  through, so they work in a session exactly as in a one-shot run.
- **A missing buffered tail fd is reported where it happens.** Its open was unchecked, and a read
  reaching the file's sub-alignment EOF tail then fell back to the O_DIRECT fd — which must reject a
  length that short — surfacing fd exhaustion as an unexplained read failure. It is now reported at
  open, and a tail read that cannot be served says which fd is missing.

### Changed
- **The ask pass stops parsing node names with `sscanf`.** The eval callback is offered every node
  of every graph, and each one was run through one to three `sscanf` calls — a format-string parse,
  locale machinery and all — to answer a question that is settled by the first eight characters.
  Every routing node this engine looks for is named `ffn_moe_…`, so one `memcmp` now gates all the
  matching and the thousands of nodes that are not routing nodes leave having done nothing else;
  the layer index is parsed with a digit loop, which is also stricter than the `%d` it replaces
  (`12abc` no longer reads as layer 12). A layer's terminal weight-chain node is remembered as the
  index of its name rather than a copy of it, so learning it costs no allocation and matching it is
  an integer compare instead of a string one — on a path that ran for every weight node of every
  layer of every token with the drop policy armed. No routing decision changes; the byte-identity
  gates cover it.
- **A completed slice read wakes a compute thread only when one is actually waiting.** Every
  completed read took the readiness mutex and ran `notify_all`, waking *every* compute thread
  blocked on *any* expert so each could re-check a predicate that was almost never its own — and
  paying the mutex even in the common case where nobody was blocked at all, because the slice landed
  inside the spin. Waiters now register themselves, and the publisher consults that count first.
  Registration and publication are both sequentially consistent, so the two cannot miss each other:
  either the waiter sees the flag already set and never sleeps, or the publisher sees the
  registration and notifies.
- **The generation loop stops re-parsing the whole answer for readers that do not exist.** Building
  a token's rendered view means parsing everything generated so far — the chat parser cannot resume
  — so it costs O(n) per token, O(n²) over a turn, and off the chat path it was a full copy of the
  generation on top. It ran unconditionally, including for the plain CLI output that writes only
  `piece` and for every benchmark run, which read neither field. `GenerateRequest::render_text` now
  says whether anything will read it: the line protocol sets it (a UI renders a running answer), the
  default CLI path and `run()` without `--progress` do not. It defaults to on, so an embedder that
  has never heard of the flag keeps the previous behaviour.
- **The finished answer is parsed once instead of twice.** The returned text and the history commit
  asked the same question of the same string and each paid its own full non-partial parse.
- `vm_reserve` maps with `MAP_NORESERVE`, making the address-only contract true rather than
  true-by-default: the expert cache reserves each span at full size and commits only resident
  slices, so the untouched remainder should never be charged against a strict overcommit limit.
- `pio::file_size` asks `fstat` instead of seeking to the end and back. Every consumer reads with
  `pread`, so the fd position was mutated to answer a question about the file and nothing used it.

## [0.16.0] - 2026-07-28

### Added
- **`--ubatch N`: the widest graph computed at once, decoupled from the context.** Compute buffers
  are reserved for the worst-case graph, and the engine had always set `n_ubatch = n_ctx` so that
  any fitting prompt prefills in one pass — which quietly ties **resident memory** to the context
  rather than to the work. Measured at n_ctx 2048: **320 MiB of compute buffer, falling to 80 MiB
  at 512**, scaling exactly with the context. On an engine whose whole problem is that the expert
  cache and the dense weights compete for RAM, that is a real budget, and it was invisible.
  Decode is unaffected — a decode graph is one token wide whatever this says; the cost is prefill
  throughput, which processes a long prompt in more, smaller passes. Default 0 keeps the previous
  behaviour exactly.

  Found while chasing what looked like a catastrophic slowdown and turned out to be this
  reservation pushing an already-tight system into reclaim — nearly 6 000 major faults per token,
  none of them the workload's fault.

- **`--predict-log` — measure how predictable the routing is, without acting on it.** For every
  decoded token the engine ranks each layer's experts a layer early, by running the **next** layer's
  router matrix on the **current** layer's gate input — the residual stream barely moves between
  layers, so the stale input ranks nearly as the real one will. It reports what fraction of each
  routing that would have had in flight, scored against the previous-token bet
  [`--prefetch`](docs/prefetch.md) already makes, per layer and in aggregate. Training-free and
  model-unchanged: the router matrix is a dense weight already resident, and the prediction is one
  GEMV.
  Alongside both it prints a **zero-staleness control** — the same row read, GEMV and ranking on the
  layer's own matrix — which must reproduce the routing llama.cpp computes from those same tensors.
  A control below 100% means the probe is wrong, or the architecture does not select by raw-logit
  ranking; either way it caps what the measurement could show, and the CLI says so rather than
  letting the gap be blamed on staleness. The byte-identity gates cover both halves (`G9`).
  Diagnostics only: nothing it computes reaches the loading path, so a probed run reads exactly the
  bytes an unprobed one does — but it costs a barrier and two GEMVs per layer, so it is not a
  benchmark run. Requires `--moe-stream`. See
  [docs/expert-prediction.md](docs/expert-prediction.md), which also states why a *good* score still
  would not imply a faster decode on a flash that is already saturated.
- **`--predict-prefetch` — speculate on that prediction instead of the previous token.** Feeds the
  stale-gate ranking into the same speculative read path as `--prefetch` (same cache buffers,
  accounting and summary line, tagged `[stale-gate]`), replacing a ~43%-accurate predictor with a
  ~89%-accurate one measured on the same run. Issued after the current layer's load rather than at
  prediction time — every load path starts by quiescing speculation, so an early queue would be
  cancelled before a lane picked it up. Drop-aware: with `--drop-cold-experts` armed, predicted
  experts below the drop threshold are not speculated (if they miss they would be dropped unread —
  reading them ahead spends the I/O the policy exists to save). Mutually exclusive with
  `--prefetch`; needs the LRU cache; byte-identical like the temporal prefetch (gate `G10`), with
  the same documented exception under dropping, where a correct guess un-drops an expert and buys
  quality rather than speed. Off by default. `--predict-spec-max N` bounds how much flash the
  prediction may spend per layer (0 = retention-only: predicted residents are LRU-protected via the
  new `IExpertSource::retain`, and nothing is read ahead). Rebuilt once already around its measured
  costs — native-F16 GEMV on aarch64 (~40× over a per-element exported-function conversion),
  barrier-less gate-input capture guarded by a sampled self-validating watchdog, prediction GEMV on
  a dedicated worker at an L+2 horizon (the probe's stale-2 column prices that staleness per
  model). Throughput verdict, from thermally matched pairs (the first day's comparisons were
  invalidated by silent thermal capping): **read-ahead loses** — −21% in the shipping drop+pinned
  configuration at spec-max 2 and −28% on 8 io lanes, each with hit rate up and most speculations
  useful, because the flash has no spare bandwidth to spend; **retention is hit-rate-neutral** at
  a 3000 MiB cache, as the offline replay bound predicted (see docs/expert-prediction.md). The
  example app exposes it as an experimental toggle in the Streaming section (off by default,
  mutually exclusive with the temporal-prefetch setting, spec-max rungs 0/1/2/4 defaulting to 0 =
  retention-only, the only rung the matched pairs did not refute).

### Fixed
- A stray `%` in the `--dense-weights` help text was read by `printf` as a conversion specifier, so
  that line printed garbage and read past the argument list.
- `scripts/build-android.ps1` now judges `cmake` by its exit code instead of by whether it wrote to
  stderr, so it works under Windows PowerShell 5.1 and not only under `pwsh` (the NDK toolchain
  prints progress to stderr, which 5.1 turns into a terminating error).

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
