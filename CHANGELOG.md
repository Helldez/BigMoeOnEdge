# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
Semantic Versioning.

## [Unreleased]

### Added
- **The first run on ROCm: a rented 192 GB AMD datacenter card, the model resident**
  (`docs/bench-data/2026-10-28-rented-rocm/`, `docs/benchmarks.md`, `docs/platforms.md`, the README
  table). Qwen3.8-Flash-Next Q2_0 fits the card whole: `--auto` decodes 53.4 tok/s over 1024 tokens
  against llama.cpp's own 55.6-55.8, and long prompts prefill at 2.1-2.3k tok/s. With ROCm's clang
  the build hangs on ggml-cpu's AMX kernels; the bench notes give the line that builds the host code
  with gcc.

### Changed
- **The README's speed table is grouped by hardware, in whole tokens per second.** `README.md` and its
  five translations list each machine once (phone, iPhone, Apple-silicon laptop, Windows laptop, two
  NVIDIA desktop cards, an AMD datacenter card), with its memory and where the experts are read from
  (flash, RAM, GPU). The phone rows of gpt-oss-120b, Qwen3-30B-A3B and Gemma-4-26B-A4B and the
  laptop row of Qwen3-Coder-30B-A3B repeated what the others show: they are in `docs/benchmarks.md`
  now, with every exact figure. `scripts/readme-diagrams.py` labels its
  figures the same way (the bars keep the measured values). The description catches up with what
  shipped since 0.36: CUDA and HIP among the released backends, the device memory as a tier of the
  expert cache, plans from the model's own routing record, and the coding agents the server serves.
  The translations also gain the CUDA and HIP accelerators and the rented-card runs they lacked.

### Removed
- **The Obtainium link for Android.** `README.md` (and its translations), `examples/android/README.md`
  and `docs/platforms.md` no longer point to an Obtainium entry: its redirect page depends on the
  browser handing the link to the app, which often does nothing, and Android's developer verification
  (from 2026-2027) makes a third-party installer a second thing that has to keep working. The APK is
  installed from the release page, as before.

### Fixed
- **`bmoe-server`'s launch flags outrank saved settings, as `--help` says** (#435). The settings
  layers applied a value saved by an earlier run after the flags of this one, so
  `bmoe-server -m A.gguf -c 32768` loaded the model and context `settings.json` held instead. The
  order is now defaults < plan < saved values < launch flags; a value set from a client while the
  server runs still replaces the flag's, as before.

## [0.38.0] - 2026-10-27

### Added
- **A rented 16 GB card with 30 GiB of RAM, against llama.cpp and Strata**
  (`docs/bench-data/2026-10-27-rented-16gb/`, `docs/benchmarks.md`, the README table). The class of
  a home PC with a mid-range card, where Qwen3.8-Flash-Next streams: `--auto` decodes 19.3-20.8 tok/s
  over 1024 tokens with #431, 27 from the 768th token, against llama.cpp's own offload at 16.6-18.9
  over 256 tokens and Strata at 36-67 (50-66 over 1024). The prefetchers, predictive prefetch and
  route-ahead all lose beside the device cache, measured; the head drafting (`--mtp-head`) does not
  pay yet. What separates the two engines there is written down with the numbers. The README's table
  also gains the 24 GB card's rows of 2026-10-26.

- **An MTP head from a file of its own** (`--mtp-head FILE`). Some quantised files leave the trained
  head out (the GSQ-RCO quants of Qwen3.8-Flash-Next), and llama.cpp's converter writes it on its own
  (`convert_hf_to_gguf.py --mtp`). The head is opened as a model of its own, whole on the host, and
  drafts in a context that reads the target's hidden states, as llama.cpp's speculative driver does
  with a draft head; the target loads as it is. `scripts/fetch-hf-subset.py` fetches only the
  tensors a set of patterns names from a Hugging Face checkpoint, by HTTP range requests (each must
  come back as the range asked for), into a directory the converter reads like the checkpoint, so a
  head is a few GB of download instead of the whole model, eight range requests at a time.
  `scripts/convert-mtp-head.py` runs llama.cpp's converter with the pre-tokenizer name taken from the
  model file the head drafts for, where the converter's own table does not know the tokenizer yet.
  `docs/mtp.md` has the three commands. Measured on a 24 GB desktop GPU with Qwen3.8-Flash-Next:
  45.1 tok/s against 30.5-33.8 without the head.
  Where the plan arms a device expert cache, the head follows the experts to that device
  (`mtp-head-on-devices`, a measured decision) and takes its bytes from the cache, never from the
  target's layers; on a 12 GB card with 8 slow cores the head drafting on the host cost 35 ms of a
  65 ms token and lost to no head at all.

- **The stack measured on rented desktop GPUs** (`docs/bench-data/2026-10-26-rented-gpu/`,
  `docs/benchmarks.md`, `docs/community-benchmarks.md`). A 24 GB card, `--auto` only: Qwen3.6-35B
  resident on the card at 217 tok/s, Flash-Next 125B with its hottest 18 GB of experts cached on the
  card at 70, DeepSeek V4 Flash streamed from the drive at 9.3 with 94% hits. The same DeepSeek file
  through llama.cpp's own offload decoded at 18.5 from the same cold page cache, which is the margin
  rule of #419 and the control-group page cache of #418, both filed with the numbers. A 12 GB card
  on a 42.6 GiB control group: `--auto` armed the device tier and was killed at 43 GB (its host-side
  memory is not in the ledger, #424); with the tier off, Flash-Next ran resident at 15.5 tok/s, and
  the MTP head drafting on 8 slow cores cost more than it saved. With CUDA and Vulkan built together,
  the Vulkan device reported no PCI id, so the backends were not grouped and the tier priced the
  slower one (#421).

### Fixed
- **`--auto --overlap` no longer refuses to start when the plan does not stream.** Asking for
  `--overlap` keeps the capacity fitter's whole layers on the card instead of the device cache, and
  where what is then left on the host fits in RAM the plan turns streaming off, leaving `--overlap`
  set: the run stopped with `moe.overlap requires moe.enabled` (a 16 GB card with 30 GiB of RAM and
  Qwen3.8-Flash-Next). Every knob that acts only on the streamer's reads (`--overlap`,
  `--row-stream`, `--route-ahead`, `--predict-log`, `--predict-prefetch`, the overlap trial) is now
  turned off with a decision saying why wherever the plan does not stream.
- **`bmoe-server` loads a model the plan gives a device expert cache.** The server arms `--overlap`
  by default, and the device tier left it on beside the cache it armed, which the session refuses
  (`--device-cache-mb computes the experts on the device; --overlap waits for them on the CPU`): on a
  16 GB card every server load of Qwen3.8-Flash-Next failed where the CLI ran. The device tier now
  turns a default overlap off with a decision where it arms the cache; one the caller asked for
  still keeps the fitter's placement, as before.
- **The expert cache in RAM takes what the plan leaves unspent.** With a routing record, `--auto`
  sized the cache where the record's hit curve stopped rising and left the rest of the RAM idle; the
  record is a few hundred tokens, so its curve is flat past the experts those tokens touched, and a
  longer answer reads every expert it had not seen from the drive. On a 16 GB card with 30 GiB of RAM
  and Qwen3.8-Flash-Next over 1024 tokens, the plan's 15.7 GB cache held 83% and decoded 11.6 tok/s;
  22 GB held 96% and decoded 20.7 (27.5 over the last 256 tokens). The ranking still prices the cache
  on the curve against the resident groups; what none of them wanted now goes to the cache, with a
  `cache-remainder` decision. The planner test that asserted the old stop is updated.
- **The model files' read-ahead is handed back after load.** llama.cpp asks for the whole mapping to
  be read ahead when it loads (every non-lazy range), so a streamed model filled the page cache with
  bytes the run reads otherwise: on a 30 GiB box, 23 GB of one shard and 21 GB of the other. Under
  pressure the kernel swapped the expert cache out beside them, and the cache brake halved it twice
  (20.7 GB to 5.2 GB, the last 256 tokens of 1024 at 9.8 tok/s against 27.4 without). Where the dense
  set is held apart from the mapping (`--dense-weights anon` or `pinned`), the session now advises the
  kernel that the files' cached pages are not needed (`posix_fadvise DONTNEED`; a page still mapped
  stays). A no-op on Windows and Darwin.
- **Speculation's memory is in the placement.** The rollback snapshots speculation keeps in the
  target's context, and the MTP draft context's own state and compute buffers, were not in what
  llama.cpp's capacity fitter placed the model against, so on a card the fitter had filled the
  draft context's 0.9 GB of compute buffers ended the first prompt in a CUDA out-of-memory. The
  placement probe now hands the draft context to the fitter as the second model sharing the
  devices.
- **The tables the plan leaves mapped stay mapped.** The allocation prices the embedding group on
  its own lane, and where that lane is not residency (the graph gathers rows from it, so a copy buys
  bytes nothing reads) the run's `anon`/`pinned` dense policy still copied every dense tensor it was
  handed: a 28.8 GB per-layer table that "fits" by the kernel's count went into memory the ledger had
  sized the expert cache without, and on a 12 GB card's 42.6 GiB control group the run was killed at
  43 GB (#424). The plan now hands the run the tables it priced as mapped (`dense-mapped`, names read
  off the file's own grouping, a `[derived]` decision), the run holds them back as it holds back a
  tensor too large, and llama.cpp's lazy prefetch of such a table stays off. Nothing names a tensor
  or a size; a small table the allocation finds worth a resident copy is copied as before.

### Changed
- **The device expert cache is sized from the card's free memory, not from the fitter's whole
  layers** (#428). `--auto` used to turn exactly the bytes llama.cpp's capacity fitter gave whole
  layers' experts into `--device-cache-mb`, so the cache inherited the fitter's granularity: the
  memory left under one more whole layer was never used, and a card whose dense part fit with no
  whole layer beside it (the 8-12 GB class) got no cache at all. The placement probe now reads what
  the fitter left free on each card past its margin, off the same free figure the fitter planned
  against, and the cache takes it beside the experts' bytes. On several cards the budget is the
  largest whose share, split by layers as llama.cpp splits it, fits every card. The floor is the RAM
  cache's: one token cycle, under which a cache evicts an expert before the same token needs it
  again; below it the fitter's placement stands, and an MTP head on the device never takes the cache
  under it. Planner tests for the remainder, a card with no whole layer, a card under one cycle and
  two cards. Not yet run on a card.
- **The benchmark script measures the plan the planner learns, not only the first load.**
  `scripts/bench-report.sh` ran `--auto` once with nowhere to keep the model's routing record or
  the lossless knobs' verdicts, so its row was always a cold plan: the cache priced by a coefficient
  rather than the model's own hit curve, `--overlap` never armed, the thread verdict lost. The app
  and the server keep both, so a community row and an app row measured two different plans. The
  script now keeps the record under its output folder (`--routing-dir`) and runs twice: the first
  load records and measures, the second plans on the record and is the row for the table. Both are
  printed, each with its `mode:` line and its `plan: predicted ... measured` line, so a row also
  says how far the planner was from the machine. A third run is the second with `--overlap`
  pinned: overlap changes no output, the planner arms it only when its trial measured it 10% faster,
  and the row says what that margin left on the machine. `BENCH_RUNS=2` skips it, `1` reports the
  first load alone.
  The script waits between the runs (`BENCH_SETTLE`, 30 s): the kernel returns the first run's
  locked memory in the seconds after it exits, and a plan made before that sized a cache half the
  right one on a 16 GB laptop; when the second plan still saw much less memory than the first, the
  output says so. Numbers it prints are in the C locale, a decimal point in every language.
- **The plan prints the storage curve it priced on.** `--plan-explain` adds a `plan: storage:`
  line: whether uncached reads were in effect and the measured rate at each request size and lane
  count, with the refault price when measured. On macOS a shell cannot bypass the page cache, so the
  benchmark script's storage column there is this probe's one-lane rate at the engine's request size
  (read with `F_NOCACHE`), labelled as such, instead of a `dd` through the cache; the drive's name
  comes from `diskutil`. The Linux column stays the `O_DIRECT` `dd` probe.
- **The Plan screen is one screen on every front-end, and its figures arrive when the answer
  does.** The plan now lives in each UI's engine state (the web app's server store, the Android
  `Engine`, the iOS `Engine`), read when the server is reached, when a session opens and when a
  turn ends, whichever screen is open: before, each Plan screen held its own copy and re-read it
  only while it was the screen showing, so on a phone the measured figures appeared when you came
  back to it, not when the answer ended. The three UIs now share one layout: one banner for
  whatever went wrong with Measure again beside it, the measurement's stages, the Plan-before-
  every-load switch above the plan, the verdict with three figures (decode, cache hits, memory),
  Apply and Measure again together under them and nowhere else (the Android top bar loses its
  refresh icon, the iOS error loses its Dismiss), then memory, devices, decisions, the command and
  Copy diagnostic with the link to how the planner decides. The worst-case tile is gone from all
  three: `predicted.s_per_token_worst` is the cost model's own bound (one token with nothing
  resident, where every tier's saving is subtracted from), stays in the API and the diagnostic, and
  told a person nothing the expected figure did not. A decode figure is compared to the plan's
  prediction (`+26%`) only when the loaded model runs that plan.
- **`GET /api/plan` with a model loaded answers the plan Apply would apply.** With a model loaded,
  the load's plan is answered (`from_last_load`) only while nothing was measured after that load;
  `?cached=1` after a measurement made by hand with the model resident answers that measurement
  (`cached`), the one Apply applies, now with `measured` beside it too (before, a cached plan never
  carried the model's figures, so a Plan screen showing one never updated after an answer). The
  rule is `held_plan` in `server/plan_view.h`, with its test.

## [0.37.1] - 2026-10-26

### Fixed
- **The real-layer probe leaves integer tables out of the layer's matmuls.** A block's 2-D
  `.weight` tensor can be an integer index map (DeepSeek V4 Flash's token-to-expert table, `I32`):
  the graph gathers rows from it, no kernel multiplies it, and the CPU backend answers `supports_op`
  for such a matmul and then calls no kernel, so `--auto` and `bmoe-server` crashed at the probe on
  every DeepSeek V4 Flash file. A weight the probe prices is now one a matmul reads as floats, by
  ggml's own type traits.

## [0.37.0] - 2026-10-26

### Added
- **CUDA and HIP in the releases.** x86_64 Linux and Windows releases get a CUDA pack
  (`bmoe-cuda-<tag>-<target>`): the CUDA backend module with the NVIDIA runtime and cuBLAS it links,
  built from NVIDIA's redistributable archives checked against their SHA-256
  (`scripts/fetch-cuda-redist.py`), for every NVIDIA generation from Turing on. It is a download of
  its own, about twenty times the rest, so a machine without an NVIDIA card does not fetch it:
  extracted into the folder of an archive or of the installed app, it is loaded with the other
  modules (the read-only AppImage cannot take it; the Linux archive can). The x86_64 Linux archives
  and packages also carry the HIP module, built in AMD's ROCm image for RDNA 2-4 and the Instinct parts, and
  loaded only where ROCm 7 is installed. A module whose driver the machine lacks is not loaded, so the
  same download still runs on a machine with any GPU or none. The Windows x86_64 build now uses Ninja
  with the MSVC environment, which CUDA needs without NVIDIA's Visual Studio integration.
- **A device cache's misses uploaded from the backend's own host memory** (`--expert-host-buffer
  DEVICE`). The device probe now also times an upload from a buffer of the device's own host buffer
  type (page-locked on CUDA, Vulkan and SYCL), and where it beats the file's mapping by the margin
  and the host holds a second copy of the experts, `--auto` copies the resident experts into that
  memory at load, byte for byte, and points the expert tensors at it. On a 24 GB card,
  Qwen3.8-Flash-Next 125B: uploads 9.9 → 20.6 GiB/s, decode 63.5 → 72.6 tok/s, with MTP 75.5 → 89.4;
  the load takes 36 s instead of 4.4 for the copy.
- **One backend per card, the faster measured** (`--device-list`). Where two backends drive the same
  card (CUDA and Vulkan on an NVIDIA board, HIP and Vulkan on an AMD one), each registers a device of
  its own and llama.cpp keeps the first registered. The plan now reads the PCI id each backend reports,
  compares the backends of one card on the same measured rate (the model's own expert block where the
  real-layer probe ran on them), and lists another one only where it is faster by the margin; the
  fitter places on that list and every rule after it sees one device per card. A device of unknown
  identity is never grouped with another, and a card with one backend plans exactly as before.
- **The cost of a device measured on a real layer** (Step 3 of `docs/measured-planning.md`). The
  planner's compute probe now builds one layer of the model from the file's own tensors and bytes
  (the expert block as `mul_mat_id` over the layer's expert matrices, top-k per token, and the
  block's other matrices as plain matmuls) and runs it on the host and on every device at decode
  width and at prefill width, the result compared with the host's first: a device that disagrees is
  refused. Four facts come out of it: each backend's rate on the expert block and on the dense
  matrices at one token (`expert_gibs`, `dense_gibs`, the host's `host_expert_gibs` /
  `host_dense_gibs`), and the whole layer at 128 tokens, which replaces the synthetic wide matmul
  behind `--prefill-device` (kept for a file whose layer cannot be read). The synthetic GEMV stays
  for the thread sweep and the figures that mean it. `--plan-explain` prints the new facts per
  device.
- **A crossing that is measured everywhere, and priced into the placement.** The crossing is now
  timed the way the graph crosses at a layer boundary - one token's hidden state copied to the
  device, one op run there, copied back, each leg waited for - instead of by differencing a chain of
  ops through the scheduler, which read 0 on CUDA and left every discrete card's crossing unpriced.
  With it, `--auto` prices the layers the capacity fitter split - dense part on a device, experts on
  the host, two crossings a token each - against the same layers whole on the host, on the device's
  measured dense rate and crossing, and enters the saving in the ranking against the expert cache
  (`device-layers`, `device-crossing`). Where it is funded those layers come home and the device
  keeps its whole layers at the top; where the device saves more than the hops cost, the fitter's
  split stands and the plan says by how much.
- **Device memory as a tier of the expert cache** (Step 4 of `docs/measured-planning.md`, closes
  #378, supersedes #381). `--device-cache-mb N` puts llama.cpp's device-side expert cache in front of
  the experts the host holds, resident or streamed: the experts used most are computed on the device
  from a cache in its own memory, and a miss is uploaded from the host. `--auto` arms it where a device
  has memory of its own, every layer's dense part is on it, and it computes the model's experts
  faster than the host: the experts used most then live next to the fastest processor, as RAM holds
  what flash would read, in place of the fitter's whole layers whose experts wait on the host every
  token. The cache fills from use and needs no record of the routing; a recorded routing says in the
  plan how much of a token it serves. The device's isolated probe also measures the rate at which
  host memory reaches it one expert at a time (`upload_gibs`), printed with the plan. On a
  24 GB discrete card with a 125B model resident on the host, a 16 GB device cache decoded at 61.8 and
  62.8 tok/s over 256 tokens and 84-88 once settled, against 31.9 for the best whole-layer split and
  18.9 with the experts on the cores. Speculation now works beside it. Gates G25a-c prove the cached
  runs byte-identical to the resident placement (resident, streamed, streamed under n-gram
  speculation), and G2u that an unstreamed run's record is the routing a stream asks for.
- **The cache is sized from the model's own routing** (Step 1 of `docs/measured-planning.md`). A
  streamed session keeps the experts each MoE layer asked the cache for during its first tokens
  (`--routing-record`, default 256 decoded tokens and the prompt's last 256; it adds no graph nodes
  and changes no output) and returns it as a value (`Session::routing_record()`, `bmoe/routing.h`).
  `bmoe-cli --routing-dir DIR` and `bmoe-server` (under `routing/` in its data folder) keep it, one
  file per model named by a content fingerprint of the file (`model_fingerprint()`). The next
  `--auto` plan of that file replays it through the engine's LRU at every size up to the whole
  expert set: the model's hit curve (`ModelProfile::hit_curve`). The cache is then ranked against
  the resident groups along that curve and credited the hit rate it gives at the chosen size,
  tagged `[measured on this model]` and with a `cache-hits` decision saying what it was replayed
  from. Without a record, or with one for another model or top-k, the plan is unchanged: the
  linear credit stays for a first load. The replay matches `scripts/route-replay.py` to the third
  decimal on the archived phone traces (`routing` test), and a new gate (`G2r`) checks that a real
  session's record replays to the hit rate its cache measured. The same record ranks the hottest
  (block, expert) pairs (`hottest_experts()`), for a later device-side cache to warm from; nothing
  reads it yet. The on-device runs this is judged by are owed.
- **The thread count and overlap are measured on the first answer** (Step 2 of
  `docs/measured-planning.md`; #294). Where the front-end keeps the verdict (`bmoe-cli --auto` with
  `--routing-dir`, and `bmoe-server` always), the plan arms a trial instead of a guess:
  `--trial-threads N[,M]`, the core-class boundaries nearest the incumbent, and `--trial-overlap`.
  Once the cache's hit rate has settled, decode windows (`--trial-window`, 8 tokens) alternate
  between the incumbent and each candidate, every candidate window between two of the incumbent's,
  on the user's own answer; a candidate wins only if it decodes `min_backend_win` (10%) faster in the
  median of its rounds. Both knobs switch with the session open: the thread count through
  `llama_set_n_threads`, overlap by starting or stopping the streamer's first read lane worker and
  the expert-ready hook between two decodes (it needs every layer on the host). The winner stays for
  the session and is returned (`Session::measured_knobs()`, `bmoe/measured_knobs.h`); the
  front-ends keep it beside the routing record, a line per machine (`hardware_fingerprint()`) and
  engine version, and the next plan of the model starts from it, states `threads` and `overlap` as
  measured on this model, and measures nothing. `--io-threads` is left to the storage probe, which
  already measures it on the model's own file. Gate `G24` holds the output byte-identical while the
  trial switches, within one answer and across turns; the policy has its own test (`knob_trial`).
  With nothing kept and the feature off, the plan is unchanged. The on-device runs this is judged by
  are owed.

### Changed
- **The plan's hit rate is set against the decode's.** The line `bmoe-cli --auto` prints after a run
  compares the credited hit rate with the one measured over the decode alone (the figure a token is
  priced with), with the prompt-inclusive one in brackets; it used to show the cache's share of the
  expert set against the cumulative rate. `bmoe-server`'s `/api/plan` `measured.cache_hit_pct` is
  likewise the last turn's decode now. `RunSummary::cache_hit_decode_pct` carries it;
  `BMOE_DONE`'s `cache_hit_pct` is unchanged.
- **The device tier and the cost model read the real layer's rates.** `choose_device_tier` prices the
  split's host layers at the host's rate on the model's own expert block, where host and device were
  both measured on it (the synthetic GEMV read a 48-core host at 77 GiB/s while its decode's experts
  ran at about 12), and the allocation prices the host's expert and dense compute each at its own
  rate. Every comparison of a host with a device takes both figures from the same experiment, the
  GEMV's where either side lacks the layer. The isolated device probe's facts line is `v=2`, with
  `expert=` and `dense=`.
- **Speculative decoding works beside a prefill device** (GPU, NPU or any device the prefill path
  takes). The pair was refused outright, so `--mtp` or `--ngram` sent the prompt back to the CPU:
  on a 16 GB laptop a 1307-token prompt of a 35B-A3B read at 68-95 tok/s with `--mtp` against 474
  without it. The refusal is now the rule it stood for: a verify batch (`--draft` + 1 positions) is
  a CPU graph and must be narrower than `--prefill-min-tokens`, so it never reuses a graph the
  device ran, and the planner declines a device only on that width. The MTP draft context, which
  runs on the CPU's backends, walks each chunk the device ran with the weights at home and the
  slots kept. Measured on that laptop with Metal: the same prompt at 340 tok/s with `--mtp`, and
  decode at 19.5-21.5 tok/s against 17.8-18.4 without it. Gate G16d holds a device prefill with
  n-gram speculation byte-identical to the CPU's run.

### Fixed
- **The real-layer probe sizes its graphs for their leafs as well as their nodes.** A graph's size
  bounds nodes and leafs together, and every matmul of the probe brings its weight, input and
  selection as leafs; sized for nodes alone, a block that needs many copies to outrun the last-level
  cache overflowed it (`GGML_ASSERT n_leafs < size`) and aborted `--auto`, and `bmoe-server` with
  it, on a 35B-A3B on a 16 GB laptop as on a 125B on a 24 GB card. The tiny models of the gates
  never needed enough copies to show it. The thread count is now chosen on the resident path too:
  a plan whose model fits returned before the thread rule, so the layers the fitter left on the host
  kept the default count (4 threads of a 32-core box, 26 tok/s where 16 threads decoded at 47.5).
- **A run reserves the logit rows its plan charged, with a prefill device or without one.** The
  context capped the rows a graph may return at 128 only when a device ran the prefill, while the
  planner projected every run at that cap. Without one, llama.cpp reserved a vocabulary-wide row
  for every token of the ubatch, where the output layer is. On a 24 GB card holding most of a 125B
  model's layers, `--auto` placed what the ledger allowed and then could not create the context:
  2.1 GiB of output rows no longer fit in the card's memory. On a CPU-only run of a 35B-A3B at 2048
  tokens the same rows were 2028 MiB of compute buffer against the 436 MiB planned (#304), 312 MiB
  with the cap. The cap now holds on every session, and a perplexity pass is fed in pieces no wider
  than it.
- **A container's memory limit is the plan's limit.** Inside a memory control group with a finite
  limit, the residency budget was the machine's `MemAvailable` (103 GB on a rented box whose group
  allowed 85 GiB), so a plan could size itself past the point where the kernel ends the process.
  The budget is now capped by what the group and every ancestor it can see still allow, less what
  each holds and crediting its inactive file pages, for cgroup v2 and v1. A finite limit is also
  read as a share the process is ended for exceeding, which widens the plan's margin the way an
  app's own share does on a phone.
- **The cores a process can keep busy, not the machine's.** The core count every thread rule and
  sweep starts from was `hardware_concurrency`, which counts every core of the machine. It is now
  the CPUs the process's affinity mask allows, capped by a CPU control group's quota (cgroup v2
  `cpu.max`, v1 `cpu.cfs_quota_us`, rounded up). A container given 30.7 CPUs of 128 swept its
  threads over 128 and probed its rates in bursts it could not sustain; the count is now 31. The
  core classes are read over the CPUs the mask allows.

## [0.36.0] - 2026-10-26

### Added
- **Context checkpoints, as llama.cpp's server keeps them** (`--ctx-checkpoints N`, default 8, where the server keeps 32: Qwen3.6-35B-A3B's is 62.8 MiB and comes
  out of the expert cache's memory;
  `--checkpoint-min-step N`, default 8192). A chat turn whose prompt diverges from the cached one
  inside a state that cannot be cut (a recurrent state, a sliding window) used to clear the context
  and prefill the whole conversation again. It now restores the newest checkpoint before the
  divergence: the part of the state that cannot be cut, copied at the end of every prompt through the
  public state API into `common`'s `common_prompt_checkpoint`. It matters most for coding agents,
  whose clients resend each answer as text: with Claude Code on Qwen3.6-35B-A3B, five requests of eight
  re-prefilled their whole 12k-token prompt. A memory that cuts anywhere keeps no checkpoint. Gate G22
  checks the restore is lossless on a recurrent model, and that a model that cuts anywhere is untouched.
- **A quantized KV cache, with llama.cpp's flags** (`--cache-type-k` / `-ctk`, `--cache-type-v` /
  `-ctv`: `f16`, `bf16`, `f32`, `q8_0`, `q5_1`, `q5_0`, `q4_1`, `q4_0`, `iq4_nl`). The types are
  llama.cpp's own, through `llama_context_params`, which rotates a quantized cache's heads first.
  `q8_0` halves the memory a long context takes and `q4_0` quarters it, and on this engine that memory
  goes to the expert cache. **`q8_0` is the default**, where llama.cpp keeps `f16`: an 18-turn agent
  chat to 60k tokens on Qwen3.6-35B-A3B (16 GB laptop) decoded in 281-303 s at `q8_0` against 442-490 s
  at `f16`. Every type but `f16` changes the output, and the default was set without a quality
  measurement; `-ctk f16 -ctv f16` is the lossless run. The plan never picks a type; the capacity
  fitter projects the KV at the type the run allocates. The metrics CSV records both types in its
  preamble. A quantized V cache needs flash attention: where llama.cpp's backend refuses it, V falls
  back to `f16` with a warning instead of failing the run. Gate G23 checks streaming stays
  byte-identical to resident with an `f16` cache; every other gate now runs at `q8_0`.
- **The plan charges a chat run's context checkpoints before it sizes the expert cache**
  (`checkpoint-budget`, a ledger row of its own). Each checkpoint copies the part of the context that does
  not grow with its length. The placement probe reads that part off llama.cpp's own projection at the
  context and at half of it, with nothing taken from the architecture: 62 MiB on Qwen3.6-35B-A3B against
  the 62.8 a checkpoint measures, and nothing for a context that is all attention. Without the charge, a
  chat filled up to 502 MiB of checkpoints out of memory the cache had already been given. A run that is
  not a chat makes no checkpoints and is charged none.
- **Desktop packages that open with a double-click** (part of #385). `release-host` builds one per
  OS from the same staged folder the `bmoe-desktop` archive carries, and the archives stay for
  scripted installs and the CLI. The scripts live under `packaging/`.
  - **macOS:** `BigMoeOnEdge-<tag>-macos-arm64.dmg` holds `BigMoeOnEdge.app` and a link to
    Applications to drag it onto. The app's executable is a small launcher
    (`packaging/macos/launcher.swift`). It starts `bmoe-server` from inside the bundle, with the web UI
    and the catalog from its Resources, and opens the page. While the server runs, the app sits in
    the Dock and the menu bar with Open in Browser, Show Log and Stop and Quit. Quitting it, from
    any menu or with SIGTERM, stops the server. If a server already answers on the port, it only
    opens that server's page. The bundle is ad-hoc signed, not notarised: the dmg's README says how
    to open it the first time.
  - **Windows:** `BigMoeOnEdge-<tag>-windows-x86_64-setup.exe` and `-windows-arm64-setup.exe`, a
    per-user Inno Setup installer that needs no administrator rights. It installs under
    `%LOCALAPPDATA%\Programs\BigMoeOnEdge` and adds a Start-menu entry that starts
    `bmoe-server.exe`, which opens the browser; closing its console window stops it. The uninstaller
    leaves the data folder (settings and models) alone. The installer is not signed yet. The
    `windows-arm64-npu` build keeps its archives only, since its NPU setup is done by hand in the
    unpacked folder.
  - **Linux:** `BigMoeOnEdge-<tag>-linux-x86_64.AppImage` and `-linux-aarch64.AppImage`, built with
    a pinned appimagetool and runtime (both checked by SHA-256). They start `bmoe-server --open`.
    Started from a file manager, the AppImage reopens itself in a terminal window so the server can
    be seen and stopped.
  - The workflow's dry run (a PR that edits it or `packaging/`) builds every package. Each is then
    started on its runner with no model, where the API must answer and the web UI must be served
    from inside the package: the app from the mounted dmg, the AppImage, and the installed copy
    after a silent install, which is then uninstalled.
- **Android updates through Obtainium.** `README.md` and `examples/android/README.md` link an
  [Obtainium](https://github.com/ImranR98/Obtainium) entry that follows this repository's releases
  and picks `app-dev-release.apk`.
- **The README quick start leads with the packages,** one line per platform; the archives and the
  CLI follow.
- **Claude Code and Codex CLI can use the server.** `bmoe-server` also speaks the Anthropic
  Messages API (`POST /v1/messages`, `POST /v1/messages/count_tokens`) and the OpenAI Responses API
  (`POST /v1/responses`). Each is a translation layer onto the same turn as
  `/v1/chat/completions`: same line, context checks, cancel-on-disconnect and key (now also
  accepted as `X-Api-Key`). Tools, tool results, thinking/reasoning, the budget and structured
  output map across; errors come in each protocol's shape, a full context as "prompt is too long"
  / `context_length_exceeded` so an agent compacts. Streams are append-only (no `reset`): text is
  sent only as far as the chat parser's view extends what was sent. Mid-conversation system
  messages become user messages, which a template accepts and which keeps the KV reusable, and
  Claude Code's per-request billing-header block is dropped for the same reason. Images, server
  tools, `previous_response_id` and stored items are refused with `400`; Responses built-in tools
  are dropped. `count_tokens` is an estimate from the request's length. The README says how to
  point each agent at the server; `docs/server-api.md` has the reference.
### Changed
- **`--dense-weights pinned` is the name of the reclaim-exempt mode** that was `ahwb`. The mode
  runs on Android (a dma-buf) and on macOS (wired memory), and the name only described the first.
  `ahwb` still parses, on the command line and in saved settings, and is written back as `pinned`;
  the choice the UIs list, the plan, the logs and the telemetry (`dense <...>` in the perf block,
  `dense_weights=` in the CSV header) say `pinned`. The mode's spelling now lives in one function
  (`dense_weights_mode_name`) instead of four copies.

## [0.35.1] - 2026-10-24

### Changed
- **The context warning is for a server with clients, and can be dismissed.**
  `model.ctx_advice` gains `refused` (a request was refused with `context_full` since the load) and
  `warn`: the context is short and the server is bound beyond loopback (`--host`, the apps' "Serve
  on the network") or has refused a request. The web UI and both apps show the warning only when
  `warn` is true, so a chat on the device itself is no longer told about clients it does not have,
  while an agent on the same machine that gets refused is still noticed. The text says what
  happened or what will, a ✕ (44 pt / 48 dp on a phone) dismisses it for that model and context, and
  a first refusal brings it back. The apps also keep the line under "Serve on the network". The
  load-time log line and the chat's `context_full` banner are unchanged.

## [0.35.0] - 2026-10-24

### Added
- **Request-level structured output.** `POST /v1/chat/completions` accepts
  `response_format` with `json_object` or `json_schema`, and llama.cpp's top-level
  `json_schema` field. The schema travels through llama.cpp's chat template, grammar and
  parser, using the session's existing grammar sampler. Thinking, tools and speculative
  decoding use that same path; there is no separate JSON sampler. The constraint belongs
  to one request. A normally completed JSON answer is checked for parseable JSON;
  length-limited and cancelled turns retain their real finish reason and may be partial.
  A completed answer that fails validation returns `invalid_output` without unloading the
  model, so the next request can proceed. See `docs/server-api.md` for schema support and
  streaming semantics and the tested template/tool-choice limitations.
- **The Hexagon NPU on Windows on Snapdragon, as an opt-in archive.** The release gains
  `bmoe-cli` and `bmoe-desktop` archives for `windows-arm64-npu`: the Windows ARM64 build plus the
  Hexagon module and one DSP library per NPU generation (v73 to v85). Windows' NPU driver runs that
  code only from a catalog signed by a certificate the machine trusts, so the release workflow
  makes the catalog with Inf2Cat (from Microsoft's WDK package on NuGet), signs it with a
  certificate made in the same job, and deletes the private key before the job ends: nothing else
  can ever be signed with it, and only the public certificate ships. Using it needs a one-time setup
  the user does by hand (the NPU driver, test-signing on, the certificate trusted), which
  `docs/npu-windows.md` walks through with what each step changes and how to undo it. The plain
  Windows ARM64 archives are unchanged and need none of it. As on Android, the NPU takes the prompt
  only where the planner measures it faster and identical.
- **The server warns when the context is too small for its clients, before a request fails
  (#362).** `bmoe-server` loads with a 4096-token context, and opencode's first request alone (its
  system prompt and tool schemas) is 9911 tokens, so every request of a coding agent was refused
  with `context_full` and opencode showed an unknown error; nothing had said so beforehand. The
  engine now names what a server's clients need, `RunConfig::ctx_for_clients` (16384 tokens, with
  its measured reason), and `GET /api/info` carries `model.ctx_advice` (`recommended`, `short`, and
  the parameter that raises it). A short context logs a `warn` line at load with the flag to raise
  it, and each `context_full` refusal logs one too, so the person at the server learns why even
  when the client hides the error. The web UI shows the warning beside the loaded model with a link
  that lands on the Context setting (settings links are now `#/settings/<key>`), and a full context
  in the chat says the context's size and links the same setting. Nothing in the UI names the
  number or the parameter. The RAM a larger context costs is said, not priced: it depends on the
  model's attention layout and only a placement fit at that context measures it; the plan's
  `kv-budget` note still says what the loaded context takes. The planner's own context choice is
  unchanged; the Android and iOS apps show the warning too (below). Docs: `docs/server-api.md`
  (sizing the context for a client), `docs/screens.md`, the Android and iOS READMEs.
- **Desktop Models screen: one list, a guided flow, downloads in the status line (#355).** The web
  UI's Models screen had five blocks, a model in the catalog and on disk shown twice, and five
  generic buttons spread over three of them. It is now one list, "Your models", where local files and
  catalog entries are merged by file. Each row has one state and one primary action: Available →
  Download, Downloading → progress and Cancel, Partly downloaded → Resume or Delete partial, On this
  machine → Load, Loaded → Reload, Unload and Go to Chat. Catalog entries with nothing here sit at
  the bottom as suggestions. With nothing on disk the screen is a three-step card (find, check,
  download then load) with the Hugging Face field inline and the catalog's entries as one-click
  downloads, and a first visit opens on Models instead of an empty Chat. The Hugging Face card is an
  explicit Find → Pick → Check → Download stepper. Check leads with the verdict as a headline, and
  Add plus Download are one button. The new row scrolls into view, carries the progress and turns to
  Load when the download lands. A running download shows in the status line on every screen (count
  and share done). The models folder path moved behind a "where models live" disclosure.
- **The fit verdict as a code and a line from the server.** `POST /api/hf/inspect` now also returns
  `verdict.fit` (`fits`, `no_streaming`, `will_not_fit`, `no_disk`, `not_a_model`, `unknown`) and
  `verdict.headline`, both decided by `fit_verdict` from the facts it already had. The UI shows the
  line and colours it by the code, with no threshold of its own. A model that streams on a machine
  whose free memory could not be read is `unknown`, not `fits`.
- **Delete a model and import from a link, on the desktop (#356).** These two were in
  `docs/screens.md` and on the phone, but the desktop UI and the server had neither.
  - `DELETE /api/models/local/<name>` deletes a model named the way the listing names it, never by
    path. It removes the files the server's own scan of the models folder groups under that name, so
    a split set goes as a unit, and its emptied folder with it. It never touches a scan directory or
    the model that is loaded or loading (`409 in_use`), nor a file that a running download is writing.
    A link is removed as the link. A set goes whole or not at all: each file is renamed aside first,
    and one that cannot be (held open on Windows) puts the others back and deletes nothing. Only a
    loopback client may call it.
  - `POST /api/models/url` turns a direct https gguf link into a catalog entry (`url:<file>`, size
    from a `HEAD` request), so its download is the catalog's, with resume, cancel and progress. A
    link to any shard names the whole split set. The link must be https with no credentials, and the
    file name must use the same narrow alphabet as a repository's. The status is read from the
    headers, because curl's exit code over HTTP/2 does not report a refused `HEAD`. A link may name
    at most 128 shards (`kMaxUrlShards`; published sets are under 50), and the size requests stop
    after 120 s in all (`504`, no entry made), so a typed `-of-99999` cannot hold a worker for hours.
    Loopback clients only.
  - The Models screen gains Delete (asked twice), Forget for an added entry, and a direct-link field.
    A download that does not start after Download on the Hugging Face card or a link says why there,
    instead of the card reading "Downloading"; a suggestion's failed download shows beside it.
  - New pure policy files with server checks: `url_entry` (which links are taken, the entry they
    become, the HEAD answer) and `model_delete` (which files a delete removes, path escapes refused,
    the loaded model recognised by identity). Both were also run end to end against a scratch models
    folder: a 404 link refused, a real link added, downloaded, refused for delete while loaded, then
    deleted.
- **The Android app's screens catch up with the web UI's.** The four screens `docs/screens.md`
  describes for 0.35.0, on the phone, all of it read from the server and the parameter table:
  - *Status line on every screen.* It moved out of Chat to sit above the navigation bar on all
    five screens, and carries the running downloads ("Downloading 42%", "2 downloads 42%"; a split
    set is one download), which lead to Models, and marks a model loaded without streaming.
  - *Context warning.* A loaded model whose context is below `model.ctx_advice.recommended` gets a
    line under the status line: the context, the recommendation, that a larger context
    takes memory from the expert cache, and a link labelled from the parameter table that lands on
    the setting `ctx_advice.param` names. A `context_full` error offers the same link beside New chat.
  - *Models: one list.* Local files and catalog entries merged by file (only a file at the top of
    the models directory, where downloads land, claims an entry; an entry's own state is the
    server's `status` with the app's transfers over it, as the web UI reads it), one state and one action
    per row (Available, Downloading, Partly downloaded, On this device, Loaded with Reload, Unload
    and Go to Chat, Shards missing), suggestions at the bottom, companion files apart, the folders
    behind "Where models live". Delete asks first and goes through the server's
    `DELETE /api/models/local/<name>` in its models directory (the app deletes what it may write
    elsewhere); Forget takes an added entry off the list. With nothing on the phone the screen is
    the three steps, and the app opens on it. The Hugging Face card is the Find, Pick, Check,
    Download stepper with the server's verdict headline coloured by `verdict.fit`, one Download
    button that adds and starts the download, its refusal shown on the card, and the new row
    scrolled into view. A direct link goes through `POST /api/models/url` and the app downloads the
    entry's files, so it gets Resume and one progress bar over a split set. The server's size
    lookup no longer refuses a link when it has no `curl` to start (the phone's case): the size is
    left unknown, as for a host that answers no `HEAD`, and the file counts whole once present (a
    desktop server without `curl` now makes the entry and fails at download rather than at add; a
    new POSIX server check runs the lookup with no `curl` on `PATH`). Nothing checks the link
    before the download then, so the download worker ends at once on a definite `4xx` from the
    host (408 and 429 still retry) and the row says why, instead of backing off five times; a link
    the server refuses shows its reason, and the app no longer fetches one file of it on its own.
    The file picker stays.
  - *Plan.* The summary with the lanes in one line and the four figures (decode expected and
    measured with the difference, cache hits credited and measured, memory held against the host
    pool, the worst case against the expected), Apply and Measure again together, Apply listing the
    settings it changed as links, Measure again beside an error, decisions linking to their row, and
    Copy diagnostic in place of the planner's account on screen. With nothing loaded and no plan
    to show, the decisions the configuration carries from the last applied plan, grouped the same
    way. Whether a model is chosen is read from the table's read-only group, not a named key.
  - *Settings.* One card per group (`group_order`, `group_label`, `group_help`, `group_read_only`)
    with its levels folded inside, a level filter with counts remembered on the phone, a search over
    every level, one source chip per row with Use plan value, Reset all my values (less
    `launch_keys` and the read-only group, after asking), "needs reload" only from `reload_keys`,
    and deep links that open the card and fold and bring the row into view.
  The rules are pure Kotlin with JVM tests (`data/Library.kt`, `engine/SettingsForm.kt`,
  `engine/PlanView.kt`, `engine/ContextAdvice.kt`); no screen names an engine parameter. The
  SHA-256 check of a catalog download was already the worker's (#347).
- **`THIRD_PARTY_LICENSES` (#352).** Every archive and APK carries llama.cpp and ggml, the server
  carries cpp-httplib and nlohmann/json, the desktop archive's web UI bundles Svelte, clsx, marked
  and DOMPurify, and the UI and the app use two fonts under the SIL OFL; only the project's own
  Apache-2.0 `LICENSE` travelled with them. The new file holds those licenses' text,
  ships beside `LICENSE` in every release archive (CLI, desktop, iOS harness) and as an asset in the
  APK, and the README's License section points at it, in all six languages.
- **Dependabot for the workflows' actions, the web UI's npm packages and the Android Gradle build
  (#351).** Monthly, one grouped PR per ecosystem.
- **A warning level, a named sanitizer option and an install target (#350).** The project's own
  targets build with `-Wall -Wextra` (`/W4` on MSVC); llama.cpp and its vendored libraries keep
  upstream's flags. `-DBMOE_WARNINGS_AS_ERRORS=ON` makes a warning an error (off by default; on in
  CI's macOS job, the compiler the tree was cleaned on). `-DBMOE_SANITIZE=address,undefined` (or
  `thread`) instruments the whole build in one flag; CI's sanitizer job uses it, and a new TSan job
  runs the threaded tests (the engine host, the server's turn queue, the byte-identity gates).
  `cmake --install build --component bmoe` installs the release layout: `bmoe-cli`, `bmoe-server`,
  the backend libraries beside them, the web UI and the model catalog. The release archives are
  still packaged by hand, since they also carry the C runtime on Windows and a README per archive.

### Changed
- **Thinking is off by default (#331).** A reasoning model spends most of a turn thinking before it
  answers: on a Mac, Qwen3.6-35B-A3B used all 1024 tokens of the default budget on a short question
  and never answered, which at 12-17 tok/s is a minute or more of waiting, and several on a phone.
  `RunConfig::think` now defaults to off, so the CLI, the server, the web UI and both apps start
  with it off; a saved setting still wins. It stays one switch away: `--think` on the CLI (beside
  `--no-think`), the Thinking setting, and per request through `think`,
  `chat_template_kwargs.enable_thinking` or `reasoning_effort` (any level but `"none"` now turns
  it on; before, only `"none"` was read). Models with no thinking mode are unaffected.
- **Route-ahead's int8 gates are chosen by a timing, not by the instruction set (#349).** The
  prediction behind `--route-ahead` ranks experts with one router-gate GEMV per layer per token,
  over the gate's float weights or over an int8 mirror of them (a quarter of the bytes, plus a
  quantization of the activation per call). Which is faster is bytes against arithmetic: the mirror
  won three times over on the test phone and lost on an x86 host. It was armed by an
  `#if defined(__aarch64__)`, right on those two machines and a guess on any other. New setting
  `--gate-mirror auto|on|off` (default `auto`): `--auto` times both kernels on the model's own gate
  set (every gated layer, so the timing sits in the cache regime the run will, bounded at 512 MiB),
  the run's own code walked layer by layer, and takes the faster with the timings in the
  plan's notes; without `--auto` the session runs the same timing at load. The probe runs only when
  route-ahead is set, so other plans do not pay for it. Measured on a development laptop: 5.7 us
  int8 against 42.8 us float per layer on a 2048 x 128 gate. G14b now commits on both gate formats;
  route-ahead stays lossy either way, and nothing it does not touch changes.
- **The web UI's Plan screen shows the verdict and four figures, and its decisions land on their
  setting (#357).** The planner's full explanation is no longer a fold on the screen: every line
  of it already had a worded twin there, and it was the one place the UI showed raw parameter
  names. It stays in the API, and Copy diagnostic puts it on the clipboard with the plan's command
  for a bug report, beside a link to how the planner decides. Under the verdict sit the expected
  decode speed, the cache hit rate the plan credited, the memory held against what the process
  may hold, and the worst case with nothing held in memory; Apply and Measure again sit together
  under them (Measure again also beside an error, so a failed plan can be retried), and Apply lists
  the rows it changed, each linked to its setting. A decision's
  "in Settings" link now lands on that row instead of the top of the page. `predicted` in
  `GET /api/plan` gains `s_per_token_worst`, and its `cache_hit_pct` is now the hit rate the cost
  model credited (a new `Allocation::cache_hit_credited`), not the cache's share of the experts,
  which assumed uniform routing (part of #310).
- **The web UI's Settings screen is one card per group, each with its own levels folded inside
  it (#358).** The parameter table gives each group an explicit order (`group_order`), a short
  `group_label` and a one-line `group_help`, marks the model file's group read-only for a form
  (`group_read_only`: it is chosen on the Models screen), and moves the prompt's device and reads
  out of Expert streaming into a group of their own, Prefill. The page renders that: a card per
  group with its basic rows open and its advanced, experimental and debug rows folded inside it by
  level, instead of one Advanced wall; a level filter (remembered per device) and a search over
  every level; one source chip per row (default, from the plan, set by you) in place of up to six
  badges; "per message" only on per-request rows and "needs reload" only on a row whose value
  differs from the loaded session's (`GET /api/config` gains `reload_keys`); "Use plan value" where
  the plan decided a different value than the person's; and "Reset all my values", which leaves the
  server's launch flags (`GET /api/config` gains `launch_keys`) and the model a load chose, so a
  reset never leaves a configuration without its model. `bmoe-cli --help` prints each group's help
  line beside its title. The Android and iOS apps sort their settings groups by `group_order` too
  (falling back to table order for a server that does not send it), so Prefill sits after Expert
  streaming there as well. Docs: `docs/screens.md`, `docs/server-api.md`, `docs/settings.md`.
- **The iOS app's screens match the web UI's of this release.** The same `docs/screens.md`, in
  SwiftUI, with the logic in BmoeKit under `swift test` and nothing in Swift that names an engine
  parameter:
  - **Models is one list.** The files the server lists and the catalog's entries are merged by file
    (`Library.rows`), each row in one state with one primary action (Download; progress and Cancel;
    Resume or Delete partial; Load; Reload, Unload and Go to Chat), Delete asked twice and sent to
    the server (`DELETE /api/models/local/<name>`, so a split set goes whole or not at all), Forget
    for an added entry, companion files apart, suggestions at the bottom and the folders behind
    "Where models live". With nothing on the device the screen is the three steps, with the Hugging
    Face card inline, the suggestions as one-tap downloads and a link field, and a first launch with
    nothing on the device opens on Models. The Hugging Face card is Find, Pick, Check, Download,
    the verdict's headline coloured by `verdict.fit`, and the new row scrolls into view and carries
    the progress. A direct https link becomes an entry through `POST /api/models/url`, and its
    files download in the app like any other entry's; the bridge now answers the server's `HEAD`
    for a link's size on `URLSession`, so those files have a size to be whole at, and a file whose
    host gives none counts as whole once it is there. An entry is on the device when the server
    says so (`status`), not because a file of the same name sits in a folder or a scan directory,
    which is that file's own row. A running download shows in the status line on every screen.
  - **The context warning.** Beside the loaded model's name when `ctx_advice.short`: the context,
    the recommendation, that a larger context takes memory from the expert cache, and a button
    named from the parameter table that lands on that setting. A chat's `context_full` error says
    the context's size and offers the same button beside New chat.
  - **Plan opens on its verdict.** The sentence, the machine, the lanes in one line and four
    figures (decode expected and measured with the difference, the cache hit rate credited and
    measured, memory held against the host pool, the worst case in ms per token), Apply and
    Measure again together, Apply listing what it changed with each row a link to its setting,
    Measure again beside an error, each decision linking to its own row, and Copy diagnostic in
    place of the planner's account shown in full. Whether there is a model to plan for is read
    from the read-only group of the parameter table or the loaded model, never by the key.
  - **Settings is one card per group** (`group_order`, `group_label`, `group_help`; the read-only
    model group shown without a control), its deeper rows folded inside by level with what each
    level means, a level filter with counts remembered on the device, a search over every level,
    one chip per row (default, from the plan, set by you), "Use plan value" or "Let the plan set
    it", "per message", "needs reload" from `reload_keys` with a Reload banner, and "Reset all my
    values" (less `launch_keys` and the read-only group, after a confirmation). A link from Plan,
    the status line or Apply widens the filter, opens the card and fold, scrolls to the row and
    focuses its field, for that visit only. A level this app does not know yet is folded with
    the deepest rows.

  Checked with `swift test` and a simulator build.
- **CI pins what it downloads (#351).** The gates' Python packages come from
  `scripts/requirements-ci.txt` (`gguf==0.19.0`, `numpy==2.5.3` and gguf's own dependencies, every
  one `==`), bumped on purpose instead of whatever was newest that day. The Windows Vulkan SDK
  installers are checked against the SHA-256 LunarG publishes for them before they run, and the
  Android Gradle wrapper verifies its distribution (`distributionSha256Sum`). `release-host` builds
  with `contents: read`, pull requests included; a new `publish` job, which builds nothing and never
  runs on a pull request, is the only one with `contents: write`: it attaches the archives the build
  jobs left as workflow artifacts, under the same names as before.

### Fixed
- **A cancelled turn leaves both contexts at the same prefix.** When the draft context of
  speculative decoding could not drop the cancelled tokens, only it was cleared, so the next turn
  started the target and the draft at different positions. Both are now cleared together.
- **`bmoe-server`: no local client can take every worker (#353).** The server answers with a fixed
  pool of 32 workers and each `GET /api/events` stream holds one for as long as it is open, with no
  bound on how many: 32 idle streams from any local process and every other request, the UI's own
  included, queued forever. The event streams are now capped at 8 (the next is a `503` with the new
  code `streams_full`, which the UI and both apps already retry with back-off), and the pool is
  sized from what can hold a worker for long (8 streams, the running turn, the 16 that may wait
  behind it) plus 7 for short requests, still 32 in all. A stream's place is taken before it
  answers and given back however the response ends, the client leaving before the headers
  included. Connections that arrive while every worker is busy now wait in a queue of 64, not an
  unbounded one; past it the next is closed at once. A stream now notices a client that closed
  its connection within a second instead of at its next 15 s ping, so a reloaded page finds the
  place it left free; a client that keeps the connection but stops reading is still let go by the
  ping hitting the write timeout. A client that vanishes without closing (a phone off Wi-Fi) is now
  bounded by TCP: keep-alive probes after 15 s idle and a 60 s limit on unacknowledged data, each
  option set where the system's headers define it. They are set on the listening socket, the one
  place the HTTP library offers: Linux and Android clone them into every connection; macOS was
  measured not to copy the idle time or the retransmission bound, so there that case is still left
  to the system's own limit.
- **`--open` starts the browser without a shell (#353).** It went through `std::system` with the
  URL quoted into a command line; it now starts `open` (macOS) or `xdg-open` directly with the URL as
  one argument, through the subprocess header the downloads already use, and drops its output on a
  thread that also reaps it. Windows already used the shell's own call.
- **The printed URL is right for an IPv6 bind (#353).** `--host ::` printed `http://:::8765/`;
  it now prints `http://[::1]:8765/`, and any IPv6 literal is bracketed.
A non-object JSON body (`[]`, `1`) on `PUT /api/config` or `POST /api/models/download`, the second
point of #353, is already a `400` with `code: "invalid_request"` since 0.31.0 (#262): checked,
not changed.
- **Repository hygiene (#352).** `docs/android-memory.md` no longer links the test phone vendor's
  kernel tree or names its modules; stale comments that described the Android app starting the CLI
  (it starts `bmoe-server`) are corrected, in the app, its README and the iOS harness; and the
  Android app drops its API-level checks for versions below its `minSdk` of 29.
- **A data race in route-ahead's int8 gate mirror (#350).** On aarch64 (the phones, Apple silicon)
  the prediction worker read layer `nl`'s gate matrix from the hook's per-layer table while the
  eval thread rewrote that table on every decode step. The value was the same weight leaf each
  time, so no run went wrong, but it was undefined behaviour; the first ThreadSanitizer run found
  it. The worker now uses the gate pointer the job already snapshotted, as the float path did.
  The warnings `-Wall -Wextra` raised (an unused parameter and helper, aggregate fields without a
  default, discarded results, a range loop that copied) are fixed without changing what any of them
  computes.

### Security
- **The model catalog is held to what its fields become (#347).** A catalog file's `name` is joined
  onto the models directory and its `url` handed to curl, and every front-end took them as written:
  a row named `../x.gguf` would have been written outside the models directory. An entry whose file
  name is not a bare file name (no separator, no `.` or `..`, no drive colon), whose URL is not
  https (plain http only under an http `--hf-endpoint` mirror), or whose digest is malformed is now
  skipped as `bmoe-server` loads the shipped catalog and the added entries (the reason goes to the
  start-up log) and refused when added; the Android and iOS apps drop such an entry too, and the
  Android download worker refuses a name that is not plain.
- **Downloads are checked against a SHA-256 (#347).** A catalog file may carry `sha256`; the two
  reference models now do (their Hugging Face LFS object ids), and a model added from a repository
  gets its own from the listing. The server reads a finished `.part` back and checks it before the
  rename (the row says so while it reads; a cancel stops it), and the Android worker does the same:
  a mismatch is an error naming both digests and deletes the `.part`, since resuming it could only
  reproduce the same bytes. Before, only the byte count was checked, so a swapped or corrupted file
  became a load failure later. The Android app no longer renames a leftover `.part` it finds under
  a succeeded download: only the worker, after its checks, makes a file a model.
- **curl is pinned to https (#347).** Every curl the server runs (downloads and repository reads)
  now passes `--proto =https --proto-redir =https` for an https URL, so no redirect downgrades it
  (only a plain-http mirror named with `--hf-endpoint` may use http), and `--` before the URL, so
  no URL is read as an option. New server checks: the name and URL rules, the digest of known
  vectors and of a file longer than one read, the LFS oid on an added entry; Android unit tests for
  the name rule and the digest field, and a BmoeKit test for the name rule.
- **The iOS app checks its downloads against the SHA-256 too.** A finished transfer moves to a
  name of its own check (`<name>.part-<id>`), so a second copy of the same file finishing meanwhile
  can never replace the bytes being hashed and only the file that was hashed is renamed into place.
  It is read back in 4 MiB chunks with CryptoKit, off the transfers' queue so the other shards keep
  coming, and becomes the model only when its digest matches the catalog's; a mismatch deletes it
  and says so, naming both digests. The row reads "Checking" meanwhile; a cancel stops the check
  and keeps the file as `<name>.part`, and a whole copy left by an app that ended during its check
  (of a known size, or of any size for a link whose host said none) is checked on the next start
  instead of fetched again. A start leaves a file already in flight to its transfer (asked of the
  system's own task list, so a relaunch cannot double one), and an entry stays Downloading while
  any of its files still comes, even after another failed; the failure is said once nothing runs.
  A start is refused before a byte moves when what the entry still needs does not fit in the space
  the device has free, worded as the server's 507. The app now also drops a catalog entry whose
  URL is not https or whose digest is malformed, beside the name rule it already held, and a task
  tagged by an earlier build (no digest) still resumes. BmoeKit tests: the digest of the standard
  vectors and of a file read in chunks that divide nothing evenly, a cancel, a matching file
  landed, a mismatch deleted with the model that was there kept, a check's own copy landed while
  another sits at the partial name, a cut-off check found again, the space refusal, the catalog
  rules.

## [0.34.1] - 2026-10-23

### Fixed
- **A client that leaves no longer cancels another client's answer (#346).** On `bmoe-server` a
  streaming request that failed to write cancelled whatever the engine was running, and the same
  write was the keep-alive sent while a request waits its turn and the last frames after its turn
  ended. So with two clients on one server, an agent and the web UI, closing a tab that was waiting
  ended the other client's answer with `finish_reason: cancelled`; reproduced on 0.34.0 with a real
  35B model (the answering client stopped after 173 of 700 tokens). The host now stops a turn only
  from inside that turn's own token callback, when its client is gone; a client that leaves while
  waiting just gives its place up. Same repro on the fix: the answering client gets all 700 tokens,
  and a client that leaves mid-answer still stops its own turn at once. Two new engine-host checks.
- **A read past the end of a model file is an error, not weights (#348).** The streamer's file
  reader clamped a range that ran past the end of the file and returned success: the aligned path
  copied the whole request out of a bounce whose tail was never read, the plain path came back
  short, and every caller checked only for failure. A truncated download or a damaged shard would
  have streamed garbage experts instead of failing the load. llama.cpp validates tensor bounds on
  the same files first, so no model was known to hit it; the reader no longer depends on that. New
  `file_reader` test (buffered and direct, at the end of the file and past it); the byte-identity
  gates pass unchanged.
- **The phone apps' "first token" is this turn's (#359).** Both apps added the model's load time to
  every turn's prefill, so a second turn after a 56 s load read as 62.7 s to its first token. They
  now show the server's `ttft_s`, as the web UI does.
- **iOS: an import that fails keeps the model it would have replaced (#359).** Importing a file
  with the name of a model already on the phone deleted that model before copying, so a copy that
  failed (no space, the picked file gone) left neither. The copy now lands under a hidden name and
  replaces the old file only once whole; picking a file from the models folder itself copies nothing.
- **iOS: a small file no longer reads as 0.0 GB (#359).** Sizes under a gigabyte are written in MB
  or KB.

### Docs
- **No architecture table left in the words either.** The engine reads which tensors are the routed
  experts from the file's shapes (`expert_layout`) and has had no architecture registry since 0.31,
  but a few places still described one: the model probe's header said a table decides which tensors
  are experts, `docs/limitations.md` spoke of "registry rows", the Android metrics glossary said the
  architecture "selects the streaming recipe", and a planner explanation, a CMake comment and three
  code comments said "recipe". They now say what the code does.

## [0.34.0] - 2026-10-22

### Added
- **Windows on ARM.** The release carries `bmoe-cli` and `bmoe-desktop` archives for Windows ARM64,
  built natively on an ARM64 runner and smoke-tested there, for Snapdragon laptops. The recipe is
  llama.cpp's own for this target: clang with its toolchain file, one CPU library tuned for those
  cores (ggml has no instruction-set tiers for Windows on ARM, so the top-level CMakeLists builds
  the one), the Vulkan module, and the OpenCL module with the Adreno kernels. The OpenCL module uses
  the loader the GPU driver installs; none is shipped, and without one it is simply not loaded. As
  everywhere, the planner measures what registered: the prompt goes to the GPU only if the probe
  finds it faster than the cores and its result identical. The Hexagon NPU is not in this archive:
  on Windows its driver loads only a DSP library signed by a trusted certificate, which a user would
  have to set up by hand (test-signing). Not yet run on a Snapdragon laptop.

## [0.33.3] - 2026-10-22

### Changed
- **The README in five more languages.** Chinese, Japanese, Spanish, French and Italian
  (`README_zh.md`, `README_ja.md`, `README_es.md`, `README_fr.md`, `README_it.md`), linked from a
  language line under the badges. Each says it is a translation: the English README is the reference.
- **llama.cpp to upstream master of 2026-10-22.** The fork branch `bmoe/expert-ready-hook-2210-dl`
  is upstream master `d88040c` plus the same single expert-ready-hook commit, rebased without a
  conflict; 95 upstream commits past the 2026-10-18 base. No engine change was needed. All 32 host
  tests pass, the overlap gates (G4, G20b) included, and on a real 35B model `--auto` prints the
  same greedy text as before at the same CPU time per token.

### Fixed
- **A tool call is held to the template's grammar, and a turn the parser cannot read is no longer
  empty (#334).** In long agent conversations a model sometimes wrote a call the chat parser could
  not read back. The server then answered 200 with an empty assistant message and `finish_reason`
  `stop`, although tokens had been generated: 12 times in 72 tasks × 3 seeds on one report, against
  none on llama.cpp with the same model and prompts. The difference was sampling. The template
  returns a grammar with its tools, and llama.cpp's server holds sampling to it once a call opens;
  the engine ignored it. It now applies it through the public `llama_sampler` grammar API, lazily:
  free text pays one check of the chosen token, and only a token the grammar refuses masks the
  vocabulary and is chosen again. A reasoning span stays free, as llama.cpp's reasoning-budget
  sampler keeps it, so a call's opening written while the model thinks does not start one. Under
  speculative decoding a draft the grammar refuses ends the accepted group. Greedy output is
  unchanged until a call opens, and a turn without tools runs exactly the old path. A finished turn
  the parser still reads as nothing comes back as the raw generation, with a warning, instead of an
  empty message.

### Tests
- **The tool-call grammar, without a model.** `chat_messages` builds the grammar the vendored Qwen3.5
  template returns over llama.cpp's vendored vocab file and walks token sequences through it: free
  text and a well-formed call pass, a call to a tool the request did not offer is refused, and with
  thinking on the same bad call inside the reasoning span passes while after the span it is refused.
  The reasoning half fails with the suspension switched off.
- **Gate G20: routing on a device with memory of its own, experts streamed (#321, #335).** The
  placement llama.cpp's capacity fitter writes for a MoE model larger than the device: every layer's
  dense part on the device, the routed experts on the host. The device is the loopback RPC one every
  host has, whose buffers are not host memory, so the router's ids take the host copy #335 added.
  The gate checks that streamed output matches the same placement with resident experts, serial (a)
  and under `--overlap` (b), and that experts were actually read. It tells the code apart: before
  #335 the overlap half hung on 6 runs out of 6, and with it it passed 21 out of 21.

## [0.33.2] - 2026-10-21

### Changed
- **The README reads like reference documentation.** At the top, real-time GIFs of the shipped apps
  generating with the same 35B on an Android phone, an iPhone and a laptop, in place of the animated
  demo and its video. The three dark illustrated diagrams become plain figures with one
  accent colour and measured numbers: one decode step, what `--auto` chose on one laptop, and the
  35B's decode speed on four devices; each in a light and a dark variant, all from
  `scripts/readme-diagrams.py`. The text loses its slogans, and the platform table says where each
  platform has run.
- **The README shows the first run on a real iPhone.** The Plan and Chat screens of the iOS app with
  a 35B on a 12 GB iPhone, a link to its real-time recording, and a row in the examples table
  (2.6-3.0 tok/s), all from @gjjkbssg's run (#235). `examples/ios/README.md` says the app has run on
  a device, and `docs/benchmarks.md` gains the iPhone figure.

### Fixed
- **A layer on a discrete GPU whose experts stream no longer crashes the first prefill (#321,
  likely #330).** The router hook read the selected expert ids straight from the routing node,
  and on a layer placed on a device with memory of its own that node lives in device memory: the
  read faulted. `--auto` reaches exactly that placement whenever a model is larger than the cards
  (DeepSeek V4 Flash on two 16 GB cards, under CUDA), and `--gpu-layers` with `--moe-stream` did
  on Vulkan. The ids are now copied to the host with `ggml_backend_tensor_get` where the node's
  buffer is not host memory, which the scheduler makes valid by synchronizing the split before the
  callback; the policies that rewrite the routing or read the router's inputs (substitution,
  route-ahead, dropping, the predictors) act only on host-resident layers, so a device layer keeps
  the router's own choice. Unified memory reads as host, which is why no Apple-silicon run showed
  it. Reproduced and verified on Apple silicon with Metal's private buffers
  (`GGML_METAL_SHARED_BUFFERS_DISABLE=1`, which makes its memory unreadable from the CPU like a
  discrete card's): 0.33.0 segfaulted at the same point as the report; with the fix 12 placed
  layers plus streamed experts print the same text as the resident run, with and without
  `--overlap`.

### Docs
- **Measured planning, the design (`docs/measured-planning.md`).** Where `--auto` goes next. The
  memory ledger and the correctness checks stay rules; speed is measured on the model itself
  instead of predicted, in four steps:
  1. The cache sized from a replay of the model's own routing trace, replacing the linear hit
     credit.
  2. The lossless knobs (threads first) settled by interleaved windows on the first real request,
     and kept per hardware and model.
  3. The cost of a device measured on one real layer's matmuls and crossing, the price that has
     kept layers off devices in decode.
  4. Device memory as a tier of the expert cache, through llama.cpp's experimental
     `moe_cache_size`. It needs a correctness check with streamed experts before the plan arms it.
     A device that fails its probe should be left out of the fitter rather than cancel every
     device's placement.

  Each step names what it removes, the earlier measurements that constrain it (the retired cache
  governor, a hit-rate curve that lost on throughput, a thermally contaminated A/B) and how it is
  judged. `docs/hardware-planning.md` points to the new route beside the closed one. Linked from
  the roadmap and the docs index.

## [0.33.1] - 2026-10-20

### Changed
- **The benchmark protocol is `--auto` alone.** `scripts/bench-report.sh` runs the model once with
  `--auto --plan-explain` and reports that row with the plan it chose; the fixed streaming run
  (`stream`, forced `--moe-stream`, CPU only, hand-picked threads, lanes, cache and ubatch) is
  gone, with the `MODES`, `THREADS`, `IO_THREADS`, `CACHE_MB` and `UBATCH` overrides that shaped
  it. A fixed configuration measured the streamer rather than the machine, and on a desktop with
  GPUs it reported a CPU-only number next to one almost thirty times faster (#321). The adb command for
  phones, the app instructions (Auto-plan on), the issue form and `docs/community-benchmarks.md`
  follow; the maintainer rows say they predate the change.

## [0.33.0] - 2026-10-20

### Added
- **The iOS app, on the shared framework.** `examples/ios/app` is the five screens (chat, models,
  plan, settings, metrics, `docs/screens.md`) in SwiftUI, in the pixel language of
  `docs/design.md`, light and dark. An app on iOS may not start a process, so it links
  `bmoe_server` and runs `run()` on a thread of its own, then speaks the same HTTP API on loopback
  as the web UI and the Android app, with a random key per launch; Serve on the network restarts
  it on all addresses with a key kept in the keychain. The app keeps the conversations, downloads
  the catalog's files with the system's background transfers (resumed after a break or a
  relaunch), imports a `.gguf` from Files, adds a model from Hugging Face through the server's
  find, check and add, and writes a metrics CSV per loaded model. Its client side is
  `examples/ios/BmoeKit`, a Swift package with no UI, tested with `swift test` on the Mac (36
  tests: the event stream, the chat protocol, decide, the parameter table, models, the plan,
  conversations, metrics files, downloads); CI runs them and compiles both apps for the
  simulator. Built and run in the simulator only: a device run is still owed (#235).
- **The iOS app's Plan screen, as the web UI and the Android app draw it.** Opening it measures
  nothing (`GET /api/plan?cached=1`, "Not measured yet" with Measure when the server holds no
  plan); the summary first (the sentence, the machine, decode speed expected against measured with
  the difference, cache hits with a mark at what the plan credited, the lanes, Apply, auto-plan),
  then one memory bar per pool, the devices with their prompt rate against the fastest, the
  decisions grouped (set open, left as is and notes folded, each row opening to its brief, Why and
  Open in Settings) and the command line with the full explanation folded. The `plan_progress`
  stages show while a measurement runs, and Models shows the seconds since a load began and,
  during an auto load, the same stages. BmoeKit reads the whole plan and the progress event.
- **The iOS app's Settings and Models match the web UI and the Android app.** Settings renders the
  parameter table's new `typical` and `special` fields: the name of a special value under a number
  ("= off"), the usual range ("usually 2 to 8") instead of the type's limit, which now appears only
  beside a refused value, the help's first sentence with the rest on demand, and an Advanced
  settings section that no longer repeats the basic rows; with no Wi-Fi or wired network the
  address line says no other device can reach the engine. A stopped catalog download (a failure,
  a cancel, a relaunch) reads **Resume (X of Y GB)** from what landed and what the system's resume
  data holds, with **Delete partial download** beside it, which throws away the resume data, the
  bytes the system kept for it and, through `DELETE /api/models/partial/<id>`, any `.part` of the
  entry; Cancel keeps them. Files whose header `role` says they are not models (projectors, draft
  heads) are listed apart with what they are and no Load, a model in a folder says which, and the
  Hugging Face listing folds the files whose names suggest a projector or a draft head after the
  models, shows when a repository last changed and marks a header that is not a model.
- **`server::Embedding`, for a server inside an app** (`server/server.h`). `run()` takes the
  network a repository is read through (the iOS app hands it `URLSession`, where the desktop runs
  `curl`) and hands back the way to stop the server, so a change of address restarts it in the
  same process. Without one, the server does what the executable does. The server library now
  builds for iOS: opening a browser is a no-op there.
- **Process memory accounting beside system availability.** Per-token CSVs append
  `process_footprint_mib` (Apple's dirty-memory ledger charge) and `process_headroom_mib` (the
  current allowance remaining for a physical iOS app), with `-1` for unknown and measured zeros
  preserved. Info-level snapshots through the existing log sink expose memory across automatic
  probe stages, model loading and session setup. Apple compressed-memory accounting is documented
  separately from disk swap and RSS; these observations do not change budgets or planning policy.
- **A device prefill where nothing can be locked.** An unavailable wire-limit query was read as a
  lock store of 0, and the planner charged an accelerator's buffers to it anyway, so the device
  never fit: a 12 GB phone's GPU, measured at 17.8x its cores on a wide prefill, was refused "on
  memory" (#304). The lock ceiling exists to keep what the process locks from being outbid, and
  where nothing can be locked there is nothing to outbid: the device is now charged there only where
  a lock store exists, and to what the process may hold everywhere. Machines that publish one, the
  Mac and the Android phone among them, plan exactly as before (checked side by side on the Mac).
- **The expert cache brakes when the system takes it back mid-run** (`--cache-brake`, on by
  default). On a 12 GB phone the default `--auto` budget ended in a Jetsam `proc-thrashing` kill
  (#304): the system compressed 2.5 GB of the engine's own memory and a token took 52 s. No figure
  a platform reports says how much a process can keep resident, so the run now watches for it: a
  token whose compute stalls at twice this machine's own median, confirmed by a sample of the
  cache's pages that are no longer in RAM, halves the cache. It never grows back within the run
  and never goes under one token's cycle; a slow token with the pages in place moves nothing. Same
  procedure on every platform (`mincore`, or `QueryWorkingSetEx` on Windows). On a 16 GB desktop
  with a 6000 MiB unpinned cache it took decode from 5.99 to 7.49 tok/s in ordinary use and from
  4.77 to 5.97 beside a process holding 9 GB; under `--auto`, whose cache is pinned there, it never
  fired. `docs/pressure.md` has the design and why it is not the retired governor.
- **`scripts/ios-runs.sh` sweeps the expert cache.** A `c<MiB>` run (`RUNS="c1000 c1500 c2000"`)
  is the plan's run with only the cache fixed at that size, so one command shows where a larger
  cache stops paying and where the device starts taking memory back (`examples/ios/README.md`).
  Its `summary.txt` also carries the prefill line, the plan's `prefill-device` and any
  `cache brake:` line.

### Changed
- **One readable face for the whole UI: Oxanium, on the desktop and in the Android app.** The pixel
  face was set on buttons, badges, the navigation and every figure, and at those sizes it was hard
  to read; it now marks only the wordmark beside the logo. Oxanium (SIL OFL 1.1, bundled) keeps the
  squared look and takes every other word, the model's answers included; figures keep tabular
  digits. Sizes are unchanged.
- `examples/ios` holds the app and the harness side by side: the harness moved to
  `examples/ios/harness`, the memory entitlements to `examples/ios/memory.entitlements`, shared by
  both. `scripts/build-ios.sh` builds both (`BMOE_IOS_TARGETS` picks one; `BMOE_IOS_APP_BUNDLE_ID`
  names the app on a device).
- **`scripts/bench-report.sh` measures the machine as well as the streamer (#321).** It used to run
  one configuration only, streaming forced on with CPU-only settings fixed for the reference phone,
  so a desktop with the model inside its RAM, or GPUs beside it, reported the cost of reads a
  resident model never makes and never touched its GPUs: a 17 GiB model on a 60 GB, two-GPU
  workstation read 4.3 tok/s. It now runs `--auto` first (the planner's own placement: devices,
  resident when the model fits, streaming only the residue) and then the fixed streaming protocol,
  and prints one row each with the settings read back from each run's CSV, plus the plan `auto`
  chose. `MODES=stream` reproduces the old single run. The community-benchmark page, the method
  page and the issue template describe the two rows, and how a CUDA build joins the `auto` one.

### Fixed
- **A model larger than VRAM beside GPUs loads where the fitter placed it (#321).** DeepSeek-V4-Flash
  on two 16 GB cards tried a 17 GB allocation on one of them. Two causes, both in how the first
  stage's placement reached the load:
  - The planner armed a prefill device over layers the fitter had already placed, since a prefill
    is compute-bound and the GPU won it. A prefill device loads as the model's only device, so every
    placed layer landed on that one card. A placement is now a conflict the prefill rule declines
    with the reason, and `validate()` refuses the combination when set by hand.
  - The fitter's per-device layer split was dropped, so llama.cpp divided the layers by free memory
    rather than as the fitter had sized each card's share. It is now carried as `--device-split`
    (device names, in the fitter's order, each with its count), applied with the same device list,
    printed in the plan's reproduce line, and checked against `--gpu-layers`.
- **Layers on a device plus `--overlap` no longer corrupt a long prompt.** In a wide graph llama.cpp
  offers the host-weight matmuls to the device, which copies a streamed expert before its read has
  landed: with 12 layers on a Metal GPU and a 548-token prompt the answer was blank lines. Under
  overlap with layers placed, the session turns that offload off; serial streaming keeps it, and
  both now match the resident run byte for byte. `bmoe-server` runs with overlap on by default, so
  this was the app's path on a desktop with a GPU.
- **`--auto` no longer aborts on a 1- or 2-bit IQ quant (#321).** The bandwidth probe encodes a
  synthetic matrix in the model's expert type, and ggml asserts when IQ2_XXS, IQ2_XS or IQ1_S are
  encoded without importance weights: DeepSeek-V4-Flash UD-IQ2_M died before loading on Linux. The
  probe now passes a uniform importance where ggml says one is required. Which expert type the
  probe used was also the first expert tensor met while walking an unordered map, so on a mixed
  quant (that file: IQ2_XXS gate and up, IQ3_XXS down) the format probed, and whether the run
  aborted, depended on hash order rather than on the file; it is now the type carrying the most
  expert bytes, and the device-support question is
  asked of every expert type the file carries. `bandwidth_probe_test` covers every format ggml
  names as needing importance weights.
- **A layer placed on a device keeps its dense weights there.** The dense policy rebound every
  non-expert tensor onto host memory, including those of layers the first stage had put on a GPU,
  so a run with layers on a device and the rest streamed printed garbage (Metal: "buffer is nil",
  repeated `!`). It now re-homes only tensors whose buffer names no device other than the CPU; the
  same run is byte-identical to its resident counterpart. This is the placement `--auto` gives a
  model larger than VRAM beside a GPU, and no machine the planner had run on had reached it.
- **Decode and prefill time include the device's work.** On a GPU backend `llama_decode` returns
  once the graph is queued, so the wait landed in `loop_overhead_ms` and tok/s overstated a GPU run:
  a two-GPU CUDA run read 1777 tok/s for 241, and a Metal run with 12 layers on the GPU 16.4 for 13.6.
  Both timers now close after `llama_synchronize`; a CPU run is unchanged.
- **The bandwidth probe encodes one row and copies it.** Every row of its matrix was already the
  same row, and encoding 16384 of them in an IQ grid took 13-16 s of a laptop core before every
  `--auto` run. `--plan` on an IQ3_XXS model: 26.2 s to 14.5 s, the same figures measured.
- **`bench-report.sh` measures the drive on the largest shard.** A split model's first shard can be
  metadata alone (5-10 MB), so the 1 GiB read a quarter into it read nothing and timed the open:
  #321's 960 MiB/s, 2113 MiB/s on a laptop whose drive reads 6400.
- **The Android app's CPU temperature is a sensor's, not a threshold's (#311).** It took the first
  thermal zone whose type names the CPU, in the order the directory lists them, and on a
  Snapdragon-class phone that was a trip threshold published as a zone: the metrics line read a
  constant 95 °C while the cores read 47-55. Zones are now taken in numeric order, one reading its
  own `trip_point_*_temp` is skipped, and the figure is the hottest zone that moves (the median
  until one has). JVM tests over a fake `/sys/class/thermal` cover it.
- **The benchmark-report issue form parses again.** Its engine-version placeholder was two quoted
  strings on one line, which is not YAML, so GitHub could not render the form.
- **iOS run batches report launch failures to automation.** `scripts/ios-runs.sh` still finishes
  the requested cases, partial-CSV retrieval and summaries, but exits `1` if any launch command
  failed. Each case retains its exact exit status; all-success and plan-only batches exit `0`.
  Device-free fixtures cover both simulator and device command paths in CI.
- **A layer's experts are never split between a device and the host** (#304). On a 12 GB phone
  `--auto` stopped before generating with `captured MoE layer is missing expert tensor
  'ffn_gate_exps'`: the planner had read the phone's GPU as having memory of its own, the capacity
  fitter put 11 layers on it whole and split the next (`blk.11.ffn_down` on the host, its `gate` and
  `up` on the device), and the streamer, which loads a layer's experts as a set, found half of it.
  - *A pattern that moves part of a layer's FFN now brings all of that layer's experts with it.*
    Any fitter's partial layer can take that shape, on any device. The experts are then streamed,
    and the device holds less than the fitter projected. A split given by hand
    (`--tensor-overrides`) is refused by name, with the pattern that keeps the layer whole, instead
    of the missing tensor it used to trip on. Checked on a Mac with the phone's own placement
    applied by hand: refused as given, and decoding the same text as a CPU-only run once the layer
    is kept whole.
  - *The GPU's memory is read from the device, not inferred from an allocation.* The Apple probe
    adapter reads `hasUnifiedMemory` from Metal's default device and applies it to the ggml device
    whose description matches it; a known shared pool cannot then be turned into separate memory by
    a flat allocation delta (allocator reuse or reclaim can hide the allocation there). Contributed
    by @gjjkbssg. Where no native fact exists, the allocation probe now counts what is charged to
    the process (resident plus compressed or swapped), not what is resident, so a kernel making
    room by compressing our own pages no longer reads as separate memory.
  - *A dense weight a device owns stays on the device.* With layers placed on a device,
    `--dense-weights anon` moved their dense weights into host memory as well, where the device
    cannot read them: Metal reported a nil buffer and the run decoded `!`. Only weights in host
    buffers are converted now, and the ones left on a device keep `--release-mmap` from dropping a
    mapping the device may still read.
- **The prefill-device confirmation no longer costs 2 GB at load.** It asked the host for all 64
  rows of logits, and from 32 rows up ggml's BLAS backend unpacks the output head, the model's
  largest matrix, into float32 and keeps it: 1940 MiB on a 248k vocabulary, outside every budget,
  which a 12 GB phone's process limit ended the run on (#304, found by @gjjkbssg). The device still
  computes all 64 positions; 31 spread across them are compared. On a 16 GB Mac the peak footprint
  of an `--auto` load fell from 7.58 to 5.66 GiB, with the same 97% agreement. BLAS stays on: it
  takes a CPU prefill of a 1208-token prompt from 66.7 to 92.6 tok/s there. On the phone, with BLAS
  on, the default `--auto` now completes every case of the device runner, a 1208-token prompt
  prefilled on its GPU included, with the startup checkpoints peaking near 5.0 GiB (validated by
  @gjjkbssg).
- **An unreadable lock ceiling no longer disables cache locking.** Denied platform queries retain
  their uncertainty and every independent finite bound, including zero. An automatic plan can
  request cache locks and fit to actual runtime grants without increasing its ordinary memory
  budget or reducing its margin. The plan and UI label that request as unverified, rather than
  reporting a protected cache or a measured zero allowance. Explicit caller choices remain authoritative.
- **The lock-limit intersection compiles with Windows headers.** Its minimum operation uses an
  explicit template argument so the Windows `min` macro cannot expand the function call.
- **Startup pin-fit uses the model's token-cycle floor before any streamed graph runs.** The
  effective routing width is resolved before initialization, so a partial lock refusal during the
  full prefill-device confirmation can shrink the cache instead of silently abandoning its locks.
  A refusal below one cycle retains the ordinary-memory fallback. Real session regressions cover
  the model default, a routing override, and the fallback; restoring the old ordering fails them.

## [0.32.0] - 2026-10-19

### Added
- **The runs a device report needs, in one command: `scripts/ios-runs.sh`.** It runs the plan, a
  run of it and the same run with the dense set left mapped, each in a fresh process of the iOS
  harness, and brings back the console output and per-token CSV of each with a `summary.txt`
  (tok/s, the decode split, major faults, dense residency); `t2`/`t4` add the run at 2 and 4
  threads. `device` drives a signed harness through `devicectl`, `sim` the booted simulator.
- **The iOS memory entitlements, one at a time.** `BMOE_IOS_MEMORY_ENTITLEMENTS=ON` now asks for the
  increased memory limit alone, which a free Personal Team is granted and which was enough for a
  streamed 35B on a 12 GB phone (#235); `BMOE_IOS_EXTENDED_ADDRESSING=ON` adds the extended
  address space, which a Personal Team is refused and which made the build unsignable for one.

### Changed
- **The README, rewritten around what the engine is: an adaptive streaming optimizer for big local
  AI agents on any device.** It opens with a silent 30-second loop (an animated WebP, so it shows
  inline everywhere, the GitHub app included) linking to the full promo video recorded on a 16 GB
  laptop and a 12 GB phone (real time, sped-up parts marked), then the four things it does (adaptive streaming, any
  hardware and OS, any model and quantization, a local OpenAI-compatible server), ten measured runs
  presented as examples rather than limits, getting started on the phone, the desktop and the
  command line, an OpenAI client example, and three diagrams drawn for it: how one token runs, how
  the planner adapts to the machine, and where the memory goes. The settings tables, the
  architecture table and the long benchmark tables leave the README for the pages that already
  hold them (`docs/settings.md`, `docs/which-models-stream.md`, `docs/benchmarks.md`), and the
  three GIFs and the older demo video go. The diagrams are SVG from `scripts/readme-diagrams.py`,
  with their own dark card so they read on either GitHub theme. Under the planner diagram, a
  one-minute real-time GIF from the phone: a 35B answering, then the Plan screen it runs.
- **Settings say what a value means and what is usual, and Advanced holds only the rest (#268).**
  The parameter table gains, per row, a typical range (named constants in `core/src/params.cpp`,
  every default inside it or one of the row's named values, which `bmoe_params_test` checks) and
  special values with a name (`0` = off, `0` = as wide as the context, `4294967295` = random per
  run); `GET /api/params` returns them as `typical` and `special`. Rows whose only upper limit was
  the type's (the cache sizes, the row-stream window, the prefill device width, the seed) no
  longer report it as `max`. Units on the rows that lacked one (lanes, threads, layers, reads,
  candidates, experts). Labels a person can read: "Wrap in chat turns" (was "Chat template", a
  switch), "Start reads in two waves", "Unmap the file after load", "Read a busy layer whole",
  "Route experts early". The web UI and the app render all of it from the same data: the special
  value's name and the typical range under each field instead of "0 to 2147483647", the help's
  first sentence with the rest on demand. The Advanced section no longer repeats the basic rows shown above it, and its title is
  in sentence case.
- **The Plan screen leads with the outcome, and reads the same on every machine.** The web UI and
  the Android app open the plan with what it decided in one sentence ("Experts stream from flash,
  with a 4.8 GiB cache") and three tiles: decode speed expected and, once the loaded model has
  answered, measured beside it with the difference; the cache hit rate measured against what the
  plan credited; which device computes the prompt and which the tokens. Then the memory as one bar
  per pool (every ledger row, what is free, what is locked), every device with its rate against
  the host, what it does in this run and why, and the decisions in three groups: the settings the
  plan decided (under their settings labels), the ones it left, and the notes that are no
  setting (regime, placement, ledger, costs), folded. A measurement shows the stage that is
  running instead of a blind wait. One layout for a phone with an NPU, a laptop with an
  integrated GPU, a discrete card or a machine with only cores: every block is a list of what the
  plan holds, device names are the plan's own, and nothing in either UI names a device or a
  system (#288). On a laptop: 0.307 s/token predicted, 0.212 measured on the first answer;
  hits credited 28%, measured 64%.
- **`GET /api/plan` describes the plan for a person.** New fields beside the old ones:
  `summary` (the sentence and the lanes), `predicted` (decode as the plan priced it), `measured`
  (the loaded model's last complete turn, on a plan from the last load; a cancelled turn is no measurement), `memory.pools` (the ledger, a
  pool per memory that is separate), `devices` (rates, `uses`, `status`, `why`) and
  `devices_none`; each decision carries its settings `label` and its `group`. Every word the
  screen shows comes with them (`label`, `badge`, `rate_text`, `devices_none_text`), beside a
  code a UI reads only for a colour, so the words live once, in `server/plan_view.cpp`, pure and
  unit-tested on an NPU phone, a discrete card and a machine with only cores, and no UI keeps a
  table of its own. A measurement now publishes `plan_progress` events, one per stage with its
  words, from a progress callback on `probe_and_plan()` (`AutoPlanOptions::on_step`).
- **Every plan decision says itself in one line, and each cost note has its own name.** A
  `Decision` carries a `brief` beside its `reason`: one plain sentence for a person, at most 120
  characters, with the number that decided it and no flag names ("Prompts run on MTL0, 8.1x faster
  than the CPU, same results"), which the Plan screen leads with while the reason stays the full
  account `--plan` prints. The cost model's notes, which all came out as `cost = -` and could not
  be told apart, are now structured (`AllocationNote`: a topic, a value, a brief, a detail) and each
  is its own decision under its topic (`storage-rate = 6.3 GB/s`, `cache-floor = 581 MiB`,
  `shared-memory`, ...); `/api/plan` adds `brief` and gives every note a `label` to be read under.
  The reasons stop citing one machine's story on every other (#290). Each decision also comes with
  `value_text`, its value as a person reads it ("4.6 GiB", "pinned in memory", "experts stream"),
  and every size on the screen is in GiB or MiB, the briefs included. The dense-weights choices
  read "mapped from the file", "mapped and read ahead", "copied into memory" and "pinned in
  memory" in Settings too. `docs/server-api.md` and `docs/screens.md` describe both.
- **The hardware probe tells "this build carries no accelerator backend" from "it carries one and
  found nothing".** `HardwareProfile::backends_placeable` counts the backends that could offer a
  local device, read from what each offers: a remote one (RPC) and a library the host calls into
  (BLAS) do not count, so a build with only those no longer reads as a machine without devices.
- **The dense set is held in memory on every platform, and what a reclaim costs is measured, not
  named.** The planner left the dense weights mapped wherever exceeding the memory limit ends the
  process, because clean file pages are not charged to that limit. They are not charged because the
  kernel may drop them, and every decoded token reads the whole dense set: on a 12 GB iOS phone the
  35B-A3B decoded at 1.2 tok/s with 25581 major faults a token (about 400 MiB read back from flash
  per token, the cores 75% idle), where the same model held in memory makes 5 on the Android test
  phone (#304). That branch is gone: a fatal limit widens the margin the ledger sizes against, and
  the dense set is held (`anon`, or pinned where a reclaim-exempt store exists) like anywhere else.
  The fact behind it was a constant per OS; it is now read from the kernel in microseconds
  (`pio::reclaim_facts`: a per-process share left, compressions since boot, a swap), so iOS, macOS
  and Windows answer from their own accounting as Linux and Android already did, and
  `HardwareProfile::file_pages_counted` is removed. The Apple-only "device buffers are locked"
  constant goes too: unknown is charged the same way. Plans on macOS, Linux, Android and Windows
  with a pagefile are unchanged; `docs/hardware-planning.md` describes the facts.

### Fixed
- **Android: an app denied the network says so, and where to give it back** (#298). Android ties
  every IP socket to the network permission, loopback included, so on GrapheneOS with the app's
  Network toggle off, or behind a firewall app, the app cannot reach its own engine even offline.
  The status line said "Engine not reachable: socket failed: EPERM"; it now says the app has no
  network access, which the engine needs even offline, with a button that opens the app's system
  settings, and polls slowly until it is back. A download denied the same way fails at once with
  that reason, instead of retrying, and points to importing the file.
- **The tag build no longer publishes the release.** Its APK job attached the debug APK with an
  action that creates the release when none exists yet, as the workflow's own token; a release made
  that way fires no `release: published`, so `release-apk` and `release-host` never ran and 0.31.0
  went out with one debug APK and no archives (#284). The step is gone, and with it the job's write
  permission: `release-apk` already uploads the debug APK beside the signed one. The agent guide
  says who publishes a release.
- **App: turning on Serve on the network no longer unloads the model or scrolls the page away.**
  The restart it needs (the server binds another address) unloaded the model and, while the engine
  was down, replaced the settings with one line, so the page jumped to the top. The rows now stay in place with a line
  saying the engine is restarting, and the model that was loaded is loaded again once the new
  server is up. The same holds for New key.
- **App: the address to connect to is shown, not described.** With Serve on the network on, the
  page shows `http://<ip>:8765/v1` with the phone's own IPv4 on its Wi-Fi or Ethernet network (a
  mobile network's private address is left out: no laptop on the Wi-Fi reaches it), with Copy, and
  follows it when the network changes; with no such network it says no other device can reach the
  engine. The app asks for `ACCESS_NETWORK_STATE`, a permission without a prompt.
- **Small things from the 0.31.0 smoke tests (#290).** The chat list ellipsizes a long title before
  its Delete link instead of running under it. Loading a model shows the seconds since it began
  and, while the plan measures, the same stages the Plan screen shows, in the web UI and the app,
  instead of a blind wait of half a minute or more. The macOS archives' README says how to open a
  program a browser download quarantined (`xattr -dr com.apple.quarantine`, or right-click and
  Open) until a signed installer ships (#234).
- **A stopped model download says it can be resumed, and resumes the same way on the phone and on
  the desktop.** The bytes always landed in `<name>.part` and the next attempt continued them, but
  little on screen said so, and the two front-ends disagreed on what Cancel meant (#293). Now a
  catalog row with a partial file reads **Resume (X of Y GB)** after a failure, a cancel or a
  restart, in the web UI and the app alike, and **Delete partial download** beside it is the one
  action that throws the bytes away: Cancel on the phone keeps them, as it already did on the
  desktop. `GET /api/models` gains `received` and `partial` per catalog entry, so a server
  restarted mid-download still shows the entry as resumable, and `DELETE /api/models/partial/<id>`
  deletes an entry's `.part` files, by catalog id and inside the models directory only. The desktop
  server now retries a dropped connection on its own (curl's exit 18 or 56 and the other line
  failures `--retry` leaves alone), resuming from the `.part`, up to six attempts with a wait
  that doubles from 2 s to 60 s, and refuses a download the disk cannot take before a byte moves
  (`507`), counting only the bytes still to fetch. On the phone, a resume of a single file is no
  longer refused for the whole model's size, a failed space check no longer deletes the partial
  file, and the download notification follows the progress instead of sitting at 0%.
  `docs/server-api.md` and `examples/android/README.md` describe the resume behaviour.
- **Checking a model on the Models page while another is loaded no longer reads every MoE as "will
  decline" (#285).** `POST /api/hf/inspect` set the candidate against what the machine had free
  with the loaded model still resident, although loading the new one unloads it first: measured on
  a 15 GB laptop, 450 MiB with a 35B loaded against 9323 MiB once it was unloaded. The verdict now
  gives back what the loaded session holds, by its own account (the memory ledger of the plan it
  was loaded with), no more than the machine has, and says so: "(8873 MiB of it held now by ...,
  which loading this model unloads first)". A model loaded without a plan has no account, so the
  figure stays the measured one and the sentence says it was measured with that model loaded. No
  second measurement is made. `machine` gains `budget_if_loaded`, `loaded_model`, `loaded_held`.
- **Projectors and draft heads are no longer offered as models (#286).** What a file is for is now
  read from its gguf header, never its name: a projector (`general.type` `mmproj`, or the `clip`
  architecture), a draft head (it names another model's `<arch>.target_layers`, or all of its
  blocks are prediction heads), or no language model at all (another `general.type`, no
  `<arch>.block_count`). `POST /api/hf/inspect` answers such a file with mode `not a model`, a
  sentence saying what it is, and `downloadable` false, where it said "plain mmap" and offered an
  800 MB projector for download. `GET /api/models` gives each local file a `role` from its header;
  `POST /api/hf/models`, which reads no header, gives one from the name, says it is only a hint,
  and uses it to list the models first. The Models screen of the web UI and of the Android app set
  companion files apart in both lists: after the models, labelled, with no Load.
- **A Hugging Face search now carries when each repository changed, and whether it is gated
  (#290).** The site's search leaves `lastModified` and `gated` out unless they are asked for, so
  `modified` was always empty and `gated` always false; the server now asks for every field it
  reads (`expand[]`, percent-encoded like the query), and both UIs show the date.
- **`--scan-dir` finds a split model kept in a folder of its own (#290).** The models folder and
  each scan directory are read one level deep: their own files, then those of each folder directly
  inside. No deeper, and a link to a folder is not followed, so a scan cannot loop or leave the
  directory; what a network client may open is still only what the confinement to those folders
  allows, tested through the new subfolders. `--help` and `docs/server-api.md` say so, and each
  listed model gains `folder`.
- **`docs/server-api.md` describes `POST /api/hf/add` as it behaves (#287).** Adding a model that
  was already added is a `201` that replaces the entry; `409` is for an id of the shipped catalog,
  and for a split set with shards missing.
- **llama.cpp's debug log no longer buries the engine's output.** llama.cpp and ggml wrote every
  line they log to stderr, DEBUG included: about 1200 lines of graph-input bookkeeping per model
  load, ~9700 for a CLI run with `--auto` (one load per probe child) and 12k in a server log after
  three loads. A log bridge (`core/src/engine/llama_log.cpp`, through the public `llama_log_get` /
  `llama_log_set`) is installed before the first backend loads, in the CLI, the server, every
  probe child and any session opened without the planner. It forwards everything else unchanged to
  the callback it replaced and drops DEBUG, together with the continuation lines of a dropped
  message, unless `--llama-debug` (the `llama-debug` parameter) asks for them (#287).
- **A cancel is no longer logged as a decode failure.** Stopping a generation aborts the graph
  through the engine's own abort callback, which llama.cpp reports as three errors
  (`graph_compute: ... failed with error 1`, `process_ubatch: failed to compute graph`,
  `llama_decode: failed to decode`). While an abort the engine requested for a cancel is in flight,
  until the cancelled `generate()` or `decide()` returns, llama.cpp's ERROR lines are demoted to
  DEBUG, and the engine says `cancelled: the decode in progress was stopped on request` instead. An
  abort for a fatal streaming error opens no such window, so a real failure still reads as one
  (#287).
- **No `UnmapViewOfFile failed` warning on Windows with `--release-mmap`.** The released view's
  range was held by a plain reservation, so llama.cpp's own unmap of it at teardown failed and
  warned on every released run, the default `--auto` path included. The range is now held by a view
  of an empty, uncommitted pagefile section, which llama.cpp's unmap releases successfully, as its
  `munmap` already did on POSIX; a plain reservation remains the fallback (#287).
- **The planner no longer lists BLAS as a device.** `plan: device BLAS: one token 0 GiB/s` appeared
  on every macOS plan. A device ggml registers as an accelerator (BLAS, AMX, ...) is a library the
  host's graphs call into, never a place for a layer, so the device probe leaves it out of the
  profile by its device type rather than its name; GPU, integrated GPU and NPU devices (Hexagon
  registers as a GPU) are kept. A new `device_probe` test checks this against the build's real
  registry (#290).
- **The dense residency sensor reads the same on every platform.** Under `mmap`/`warm` it found the
  mapping through the process's region list, which an iOS app is not given and Windows does not
  have, so `dense_resident_frac` was `-1` exactly where the dense set was being refaulted. It now
  reads each tensor's own address in llama.cpp's mapping, and the same lookup serves dropping the
  mapped copies after an `anon` read and the random-access advice on held-back tables. On Windows
  the sample is `QueryWorkingSetEx` and the CPU time is `GetProcessTimes`, so the `compute:` line
  is printed there; with no per-process count of hard faults published, it says so rather than
  print a 0, and the summary's `majflt_tok` is `-1`.

## [0.31.0] - 2026-10-18

### Added
- **iOS rides with every release: the harness app for the simulator, as a release asset.** The
  `release-host` workflow gains an `ios-harness` job that builds `examples/ios` for the simulator,
  unsigned, and attaches `bmoe-ios-harness-sim-<tag>.zip` (the app, a README with the simulator
  commands, the licence) to the release, or keeps it as an artifact on a dry run. The one build of
  this engine for the platform anyone can run without an Apple team; a device build stays signed by
  whoever has the device (#235). `docs/platforms.md` and the harness README say so.
- **A model's name is enough: the Hugging Face field searches.** Text that is not a repository id
  or URL (`POST /api/hf/models` answers `400` with the code `not_a_repo`) is searched for on the
  site (`POST /api/hf/search`: repositories with gguf files whose name matches, the most
  downloaded first, a gated one marked), and the repositories found are offered to pick from, in
  the web UI and in the app alike. The query is percent-encoded by the server and goes to the one
  endpoint the server was started with, as every other request does.
- **A model from any Hugging Face repository, with its header read before the download.** The
  Models screen of the web UI and of the Android app takes a repository id or URL and the server
  does three things in turn (`POST /api/hf/models`, `/inspect`, `/add`, `docs/server-api.md`):
  lists the gguf files the repository holds (a split set as one model), reads one model's header
  through a range request and parses it with the planner's own model probe, and says what it is
  on this machine: architecture, experts and top-k, what stays resident and what streams, whether
  this engine streams it or runs it as plain mmap, how much disk it needs against what is free and
  the resident floor against what this process may hold. Added, the model is a catalog entry like
  the others (`"added": true`, kept between runs, removable; its files stay) and its download is
  the catalog's: the server's `curl` on the desktop, the app's own downloader on the phone. The
  server fetches only from its endpoint (`--hf-endpoint`, the site or a mirror) with a repository
  id and paths it validated, never a URL a client wrote; a token for a gated repository is used for
  the requests through a file only this process reads and kept nowhere. The rules (what a person
  typed, what a listing holds, the entry a model becomes, the verdict from the numbers) are one
  pure module, `server/hf_repo`, with tests; the network is a port with the system's `curl` behind
  it, and the core gains `probe_gguf_header()` so a gguf that parses can be told from one this
  engine does not stream without reading an error's words.

- **Every release carries its compute backends as modules, on every OS.** The engine knows no
  backend at compile time: each is a library ggml loads from the executable's own directory at
  start-up, the CPU one once per instruction-set tier with the best picked on the machine, and a
  module whose driver library is missing is not loaded instead of keeping the binary from
  starting (`BMOE_BACKEND_MODULES`, on by default; iOS stays linked, as the platform demands).
  The APK carries Vulkan and OpenCL beside Hexagon, the Linux and Windows archives Vulkan, the
  macOS one Metal; a module built with another toolchain (CUDA, HIP, SYCL) and dropped beside
  the binary loads like the shipped ones. The planner enumerates whatever registered, measures
  each device in a process of its own and arms a prefill device by the rule that arms Metal and
  the NPU, with no engine change (`docs/backends.md`, `docs/hardware-planning.md`). `--overlap`
  survives the move: the fork's expert-ready hook is reached through the CPU backend's registry
  (`ggml_backend_reg_get_proc_address`) rather than linked, so the fork stays one commit and a
  build on stock upstream refuses `--overlap` with the reason instead of failing to link; the
  server's default and its `overlap_available` follow the backend that loaded
  (`docs/seam.md`). The gates that prove `--overlap` (G4, G5b, S2) skip themselves where the
  hook is absent, and with `-DBMOE_GATES_REQUIRE_HOOK=ON`, which CI's gate job passes, fail
  instead, so a CPU module without the hook cannot pass the suite unseen. Windows gains the
  device probe in a process of its own, so a GPU driver that
  dies there ends the probe and not the server. `scripts/build-portable.sh` is gone: the default
  build is that build. The modules share the engine's C runtime, so the Windows archives carry
  the C++ runtime DLLs beside the binaries and still start on a machine that never installed the
  Visual C++ Redistributable. The Linux x86_64 archive builds on Ubuntu 22.04 with LunarG's
  Vulkan SDK and runs where the previous ones did; the aarch64 one builds on 24.04 for the newest
  ARM tier, as llama.cpp's release does, and needs glibc 2.38 or newer.
- **Android: the app's launcher names no backend.** The engine's linker path is its own
  directory, then `/system/lib64`, then `/vendor/lib64`: the OpenCL module links the phone's own
  `libOpenCL.so` where there is one (through the Khronos loader instead, the two deadlock,
  measured on a Snapdragon phone) and is not loaded otherwise. The vendor directory has to be on
  the path and not reached through a symlink: a process the app starts links in the linker's
  default namespace, which refuses a vendor library from anywhere else, and the OpenCL and
  Hexagon modules then fail to load without a word. Last, it shadows no system library. The two
  Hexagon variables stay set (`GGML_HEXAGON_HOSTBUF`, `GGML_HEXAGON_OPPOLL`); only that backend
  reads them. `scripts/android-gpu-deps.sh` fetches the Khronos headers
  and the OpenCL link-time library at pinned tags, for the release recipe and for CI, which
  cross-compiles both GPU modules on every PR; the release-apk workflow gains a dry run from any
  ref that keeps the APKs as an artifact. Run on a Snapdragon-class phone: OpenCL is 9x the cores
  at prefill width and wrong, Vulkan is right in fp32 and no faster than the cores, so the plan
  arms neither and keeps arming the NPU where the ledger has room; the figures are in
  `docs/npu-prefill.md`, and the crash that followed the NPU load there is #264.
  `docs/backends.md` also corrects one cell: OpenCL's `buffer_from_host_ptr` is wired to a
  function that declines, not to an implementation.
- **A prefill device is confirmed on the model before the session keeps it** (`--prefill-verify`,
  on by default). After the load the session prefills a fixed text, 64 tokens or the device's
  narrowest graph where that is wider, on the host and on the device and keeps the device only if
  its top next token matches the host's in at least 90% of the positions; otherwise it is handed
  back like a device that failed to open, and a plan's device leaves the prefill on the CPU. A
  check that cannot run (a text that does not tokenize, no graph that wide within what one graph
  may return) leaves the device as set and says so: not confirming is no evidence against it. The
  planner's synthetic matmul cannot see a fault only the model's
  activations trigger: a phone GPU's half-precision Vulkan prefill took a 1070-token text's
  perplexity from 32.4 to 711 445. A laptop iGPU agrees in 98%. Three short prefills at load; the
  check judges correctness, not speed (`docs/hardware-planning.md`).
- **A security policy and the forms a contributor expects.** `SECURITY.md` says how to report a
  problem privately and what is in scope; a bug-report issue form and a pull-request checklist
  (gates, changelog and docs in the same PR, no hand-written tunable) join the benchmark form.
- **A log port: where the engine's diagnostics go.** The engine wrote its ~70 diagnostic lines
  ("prefill stays on the CPU", the cache budget it settled on, a policy disarmed) straight to stderr
  from wherever it was; they now go through `bmoe/log.h` (`set_log_sink`, `set_log_level`, three
  levels). The default sink writes the same `bmoe: <line>` to stderr, byte for byte, so nothing that
  reads the CLI's stderr changes; the `BMOE_*` protocol lines and the CSV/trace sinks never pass
  through it. `bmoe-server` keeps the stderr line and also publishes each one as a `log` event
  (`level`, `message`) on `GET /api/events`, so a UI can show why a load fell back.
- **The engine host is tested.** `bmoe-server`'s state machine around the one session (loads in the
  background, turns in line, cancel and unload mid-turn, which failures close the session) had no
  test. It now drives the session through a narrow seam (`server/hosted_session.h`, a real `Session`
  by default), and `tests/engine_host_test.cpp` runs it with a scripted session: a load ending ready
  or in error, a turn with nothing loaded (503 `no_model`), a turn sent during a load waiting for it,
  cancel and unload during a turn, a failed decode closing the session (500) and a recoverable kind
  keeping it (400), the same for decisions, and diagnostics reaching the bus.
- **CI runs the gates under AddressSanitizer and UndefinedBehaviorSanitizer** (a Linux job,
  RelWithDebInfo, llama.cpp instrumented too, so ggml's reads of the streamer's own buffers are
  checked), and **on macOS**, where the job only compiled before: the Apple I/O paths are now proven
  byte-identical on every PR, not only on a developer's Mac. llama.cpp's own sanitizer findings,
  which this project does not patch, are listed one by one in `tests/sanitizers/ubsan.supp` (today:
  ggml-rpc's unaligned reads, reached by the RPC gate). The web UI job also type-checks
  (`npm run check`) and builds. Windows stays compile-only, with the reason in the workflow: its
  gates have never been run.
- **`--scan-dir PATH`: the server lists models outside its own directory.** Repeatable; each is
  only read. `GET /api/models` adds `scan_dirs`, and each local model its `dir` and its `files` (every
  file present, in shard order), so a client deletes a split set without knowing how shards are
  named. The phone hands the server its other model folders this way instead of scanning them in
  Kotlin. Split sets are recognised by a small explicit parser (`server/model_files.h`, tested) in
  place of a `std::regex`.
- **The web UI checks what the server answers.** Every read is checked at runtime for the parts of
  its shape the UI depends on (`ui/src/lib/guards.ts`), and so is every event; an answer that is not
  the documented shape fails where it is read, with `code: "bad_response"`, instead of as an
  undefined inside a component. A reconnection now asks for the config again, beside the schema: a
  change made while the page was cut off arrived as no event.
- `docs/roadmap.md` gains a step that makes measuring and loading readable in every UI: each
  measurement step with its result and duration, the load's progress with a cancel, a notice before
  measuring, and measurements kept between loads if the recorded durations show it pays.

### Changed
- `docs/roadmap.md`: how the server will keep a hybrid model's conversation through a tool loop
  (replay the assistant's own tokens, then a recurrent checkpoint per turn), after issue #240 showed
  Qwen3.8-Flash-Next recomputing the whole history on every tool call.
- **llama.cpp moves to upstream master of 2026-10-18** (523 commits past the 2026-09-26 base): the
  fork branch `bmoe/expert-ready-hook-1810-dl` carries the same single expert-ready-hook commit,
  in the variant that also answers the CPU registry, so the backend-module builds reach it too.
  The new base loads architectures the old one did not (Kolibri-1, `kolibri1`, among them); their
  expert layout is read off the file like any other, but none of them has been run here yet.
  Upstream reworked `common`'s chat and speculative APIs, and the engine follows with the smallest
  change that keeps its behaviour: the rendered prompt is joined back into one string (`.str()`,
  as upstream's own server does), the parser arena is copied instead of reloaded from a
  serialization upstream removed, and the batch handed to the MTP driver is mirrored into the
  `common_batch` it now reads. Same backends and build options, so `docs/backends.md` holds;
  greedy output on a real model is unchanged.
- **The catalog is the two reference models.** `catalog/models.json` offers Qwen3.6-35B-A3B
  (Q4_K_M) and Gemma 4 26B-A4B-it (Q4_0), both from ggml-org, the models the engine is measured
  on and a first run starts from; every other model comes from its own repository through the
  Models screen, where the server reads its header first. The five other entries are gone, not
  because they stopped working but because a list cannot keep up with what is published, and the
  product is the engine's rules, not the list.
- **`Session::open()` and `Session::generate()` read as their steps.** The two functions were 690
  and 770 lines; each is now a short sequence of named steps on the session's `Impl` (`load_model`,
  `read_model`, `build_hook`, `create_contexts`, ..., `bind_streaming`, `describe_run`; a `Turn` with
  `render_prompt`, `tokenize_prompt`, `reuse_kv_prefix`, `prefill`, `decode` and its `draft`,
  `verify_decode`, `accept`, `emit`, then `summarize` and `finish`). The order of operations, the
  error kinds and messages and every metric are unchanged, which the byte-identity gates confirm.
  The router hook's eval callback is split the same way (capture, compute trace, device arena, row
  tables, routing node), each job reached only when its feature is on, so a node no job wants costs
  what it did.
- **`RunConfig` holds tunables only, and the table names every bound.** `--progress` and
  `--compute-trace-layers` were hand-written flags writing two `RunConfig` fields the parameter
  table had no row for. Neither is a tunable of a run, so neither is in `RunConfig` any more: the
  line protocol is the CLI's output mode and reaches the engine as `RunOutputs::render_text`
  (`run()` now takes the token callback, the sinks and that switch as one `RunOutputs`), and the
  compute trace's granularity is the trace's own (`IComputeTraceSink::layers_only()`, set by
  `make_csv_compute_trace_sink(path, layers)`); the CSV preamble's `compute_trace_layers` is now read
  from the attached trace. `BMOE_PROGRESS` goes through the CLI's one table of environment
  overrides instead of a line of its own. `--ppl-skip` is read with the table's strict parser, now
  public as `parse_int_strict`: `4x` is an error, not 4, and `abc` not 0. The table's
  last literal bounds are named constants that `validate()` checks too (`SamplingConfig::temp_max`,
  `top_p_max`, `seed_random`, `SpecConfig::draft_p_min_max`,
  `MoeStreamConfig::drop_cold_frac_max`, `substitute_lambda_max`). The sink factories return
  `std::unique_ptr`.
- **One probe-and-plan sequence, in core.** `bmoe-cli --auto` and the server's `/api/plan` each
  carried the planner's probe calls in the same order, the cache-floor arithmetic behind the
  headroom probe and the two-minute device-probe limit, kept in step by hand. Both now call
  `probe_and_plan` (`core/include/bmoe/auto_plan.h`) with what differs between them as options (the
  storage and memory probes, how the executable starts itself again to probe a device), the child of
  an isolated probe is one call (`probe_device_child`), and neither front-end includes `llama.h` to
  plan. The rules are unchanged and still pure: `plan_run` is now a list of named stages over one
  planning state and `allocate` a list of named steps, and every plan the planner tests make is the
  same before and after, decision for decision. A reason that tells a person what to type spells
  the flag through the parameter table (`flag_spelling`), so a renamed flag cannot leave advice
  naming one that no longer parses; the one reason that named a front-end flag (`--auto`) now says
  "the planner". The smallest context the rationale suggests and the capacity fitter's device
  margin and minimum context are named constants.
- **Tensor and graph-node names are read in one place.** How llama.cpp spells a block's tensors, an
  expert weight, a per-layer graph node and the router weight chain, and the capacity fitter's
  expert-override patterns, was parsed where it was used: a `sscanf` in the session and the prefill
  path, two copies of the node matchers in the router hook and the decide probe, a third block
  parser in the model probe, and a `std::regex` in the placement probe. All of them now go through
  `core/src/moe/tensor_names.{h,cpp}`, with one rule for an index (digits only, the whole of it) and
  no regex, and `tests/tensor_names_test.cpp` covers the shapes the conventions produce and the near
  misses each rule rejects.
- **The UIs read facts, not parameters.** The live line and the metrics card took the flash term
  from `stall` or `io` by reading the `overlap` parameter, and the streaming badge guessed from
  `moe-stream` what a load would do. The engine now reports the wall-additive flash term already
  chosen (`TokenMetrics::flash_wait_ms`, `RunSummary::moe_flash_wait_s_per_token`), the server's
  events carry it as `flash_wait_ms` and `flash_wait_s_tok`, the CLI's `BMOE_DONE` as
  `flash_wait_s_tok`, and the loaded model in `/api/info` carries the threads a CPU figure is read against (`compute_threads`, `busy_threads`). The web UI
  names no engine parameter any more; its metrics card shows the flash wait in place of the stall
  share, which read 0 on every serial run, and the badge appears once a model has loaded, since
  only the load knows (a dense model, a plan that declines).
- **The Hexagon backend's environment is the server's to set.** Polling for the DSP and the host
  buffer type the prefill device moves the KV cache into were set by the Android service before it
  started the server; they are platform policy, and now live in `server/platform.cpp`, set first
  thing in `run()` (so a device-probe child inherits them) and only where the environment has not
  set them already.
- **The Android app reads the server's facts and labels, and keeps no model logic.** The live line
  and the metrics card took the threads, the read lanes and overlap from the settings with invented
  defaults (4, 4, on); they now read the flash term and the threads the server reports. The
  metrics screen's hand-kept labels for the CSV preamble (`CONFIG_ORDER`, `ConfigFields`) are gone:
  a recorded setting is labelled, typed and explained by the parameter table, and a run's identity
  names every setting that changes the reply and is off its default (the table's `lossy`), instead
  of a list of keys. The Kotlin gguf parser that decided MoE-ness from a text marker
  (`GgufHeader.kt`) and the shard-name regexes are deleted: the service starts the server with the
  phone's other model folders as `--scan-dir`, the Models screen lists what the server lists -
  every gguf, so a dense model the engine runs as plain mmap now appears too, labelled as one this
  build does not stream, where the app used to hide it - and a
  delete removes the files the server named. A summary's rate is read by field name, not by regex,
  a chat error is acted on by its `code` (`no_model` links to Models), a closed port is told apart
  by `ConnectException`, not by the text of a message, and a failed message can be sent again, as
  on the web; a failed decision leaves no question behind to be sent as history.
- **The line under an answer names each rate by its phase, and gives the prefill a rate.** It read
  `1.7 tok/s · 9 tok · prefill 4.0s (13 tok)`: a bare rate that was the decode's, and a prefill
  given only as time. It now reads `decode 1.7 tok/s (9 tok) · prefill 3.3 tok/s (13 tok, 4.0 s)`,
  in the web UI and in the app alike, from the `prefill_tps` the server already sends. The prefill
  counts the tokens the turn read, which with the conversation reused from the KV cache is the new
  message. `docs/screens.md` gives the line.
- **No table of architectures: which tensors are the routed experts is read from the file.** A
  routed expert weight is a block's `.weight` with three dimensions whose third is the expert count
  the file declares, which is how llama.cpp lays out every weight it hands to the expert matmul; the
  shared expert, the router, a per-expert scale and the router's bias all have another rank and fall
  out by shape (`bmoe/expert_layout.h`, unit-tested). The registry of ten architecture rows
  (`arch_registry.cpp`, `recipe.h`) is gone, and `--list-archs` with it: of the 57 model builders
  the pinned llama.cpp routes through `build_moe_ffn`, the engine named 10, and 8 of the rows spelt
  the same three names. The refusal stays and names its reason: a dense model, a file whose MoE
  layers disagree on their expert tensors, or one with more than three per layer is refused on the
  CLI, in the plan and in the server's `streamable` fact, which now comes from the same reading of
  the file. The three layouts (split, fused, gate-less) keep their byte-identity gates, and a new
  layout gets a gate, not a row (`docs/which-models-stream.md` replaces `adding-a-model.md`). The
  CLI's `mode:` line reads the file's expert count (`RunSummary::n_expert`) to tell a MoE run
  without streaming from a dense model.

### Fixed
- **A host-only run hands llama.cpp no device instead of naming the CPU as one.** Since `--devices
  auto` (0.29.0) a run with no layer on a device passed the CPU in llama.cpp's device list, which
  names the devices it offloads to; the context then built a second CPU backend beside its own and
  scheduled the graph on it, with the other idle. An empty list is what a build without a GPU
  backend gets from llama.cpp's own selection: one CPU backend, and still no GPU in the scheduler.
  Same output and the same memory; one backend fewer.
- **The planner counted every block of a hybrid model as a MoE layer on the host.** The fitter's
  list of layers left on the host speaks in llama.cpp's block indices, and on an architecture
  where most blocks carry no experts (Nemotron-H, Qwen3.8-Flash-Next, the leading-dense families)
  the plan read "52 of 23 MoE layers keep their experts on the host" and prorated the expert set
  by that ratio: 2.3 times the bytes on the host, in the regime line and the cache arithmetic. The
  model profile now names the blocks that carry experts (`moe_layers`), the placement probe keeps
  only those in its host list, and the planner counts the list against them either way.
- **The page two experts' slices share is released with the last of the two.** A slice is
  committed through the slack after it, which is the head page of the next slice; eviction
  released only the pages between the two boundaries, so once an expert had been resident the
  head page of its right neighbour stayed committed until the buffer was freed, outside the
  cache ledger: one page per expert and projection, up to 72 MiB on a 48-layer, 128-expert
  model. A boundary page now goes when neither neighbour is resident or being read.
- **The device agreement check is llama.cpp's own.** The planner compared a device's matmul with
  the host's under a private bound five times stricter than the normalized mean squared error
  `test-backend-ops` allows a matmul (5e-4), and refused a laptop iGPU whose half-precision
  prefill leaves the model's perplexity unchanged (32.58 against 32.46). The measure and the bound
  are now upstream's; the confirmation above catches what a matmul cannot.
- **A device that pads a single expert past its share of the slot is refused, not aborted on.**
  The OpenCL backend adds carve slack to every quantized tensor's allocation, so one expert's
  allocation outgrows its stride and the last expert reached past the slot, where ggml aborts
  inside the allocation instead of returning. The arena checks it when it sizes the slots and
  refuses with the reason, where a planned device still falls back to the host.
- **The capacity fitter's placement is applied only where every device it used has memory of its
  own.** The check was whether any device on the machine did. On a phone an NPU, whose buffers
  come from a kernel allocator and states no size, passed it, and the fitter's answer, 41 layers
  and 2106 MiB on the GPU beside it, was applied although that GPU's memory is the host's and the
  plan's ledger held none of it. The devices given bytes are now the ones asked, each with a stated
  size; where one shares the host's memory the placement is quoted and not applied, as it already
  was where every device does.
- **A prefill device is charged its own compute buffer, not the fitter's sum.** The fitter projects
  compute buffers for the devices it places layers on, and the planner charged their sum to
  whichever device would prefill. On a phone that put a GPU's 467 MiB on the NPU beside it, which
  the fitter never places on, and took it from the expert cache (904 MiB instead of about 1370, and
  one expert slot instead of two). Each device's part is now kept, and the prefill device pays its
  own.
- **Every buffer the engine allocates for tensor data keeps a page readable past its end.** One of
  ggml's CPU kernels (Q6_K, on cores with SVE and int8 matmul) loads 14 bytes past a block's end,
  which llama.cpp's own buffers absorb. A dense weight in a buffer sized to it exactly, with a guard
  page after it, faulted: seen on a phone, at random, once the release carried that CPU variant.
  The dense copies, the expert slots and cache regions, the row-gathered tables and the probe's
  host copy now end with the OS's page of slack, committed wherever the platform faults on a page
  that was only reserved.
- **A turn sent the moment a load is announced waits for the load.** `EngineHost::load()` reads
  "loading" before its thread has taken the generation lock, and a request in that gap took the lock
  first, found no session and answered 503 for a model seconds from ready. A turn now gives the lock
  back while the state reads loading and takes it once the load has ended either way; the engine
  host test that caught it on Windows runs stable (30 runs under `ctest`).
- **The prefill device's dense layer slots are one table, indexed as they were sized**, in place
  of a conditional between two member arrays. The two spell the same selection, so this changes no
  behaviour: it was made while gate G17 (nemotron_h_moe) aborted under AddressSanitizer and
  UndefinedBehaviorSanitizer with a recurrent layer placed in the wrong parity's slot, and the
  gate has passed since, but the cause of that abort was not found and this rewrite cannot have
  been it. The fault is recorded as open (a race in the arena's workers or an uninitialised read
  are the candidates), and the sanitizer job is where it would show again.
- **Two gate runs on one machine no longer meet on one port.** The prefill-device gate served its
  RPC device on a fixed `127.0.0.1:50599`, so a second build tree or CI job running the gates at the
  same time failed to bind, or registered the other run's server; the port is now one the kernel
  picks per process.
- **The docs index the pages they have, and name the test phone as a class.** `docs/README.md`
  lists the application framework, the five screens and the visual language; the bench-data index
  lists all eighteen sessions; the agent guide's project map names `session.cpp` as the composition
  root; and the benchmark pages, bench notes and scripts call the phone "the 12 GB test phone".
- **The server holds against malformed requests, the pages around it and the network.** A field of
  the wrong type in a request (a `role` or a content part's `type` that is a number, a download
  `id` that is an object, a body that is an array) threw out of the JSON library and answered
  `500`; every read is now type-checked, a body must be an object, and anything a handler still
  throws on a JSON value is a `400` with `code: "invalid_request"`. An answer no longer makes the
  browser fetch anything: the Markdown renderer keeps an image only when its bytes are inline
  (`data:`) and shows any other as its alt text, so text that talks the model into writing
  `![](https://host/?q=...)` can no longer send what the URL carries, or GET the server's own API;
  media, `srcset` and `background` go too. Every response now carries a Content-Security-Policy
  (`default-src 'self'`, images from the page or `data:`, no inline script or style, no framing, no
  form submission) with `nosniff` and `no-referrer`. A request with no `Origin` that the browser
  marks `Sec-Fetch-Site: same-site` (another port on the same host, where a hostile local page is
  served) was answered; only `same-origin` and `none` are now. `--api-key-file PATH` reads the key
  from a file, since a command line is visible to every local user; `--api-key` stays. A client on
  the network may name a model only in a folder the server lists models from, the models directory
  and each `--scan-dir` (canonicalised, `..` and links resolved; `403` `outside_models_dir`
  otherwise), and `--models-only` confines local clients too. Both "no model loaded" refusals of
  the chat and decide routes carry `code: "no_model"`, which the UIs' Models link reads.
  The default host, port, context and answer length are named in `server/server.h`, and the usage
  text is built from them; `--port` is read strictly. `docs/server-api.md` says all of it.
- **The release workflows no longer splice typed text or a secret into a shell script.** The tag a
  person types into a manual run, and the release's tag name, were expanded straight into `run:`
  lines, so a tag with shell syntax in it would have run on a job that signs and publishes; they
  now reach the shell as environment variables, quoted, and a tag that is not a plain ref name
  (letters, digits, `.`, `_`, `-`) stops the run. The signing passwords were expanded inside an
  unquoted heredoc, where a `$` or a backtick in one is shell syntax; they are environment
  variables written with `printf '%s'`. `ci.yml` declares `permissions: contents: read` for every
  job but the one that attaches the APK, which already asks for its own.
- **Any app on the phone could read the conversations.** The Android app started the server on
  loopback without a key unless it was shared, and loopback on a phone is reachable by every
  installed app: any of them could follow `/api/events` and read every prompt and answer, or drive
  the engine. The server is now always started with a key: a random one per launch
  (`SecureRandom`), handed over in a file of the app's private storage with mode 0600
  (`--api-key-file`, never on the command line), or the persistent key Settings shows when it is
  shared; every request carries it. Before it trusts the server on the port the app checks that it
  is its own (it refuses a random key and accepts the launch's), and a port held by another program
  is reported as such and sent nothing.
- **Stopping the engine no longer blocks the phone's main thread.** A stop waited up to six seconds
  for the server to exit on the main thread; starts and stops now run one at a time on a background
  thread, a restart starts the next server only once the previous one has ended (it slept 800 ms
  and hoped), and a stopped server's exit, which arrives seconds later, no longer overwrites the
  state of the one that replaced it (each launch has a number, and only the current one's exit
  counts). Every call has a read timeout fitted to what it waits for, instead of none; the chat
  stream and a decision still wait as long as they must, and the event stream has a watchdog that
  reconnects after two missed pings. A cancelled coroutine is no longer read as a failed call.
- **A failed message stays in the web UI's transcript.** A send refused before anything streamed
  moved the message back into the composer and out of the conversation, against the screen
  specification; it now stays, saved, with the error under it, Send again resends it as it is, and
  a server without a model links to Models (`code: "no_model"`). A failed decision still leaves no
  question behind, since it is not a conversation turn. Tested against a stubbed server.
- **Direct reads are aligned to what the file's filesystem states, not to 4096.** Every reader cut its
  O_DIRECT windows at 4 KiB, and the storage probe reported that literal as the storage's alignment. A
  filesystem formatted with 16 KiB blocks rejects a 4 KiB window, and the open-time check then put the
  run on buffered reads, blaming the storage for returning wrong data. The reader now asks the
  descriptor it opened (`pio::direct_alignment`: `statx(STATX_DIOALIGN)` on Linux and Android where
  the kernel and the app sandbox have it, the filesystem's block size otherwise; the volume's sector
  sizes on Windows; the block size on Apple, where `F_NOCACHE` needs none), keeps 4096 only as the
  named fallback, and the probe reports `0` (unknown) instead of a value it did not read. Buffers the
  reads land in are aligned to that granule and never less than a page. Three more facts are no longer
  assumed: the refault price is read at this machine's page (16 KiB on Apple silicon) rather than at 4
  KiB; a row-stream slab is a whole number of pages, so on a 64 KiB-page kernel evicting one slab can
  no longer zero its resident neighbours; and the expert-readiness flags are padded to 128 bytes, the
  widest cache line of any target, instead of 64. The swap probe finds a zram device under any number
  rather than only `zram0`, and the spin-wait's pause and the decide probe's 64-bit file reads moved
  behind `core/src/io/`, so no operating-system branch is left in `core/src/moe/`.
- **The plan names every compute device it did not weigh for the prefill, and why.** A device
  with no measurement at prefill width dropped out of the `prefill-device` line without a word, so
  a plan read the same whether the machine had no such device or had one that could not run this
  model. On a phone with an NPU, a Q2_K model planned as if the NPU were absent; the line now ends
  with "HTP0 was not weighed: it does not run this model's expert matmul in the model's own
  format", or "it gave no measurement at that width" where the reason is not known.

## [0.30.0] - 2026-10-16

### Added
- **`docs/platforms.md`: what runs where.** One page with a matrix per operating system (engine,
  server, native app, planner, isolated probes, CI, release artifact, the hardware it was run on)
  and per compute backend, each cell linked to the page that backs it, and what each platform
  still owes. Linked from the docs index and from `docs/backends.md`.
- **The Android app is a client of `bmoe-server`, and a complete app.** The APK's `libbmoe-server.so`
  runs as a child process of a foreground service, on loopback with the app's data beside its
  files, and the five screens speak its HTTP API as the web UI does: chat with conversations saved
  on the phone (a drawer to switch, copy, regenerate, stop; a full context says so and offers a new
  chat), Models (the server's directory, the catalog from the one `catalog/models.json` shipped as
  an asset, the app's own resumable downloads since a phone has no `curl`, imports, loading by
  path), Plan (the planner's decisions with their reasons, measure and apply, auto-plan on every
  load), Settings rendered from the parameter table with a level filter and per-value reset, and
  Metrics with the live card and the CSV views, now written by the app from the server's token and
  done events. **Serve on the network** binds the engine to every interface with a generated key,
  so any OpenAI-compatible client uses the phone. The Kotlin that reimplemented settings, the
  catalog, the line protocol and the telemetry parser is deleted, not ported: no engine logic in
  the app (`docs/application-framework.md`). The code's package is `io.bigmoeonedge.app`; the
  application id stays, so the update installs over the previous build. Run on a 12 GB phone:
  Gemma 4 26B and Qwen3.6-35B load from the Models screen with the planner's plan, chat with the
  reasoning block, 3.3 tok/s, every screen live. Two things found there: Android forbids cleartext
  HTTP even to loopback for an app targeting API 28 or later, so the app's network security
  configuration permits it for `127.0.0.1` alone and the status line now says why the engine is
  unreachable instead of "not running"; and the settings page no longer hides rows behind a level
  filter: each group shows its basic rows and an **Advanced** section at the end, folded, holds the
  advanced, experimental and debug rows grouped and marked, so no lever is lost. The page names no
  device: the prefill device is a row like any other and the planner chooses it.
- **`POST /api/decide`: a decision over the API.** The CLI's decide request (which of a list of
  choices the model would answer next, from one prefill and no decode, `docs/decide.md`) is offered
  by the server with the same fields and the same answer keys as `BMOE_DECIDE`, waits its turn like
  a chat turn, and is published as a `decide` event. The `decide` parameter is on by default in the
  server, which costs nothing until a decision is asked for. The request and answer are one pure
  protocol file (`server/decide_protocol.h`) with tests. With it, Choose from options can return to
  every UI from one endpoint (#245).
- **What a client on a phone needs from the server.** `GET /api/models` lists every file of a
  catalog entry with its URL (`files`), so a host with no `curl` downloads for itself from the one
  catalog; the `done` event carries the CLI's whole `BMOE_DONE` summary (load time, faults and CPU
  per token, the loop overhead, the speculation counters, cache churn), so a metrics view reads the
  two front-ends alike; and the planner's device probes run in a process of their own in the server
  too, as `bmoe-cli --auto` runs them (`--probe-device` is the child's mode, `--no-isolated-probes`
  turns it off), so a driver that ends the process it is used from marks the device unusable
  instead of ending the server an app is running behind.
- **`--api-key`: the server can be reached from the network, with a key.** With `--api-key KEY`
  every request to `/api/*` or `/v1/*` must carry `Authorization: Bearer KEY` or is refused with
  `401`; the UI's own files are served without it, and the UI asks for the key on a `401`, keeps it
  in the browser and sends it on every request, the event stream included (which now goes through
  `fetch`, since `EventSource` cannot carry a header). A `--host` beyond loopback without a key is
  refused at start, unless `--no-api-key` says the open server is meant. Who may reach the API
  (host name, origin, key, content type) is one pure policy, `server/access.h`, with its own tests;
  the HTTP layer only reads the request into it. `docs/server-api.md` states the result.
- **The server is a library, and the Android builds carry it.** `bmoe_server` holds the engine
  host, the model library, the planner adapter, the platform and the HTTP API behind one
  `bmoe::server::run(argc, argv)`; `bmoe-server` is `main()` over it, and an app that may not start
  a process (iOS) can call it on a thread. Every Android build (CI, the release workflow, the two
  build scripts) now compiles the server and stages it in the APK as `libbmoe-server.so`, which the
  app does not start yet. `docs/application-framework.md` says why: the server is the application
  framework, one API behind every UI, in process or in a child process as the platform allows.

- **One visual language for every UI: pixel, from the logo** (`docs/design.md`). The chip's own
  palette as the tokens (dark by default, light when asked), Pixelify Sans (OFL, bundled) for the
  product's own voice (titles, the screen names, badges, every figure the engine reports), the
  system face for prose, square boxes with 2 px edges and a hard shadow, the mark beside the
  wordmark in every bar. The web UI carries it in its stylesheet, the Android app as a Material 3
  colour scheme, typography and shapes; the launcher icon is the chip.
- **The web UI shows the five screens the phone shows, the same way.** Chat, Models, Plan,
  Settings and Metrics in that order, Metrics a page of its own with the live card. Chat keeps
  conversations in the browser (a list to switch, open and delete; a reload finds them), writes
  the same one-line figure under every answer the phone writes (rate, tokens, prefill, context,
  cache), offers Copy and Regenerate, and shows the live rate line above the prompt; how many
  tokens to generate and whether to think are Settings rows, not composer controls. Settings shows
  each group's basic rows and one folded Advanced section with the rest, grouped and marked, in
  place of the level filter.
- **Choose from options, in the web UI and in the Android app.** A switch under the prompt turns
  it into a question with a field for options, one per line; the model picks one without writing
  an answer (one prefill, no decode, `POST /api/decide`), and every option comes back with the
  probability the model put on it, the picked one marked. The two UIs build the same request and
  read the answer the same way, from one pure module each, with tests; a decision is not a
  conversation turn and stays out of the history the model is sent. The Android app had this over
  the CLI's line protocol and lost it on its move to the server; now both have it from one endpoint.

### Changed
- **A shorter README, for what the project is now.** It introduced a way to run a model past RAM
  on a phone, CPU only. It now leads with what the engine is: MoE streaming on llama.cpp's public
  API, self-configuring (`--auto`) to the model and the machine, and one application layer,
  `bmoe-server`, behind the Android app, the desktop web UI and an OpenAI-compatible API that can
  serve the network with a key. A third of its former length: one summary table each for settings
  and benchmarks, with the full tables moved, not dropped, to the new `docs/settings.md`, to
  `docs/benchmarks.md` and to `docs/benchmark-method.md`; the per-platform table moved to
  `docs/platforms.md`. `docs/settings.md` names each setting as the parameter table does, gives the
  bounds the code enforces and the server's defaults where they differ from the CLI's, in place of
  the former app's own steps and defaults, and `docs/cache-aware-substitution.md` no longer
  describes a settings menu the app no longer has.
- **`docs/screens.md`: the five screens, specified once.** What chat, models, plan, settings and
  metrics show, what a person can do on each, what every state means, and the rule behind them (a
  UI knows no engine parameter by name), so the web UI, the Android app and an iOS app render the
  same thing their own way (#236).
- `docs/roadmap.md` opens with the product plan, in the order the work has to happen: one engine,
  one application framework, native UIs per OS, the OpenAI-compatible endpoint everywhere. The
  research themes are its second half. Two steps join it: the Vulkan and OpenCL GPUs (a build that
  carries each, a measured prefill run, then the host-memory route for streamed experts), a
  Connect view that tells any client what it needs (#257), and one diagram of the planner's logic
  for `docs/hardware-planning.md` and the README.

### Changed
- **A load says it is planning.** `state` reads `planning` from the moment a load in auto mode
  starts measuring the machine until it starts loading, and both UIs show it ("Measuring, then
  loading…") and keep the Load buttons quiet meanwhile: on a phone the probes take tens of seconds,
  during which a tap on Load looked like nothing.
- **Settings: the Advanced section holds every knob.** The groups above it stay the recommended
  configuration (the basic rows); the folded section at the end now lists every row of the table,
  grouped and marked with its level, so an expert sees the whole engine in one place. Web and
  Android alike.

### Fixed
- **A request no longer fails because its answer budget is larger than the room left.** The session
  reserved the whole `n_predict` against the context before generating, so a two-word prompt was
  refused whenever Max tokens was set near or above the context size: the first outside client
  tried on a phone (Open WebUI, which sends no `max_tokens`) got `prompt + n_predict exceeds the
  session n_ctx` on every message. `n_predict` is now a ceiling: the answer gets what the context
  has left after the prompt and stops there, which the OpenAI endpoint reports as
  `finish_reason: "length"`. Only a conversation that fills the context is refused, with a message
  that says what to do (start a new conversation or load with a larger context size). A
  byte-identity gate checks that a budget past the context produces exactly what a budget of the
  room produces (#138).
- **A failure is recognised by its kind, never by its wording.** The CLI, the server and the
  Android app told a full context, an empty prompt or a refused template from a crash by searching
  the error message for a phrase, so rewording a message would have closed sessions that were
  fine, or offered New chat for the wrong failure. The engine now returns a kind with every
  failure (`core/include/bmoe/run_error.h`), and its stable name travels as `code`: on
  `BMOE_ERROR` lines, beside `error` in every API error body, and in the stream's error chunk,
  with the server's own refusals (`busy`, `no_model`) alongside. Both UIs decide on
  `code: "context_full"`, and the web UI now offers New chat on a full context as the phone does.
- **The roadmap linked a page that is not on `main`.** It pointed at `docs/cache-stall-sizing.md`,
  which exists only in the draft PR #238; it now points at the PR.
- **A second plan could leave the configuration invalid and refuse every load.** A plan's values
  were what differed from the current configuration, which already held the previous plan; a
  choice the new plan repeated (`force-cache` on, for a cache the model's token cycle allows
  under the generic floor) was therefore not a value, and applying the plan, which replaces the
  layer whole, dropped it: `cache-mb` below 1500 with no `force-cache`, `valid: false`, and
  `POST /api/session/load` answered with the validation error. Seen on a phone loading a small
  model after a larger one. A plan is now made from, and measured against, the configuration
  without the plan layer (`SettingsStore::planning_config`), so it is complete on its own; tested.
- **The Windows desktop archive did not start on a machine without OpenSSL.** `bmoe-server.exe`
  imported `libssl-3-x64.dll` and `libcrypto-3-x64.dll`, which the archive does not ship: llama.cpp's
  `LLAMA_OPENSSL` option defaults to on, and the `cpp-httplib` the server builds from its vendor tree
  links the OpenSSL the build runner happens to have. Double-clicking the exe hung on the "DLL was
  not found" dialog; it worked from Git Bash only because Git for Windows carries those DLLs on its
  `PATH`. The server speaks plain HTTP on loopback and downloads go through the system curl, so the
  option is now off and the binary links no TLS stack. The CLI archive was never affected (#242).

## [0.29.0] - 2026-10-13

### Added
- **`--auto` chooses how wide a prompt is computed (`--ubatch`), and counts its buffers at that
  width.** The compute buffers are reserved for the widest graph, which followed the context, while
  the ledger read them at llama.cpp's default of 512 tokens with a logit row per position: about
  right at a 2048 context, and 1.2 GB charged for 8 GB reserved at 16384. The projection is now taken
  at the session's own width and at half of it, so a narrower graph can be priced, and the width is
  the widest whose buffers stay inside a tenth of what the process may hold
  (`PlannerPolicy::compute_share_max`), never below 512. On a 16 GB Apple-silicon laptop,
  Qwen3.6-35B-A3B Q4_K_M at a 16384 context with `--auto` alone: decode 22.4 and 22.3 tok/s (14.6
  and 16.7 before), the cache locked at its full 7.7 GB (it settled at 3.5), and a 10850-token
  prompt in 31.8 s (20.4 before, at a width that cost the decode a third). A 2048 context plans as
  it did. Measured on that one machine; `docs/hardware-planning.md` lists the others it is owed on.
- **An iOS harness (`examples/ios`, `scripts/build-ios.sh`).** `bmoe-cli` compiled into an app,
  since an app there may not start a process: one field for the command line, the CLI's own output,
  and a `--bmoe` marker to script a run from the machine the device is attached to. On iOS the
  planner now reads the process's own memory share instead of the system's free memory, and probes
  devices in-process. CI compiles it for the simulator. Not run on a device: nothing the planner
  measures there has been seen.
- **`--auto` probes each compute device in a process of its own.** A device is only as sound as
  its driver, and a backend that calls a function the driver lacks ends the process with no error
  to catch. Each device beyond the host is now asked by a child process first; one whose probe
  dies or does not answer in time is marked unusable, shown as such in the plan, and kept out of
  the run. On a 2020 phone a build carrying the Vulkan backend used to die while planning; it now
  plans in 23 s, reports the GPU unusable, and runs on the cores. `--no-isolated-probes` turns it
  off. Not yet on Windows or in the desktop server, where the probes run in-process as before.
- **`--prefill-slots-on-demand`: the prefill device's expert slots take turns with the expert
  cache.** The two are never busy at once, so the slots are held only while a prefill runs on
  the device: the cache yields what they need, they are built, and after the prefill the cache
  has its budget back. `--auto` arms a device this way, which gives decode most of its cache
  back and lowers how much memory must be free before a device can be used. On a 16 GB
  Apple-silicon laptop, Qwen3.6-35B-A3B Q4_K_M with `--auto`: prefill of 1461 tokens 3.52 and
  3.64 s (3.43 with the slots held), decode on a short prompt 21.9 tok/s (20.9 with the slots
  held), same text. On a 12 GB phone with an NPU, the model at Q4_0, `--auto` alone armed the
  NPU: 1461 tokens in 10.9 s, decode 3.13 tok/s. Gates G17i score it bit for bit.
- **`--prefill-slots 1|2`: a prefill device can hold one layer of experts instead of two.** One
  slot reads each layer at its own routing node with the device waiting, and costs a layer less
  of memory; the result is the same, and the gates score both bit for bit. `--auto` uses two
  where they fit the ledger and one where only one does. On a 16 GB Apple-silicon laptop,
  Qwen3.6-35B-A3B Q4_K_M, a 1461-token prompt: 3.43 s with two slots, 3.94 and 4.19 s with one,
  498 MiB less held. On a 12 GB phone's NPU, the model at Q4_0: 10.2 s with two, 11.7 s with one.
- **`--auto` chooses the prefill device.** The planner measures this model's matmul 128 tokens
  wide on the host and on every device - the shape of a prefill, which ranks devices in the
  opposite order to the one-token decode - and arms the fastest device that gave the host's
  result, wins by a margin, and whose two layer slots still fit the memory ledger. Every refusal
  names its reason and its figures, and `--plan` prints one line per device with both
  measurements. On a 16 GB Apple-silicon laptop the GPU measures 7.6x the cores on the wide
  matmul and 0.6x on the one-token one; Qwen3.6-35B-A3B Q4_K_M with `--auto` alone prefills a
  1461-token prompt in 3.4 s against 21.2 s on the cores, and gives up 1.55 GB of expert cache
  for it (decode on a short prompt 20.9 tok/s against 23.3). The text after a device prefill can
  differ from the text after a host one at a near tie; an empty `--prefill-device` keeps the
  prefill on the host. See `docs/hardware-planning.md`.
- **`--prefill-best-effort`**: a prefill device that cannot be set up leaves the prefill on the
  host instead of failing the load. Set by `--auto` when it chose the device; a device named by
  hand still fails loudly.
- **The planner keeps one memory ledger, with two ceilings.** Everything a run holds - context
  and compute buffers, a device armed for prefill, the dense set, the expert cache - is a row
  charged against what the process may hold and, separately, against what it may hold
  reclaim-exempt, net of what is already locked. The dense set is pinned whole or not at all, and
  the cache is sized to what can be locked where it can be locked at all. Before this a plan
  could ask for more locked memory than the machine grants; `--plan` prints the ledger row by
  row. Context and compute buffers are now charged on machines whose devices share the host's
  memory, where they were charged nowhere. See `docs/hardware-planning.md`.
- **`--dense-weights ahwb` on macOS**, as wired memory. The planner chooses it when the dense set
  fits the lockable total.
- **`--cache-pin-fit`: a lock refused part way can shrink the cache instead of ending pinning.**
  The budget steps down to what the platform grants and stays locked. `--auto` sets it when it
  sized the cache to the lockable total; by hand it is off, so a budget set by hand is never
  shrunk unasked. On a 16 GB Apple-silicon laptop, DeepSeek V4 Flash at UD-IQ2_M with a pinned
  dense set and a 4.1 GB cache asked for: 0.27 tok/s without it, 2.48 with it; `--auto` on the
  same model 2.48, with no refusal. Separately, a pinned dense store that runs out during load
  falls back to ordinary buffers for the remaining tensors and the run continues.
- **The desktop server refuses cross-site requests on every method, and says what it does not
  protect.** A request to `/api/*` or `/v1/*` from another site is refused whether it carries a
  foreign `Origin` or is marked `Sec-Fetch-Site: cross-site`, so a web page can no longer make the
  server measure the machine or hold its workers with an image tag. Loads, unloads and plan
  measurements are serialised (two racing requests could end the process), request bodies are
  capped at 8 MiB, parameters that make the engine write a file are refused over the API, and an
  IPv6 loopback host name is accepted. `docs/server-api.md` gains a security model section.
- **The desktop server takes writes only as JSON, and an answer in the UI cannot carry a form.**
  Every `POST`, `PUT` and `DELETE` to `/api/*` or `/v1/*` must declare `Content-Type:
  application/json`, body or no body, or it is refused with `415` ahead of routing; the UI's
  chat view drops form controls and styles from model output. Together they close one path: text
  that talks the model into writing a form gave a same-origin request, one click away, whose
  `text/plain` body parsed as JSON and could start a download or a load. `max_tokens` is clamped
  to the `n-predict` bound and refused with `400` when negative or not an integer. The UI's
  `dompurify` and two build-time packages move past published advisories, and CI runs the UI's
  unit tests.
- **macOS builds carry Metal, and the prefill device runs on it.** `--prefill-device MTL0` sends
  wide prefill graphs to the GPU through the same two-layer arena the NPU uses, with no change to
  that path. On a 16 GB Apple-silicon laptop, Qwen3.6-35B-A3B Q4_K_M streamed, a 1473-token
  prompt prefills in 3.4 s against 25 s on the CPU, with the same generated text; see
  `docs/npu-prefill.md`. `--auto` chooses it (see the prefill-device entry above).
- **The expert cache is pinned in RAM where the platform allows it (`--no-cache-pin` turns it
  off).** macOS answers memory pressure by compressing anonymous memory, quantized weights barely
  compress, and so the compressor ended up holding 8 GB of RAM to store the cache badly while
  every hit paid a decompression that no fault counter shows: 2 GB/s compressed during decode.
  Same model, cache and threads on that laptop, arms off on on off: 7.4 and 7.2 tok/s unpinned,
  18.2 and 18.3 pinned, same text; 24.2 tok/s with `--auto`, which gives the cache 9.8 GB. Where
  the platform refuses (Android caps locked memory at 64 KiB) the cache is ordinary memory as
  before, and the run says which it got.
- **`--devices auto`, the new default.** llama.cpp is handed every device when a layer is placed on
  one and the CPU alone otherwise, so a build that merely carries a GPU backend runs a host-only
  plan like a build without it: no nodes drifting to an idle device, and the gates pass to the bit
  with Metal compiled in. `all` and `cpu` still force either. Before this a Metal build with
  nothing placed ran slower than a CPU build and failed gate G18c.
- **The prefill-device and decide flags are parameter-table rows**, so `--describe-params` and the
  desktop UI list them like every other setting.
- **Hardware planner: `--auto`, `--plan`, `--probe`, and `/api/plan` in the desktop UI.** The engine
  measures the machine and reads the model, then resolves the streaming knobs with the fact behind
  each one; a knob set by hand is never touched and nothing lossy arms itself. The planner's
  placement knobs join the parameter table (`--gpu-layers`, `--threads-batch`, `--devices`,
  `--dense-on-device`). See `docs/hardware-planning.md`.
- **`Session` takes a conversation whole, with tools.** `GenerateRequest::messages` replaces the
  engine-held conversation (the KV is kept and reused by prefix), `tools` / `tool_choice` describe
  functions the model may call, and `RunResult::tool_calls` returns the calls. The types are in
  `bmoe/chat.h`, with no llama.cpp or JSON type in them. A turn that fails before it decodes (an
  overflowing or empty prompt) no longer leaves its user message in the conversation.
- **A desktop app: `bmoe-server` and a local web UI.** A second front-end next to `bmoe-cli` that
  keeps one model loaded and serves a browser UI on `127.0.0.1:8765`: chat with the reasoning
  channel, live engine metrics per token (decode rate, flash read rate, stall share, cache hit,
  memory), model management and every engine setting. The settings form is rendered from the
  parameter table alone, so a knob added to the engine appears in the UI with no UI change. The
  same server speaks an OpenAI-compatible `/v1/chat/completions` (streaming, with reasoning and
  metrics as extensions), so any OpenAI client can drive the engine; the API is in
  `docs/server-api.md`. It binds to loopback only and refuses a foreign `Host` or `Origin`.
  - Settings live in layers (server defaults, plan, launch flags, user); only the user's are
    saved, so an improved default still reaches someone who never touched that knob.
  - Tool calling (`tools`, `tool_choice`, `tool` messages, `tool_calls` in the answer, streamed or
    not), so a coding agent can drive the engine: opencode fixes a file through it in four steps.
    The server knows no tool syntax: the model's own chat template writes the tools and llama.cpp's
    chat layer parses the calls back out. Calls are not grammar-constrained.
  - The conversation is the client's: every request carries the whole history and the engine
    reuses the leading tokens its KV cache already holds, so resending a conversation with one more
    message prefills that message alone (11 000 tokens held, 22 to 167 prefilled per agent step),
    and an edited one prefills from its first differing token. `usage` reports the reused part as
    `cached_tokens`.
  - Requests wait their turn instead of being refused: a second request while one runs used to
    get `409`, which an agent that asks for a session title beside its first turn retried until
    it gave up. `429` only with 16 already waiting.
  - `stream_options.include_usage`, and `reasoning_effort: "none"` as a way to turn thinking off.
  - Models: the ggufs in the models folder (a split set listed once), a curated catalog kept as
    data in `catalog/models.json`, and resumable downloads through the system `curl`.
  - `/api/plan` offers the hardware planner's plan, with the fact behind each decision, when the
    engine carries the planner; values set by hand are never overridden. With the planner built
    in, every load plans first by default (auto mode), with the previous model unloaded so the
    probes measure the machine rather than the model already in it; a plan measured with a model
    loaded says so.
  - Release builds attach a `bmoe-desktop-<tag>-<target>` archive: the server with the UI and the
    catalog beside it. On Windows a double click starts it and opens the browser.
- **One parameter table for every engine tunable** (`bmoe/params.h`). Each `RunConfig` knob is one
  row: key, type, group, level, scope, bounds written with the same constants `validate()` checks,
  help text, and its reader and writer. Keys are the long flag without dashes, the name a planner
  decision already uses. `bmoe-cli` now parses its engine flags and prints their usage from the
  table, so a knob added there is parsed, documented and exposed to a front-end in one place.
- `bmoe-cli --describe-params` prints the table as JSON (the schema a settings form is rendered
  from); `--show-config` prints the fully resolved configuration, the equivalent flags and the
  `validate()` verdict, then exits.
- `--ngram-max-match N`: the n-gram source's longest suffix had a field and a validation rule but
  no flag.
- **`docs/backends.md`: every compute backend the pinned llama.cpp carries, and which of them this
  engine has run on.** Eighteen in the submodule, three with a measurement behind them here (CPU,
  Hexagon, Metal); the rest are listed as unknown, with what each use of a device asks of a backend.

### Changed
- **The README says what `--auto` is and how it decides.** A paragraph in Features, a quickstart
  line, and two schemas under "How it works": what happens on each token, and how a plan is
  derived from the model and the machine. The roadmap no longer lists the choice of the prefill
  device as owed, `docs/hardware-planning.md` no longer says no rule reads the compute devices,
  and `AGENTS.md` describes the llama.cpp submodule as what it is: upstream plus the one-commit
  hook `--overlap` needs.
- **`--auto` estimates what can be held, not only what is reported free, on every machine that
  publishes the two facts.** The free headroom estimate (the compressor's own ratio applied to idle
  anonymous memory) was read on Linux and Android alone, so a macOS plan was sized from the reported
  figure, which is a floor wherever a reclaim compresses. The same rule now reads the host's page
  accounting there, and applies only where a reclaim compresses. On a 16 GB Apple-silicon laptop,
  Qwen3.6-35B-A3B Q4_K_M at a 16384 context with 9.5 GB of idle memory held by another process:
  2.7 GB reported, 4.8 GB estimated, and 12.0 tok/s with a 2 GB cache where the reported figure
  left none (8.1 tok/s). With nothing else running the plan and the speed are unchanged. Measured
  with a synthetic neighbour on that one machine.
- Malformed numeric flag values are rejected (`--threads 4x` used to run with 4 threads,
  `--threads abc` with 0). Exit code 2, as for other invalid values.
- `--cache-mb` given twice takes the last value instead of failing validation when one was `auto`,
  and `BMOE_CACHE_MB=auto` now means auto-sizing (it used to parse as 0, cache off).
- The per-flag usage text is the table's help, shorter than the hand-written one; the measured
  detail stays in the docs each flag points to.
- The Android builds (`scripts/build-android.ps1`, `scripts/build-hexagon-android.sh`, CI)
  configure with `-DBMOE_BUILD_SERVER=OFF`; CI formats `server/` too and compile-checks
  `bmoe-server` on macOS.
- **CI runs on every pull request, not only on those based on `main`.** A PR stacked on another
  branch was never built until it was retargeted.

### Fixed
- **The plan's "reproduce with" line left out choices the plan had made.** It was a list of flags
  written by hand beside the parameter table, and it had no entry for the devices the plan
  restricted the run to, for the tensor placements the capacity fitter chose, or for a cache the
  plan turned off - which the CLI then sized by itself, so the line reproduced a different run. It
  is rendered from the table now: every setting that differs from the default, in the table's
  order, quoted for a POSIX shell where a value needs it. Internally, the thresholds the rules
  compared against as bare numbers have names, parameter bounds come from named constants, and
  four functions nothing called are gone; `--describe-params` and `--help` are byte-identical.
- **`--auto` gave up streaming whenever the expert cache could not be afforded.** A budget under
  the model's token cycle declined the stream and left the file mapped, on the machine where a
  mapped file is slowest. The floor now turns off the cache alone: the plan keeps `--moe-stream`
  with `--cache-mb 0` when the ledger prices the uncached stream under the mapping, and still
  declines where the drive's read rate was never measured. On a 16 GB Apple-silicon laptop,
  Qwen3.6-35B-A3B Q4_K_M at a 16384 context with about 3 GB reported available: 0.26 tok/s declined
  and mapped, 7.3 tok/s from `--auto` now. With 8.9 GB reported available the plan is unchanged
  (a 5.9 GB cache, 17.8 tok/s). Measured on that one machine.
- **`--auto` on a model split into shards measured the wrong file, and a declined plan quoted the
  cost of the plan it refused.** The storage probe read the path the caller names, which on a split
  model is the first shard and can be metadata alone: on a three-file, 85 GB DeepSeek V4 Flash it
  timed a 5 MB file and reported 101906 MiB/s. It now reads the file that holds the most expert
  bytes (`ModelProfile::expert_file`) and reports 6843 MiB/s on the same drive. And a plan that
  declines streaming for want of room is now priced as what it runs - the file left mapped, nothing
  held - instead of as the streamed allocation: that plan predicted 0.23 s/token against 1.46
  measured. Measured on a 16 GB Apple-silicon laptop, 48 tokens at a 2048 context: declined and
  mapped, 0.69 tok/s; planned with the probe corrected and 12.2 GB reported available, 2.37 tok/s
  (0.35 s/token predicted, 0.42 measured). The first run had 6.9 GB reported available and would
  still decline at that figure, so the two runs differ by the memory the machine reported as well
  as by the fix.
- **A prefill device could be armed at a cost of 0 MiB.** A device that measures as not drawing
  on the host and reports no memory of its own - an NPU fed through a kernel allocator answers so -
  was read as having its own pool and charged nothing. It is charged to the host now. Found on a
  phone, where the plan otherwise recognises the NPU on its own (40x the cores on the wide
  matmul, same result) and arms or refuses it on what the ledger has room for.
- **A device was reported as computing a wrong result when it was right.** The agreement check
  between a device and the host used a relative tolerance of 1e-4, inside the range by which two
  correct kernels differ on a 2-bit quantization (1.9e-4) and on any wide matmul (3e-4 to 4e-4).
  It is 1e-2 now; a wrong kernel is off by the size of the answer.
- **macOS: the planner, `--release-mmap` and the memory telemetry work.** Everything the engine
  asked of `/proc` and `/sys` returned nothing on Darwin, so `--auto` saw a machine with 0 MiB
  available and declined to stream, `--release-mmap` failed with "cannot read /proc/self/maps",
  and the per-token memory columns stayed empty. The mapped regions now come from
  `proc_pidinfo`, available memory and swap from the host's page accounting, the process split
  from the task's own ledger, and the core classes from `hw.perflevel*`. The gates had never been
  run on macOS; all 16 pass there now, with and without Metal.
- **macOS: an evicted expert's pages are released.** Darwin's `MADV_DONTNEED` is advice and frees
  nothing, so the process grew past its cache budget towards the whole expert set (10.6 GB
  compressed against a 4.7 GB budget). Eviction now maps fresh pages over the span.
- **A unified-memory GPU is no longer taken for one with memory of its own.** The probe that asks
  whether a device shares the host's memory compared the system's available figure around an
  allocation, and on an Apple-silicon machine gave both answers on consecutive runs; the wrong
  one applied the capacity fitter's placement and counted one pool twice. It now also watches
  this process's own footprint, which nothing else writes to.
- **The engine reported 0.27.0 in the 0.28.0 release.** The version in `CMakeLists.txt`, which
  `bmoe-cli --version` and the preamble of every metrics CSV print, was not moved with the app's
  and the changelog's; the same had happened at 0.26.0. It is 0.28.0 now, and
  `scripts/check-version.sh` compares the three: CI runs it on every pull request, and both release
  workflows run it against the tag before anything is built. Binaries built from the `v0.28.0` tag
  still print 0.27.0.

## [0.28.0] - 2026-09-29

### Changed
- **The NPU prefill reads only the experts a graph routes to.** The expert arena behind
  `--prefill-device` read every expert of every layer ahead of its routing, on the assumption that
  a wide prefill graph routes to nearly all of them. For short prompts that is not so: with top-4
  routing, a 130 to 480-token prompt routes to about half the experts of each layer of
  Qwen3.6-35B-A3B. The arena now reads a layer in two parts: ahead of its routing, the experts the
  previous graph routed at that layer (consecutive prompts route much alike); at its routing node,
  whatever the routing needs that the prediction missed, ahead of anything else queued. The
  matmul reads only routed experts, so the output is bit for bit the same. A layer routing more
  than `--prefill-routed-full` (default 0.85) of its experts gets the next layer read whole, so a
  long prompt loses nothing; `--no-prefill-routed` restores whole layers everywhere.

  Measured on a 12 GB phone with a Hexagon v81 NPU, streamed, top-4, ubatch 2048, decisions over
  Android screens (130 to 480 tokens), same session with and without, every answer identical:

  | model | experts routed per layer | whole layers | routed | |
  |---|---:|---:|---:|---:|
  | Qwen3.6-35B-A3B Q4_0 | ~50% of 256 | 7.68 s | 4.16 s | 1.85x |
  | Qwen3.6-35B-A3B Q4_K_M | ~50% of 256 | 9.95 s | 5.37 s | 1.85x |
  | Gemma 4 26B-A4B Q4_K_M | ~58% of 128 | 6.69 s | 3.70 s | 1.81x |
  | Nemotron 3.5 30B-A3B Q4_0 | ~79% of 128 | 7.42 s | 6.81 s | 1.09x |

  The gain follows how much of each layer a prompt leaves unrouted: Nemotron routes to most of
  its experts and barely gains. Gates G17f (routed == all CPU, fewer bytes read, with a slowed
  loader too) and G17g (an arena that skips its routing-node reads is caught). See
  `docs/npu-prefill.md`.
- `BMOE_DECIDE` carries the prefill device's own counters, as `BMOE_DONE` does:
  `prefill_dev_read_mib`, `prefill_dev_stall_s`, `prefill_dev_routed` and `prefill_dev_demand`.
  Before, a decision on the device reported `prefill_read_mib` 0.

### Added
- **`--decide-probe FILE` (experimental diagnostic).** Appends one JSON line per decision: the
  experts each layer routed with their router weight, and the choice logits read from every layer's
  last-token state through the model's final norm and head (a "logit lens"; for the last token it is
  exactly what a prefill cut after that layer would answer). Works on the prefill device, where
  `--route-trace` records nothing. See `docs/decide.md`.

### Docs
- The README explains decisions (`--decide`: choosing from a list with one prefill, scored over
  the whole vocabulary) in a section of their own, and its NPU prefill section carries the routed
  arena and its per-model numbers.

## [0.27.0] - 2026-09-28

### Added
- **`--decide`: choose from a list instead of generating, from one prefill and no decode.** A
  session opened with `--decide` accepts `{"cmd":"decide"}` requests: a prompt in two parts
  (`prefix`, the part that repeats from call to call, and `suffix`) and a list of `choices`. The
  answer is read from the next-token distribution after the prompt, as the log-probability of each
  choice's first token over the whole vocabulary, so a decision costs its prefill and nothing else.
  On a model streamed from flash that skips the slow part entirely. Off by default: without the
  flag the request is refused, not fatally, and nothing is allocated. See `docs/decide.md`.

  The session keeps the model state after the prefix and restores it when the next prefix extends
  it, the shape of an agent whose history grows step by step (`--decide-prefix-cache auto|on|off`,
  and `"reuse_prefix":false` per request). The state is the whole sequence state, so hybrid models
  with recurrent layers work too; a `generate()` drops it, so it holds RAM only while decisions
  follow one another. Choices that share a first token, and prompts past the context, are refused
  before anything is prefilled. A decision is always rendered with reasoning off (its answer is the
  first token, which with reasoning on would be the reasoning opener), and `--decide` without
  `--session` is a config error.

  Measured on a PC as a correctness run (Qwen3.6-35B-A3B Q4_K_M, streamed, expert cache off, times
  not a benchmark): an agent's three steps to turn on Wi-Fi were answered right at p 0.986 to 0.998;
  restoring 31 prefix tokens took 8 ms, and the kept state of this hybrid model is 63 MiB.

  Built as policy over a port (`core/src/engine/decide/`): the prompt split, choice checks, scoring
  and the kept-state policy have no llama.cpp include and are unit-tested over a scripted backend
  (`decide_policy`); one adapter drives the live context. Gate G18 checks that a restored prefix
  scores bit for bit what a fresh session computes, that decide and `perplexity()` read the same
  distribution, resident == streaming, and that a generation after a decision is unaffected.

  With `--prefill-device`, a decision is prefilled by the same rule as a chat turn (wide pieces on
  the device, a narrow tail on the CPU, weights back on the host after it), and `BMOE_DECIDE`
  reports `prefill_dev_tokens`. No prefix state is kept there (`auto` resolves to off, `on` is
  refused): llama.cpp saves a sequence through KV views that do not follow the moved model state,
  which gate G18g caught as a restored prefix scoring differently from the same prefix computed.
- **App: Choose from options.** A switch on the chat screen turns the prompt into a question and
  adds a field for options, one per line; the model picks one and each option is shown with the
  probability it put on it. The session always accepts decisions, so switching between Chat and
  Choose never reloads the model.

### Changed
- **App catalog: Nemotron-3.5-Lightning-30B-A3B is now the Q4_0 build from ggml-org** (~18.9 GB)
  instead of a third-party Q4_K_M (~25.5 GB). ggml-org publishes the reference conversions for
  llama.cpp and ships the MTP head as a separate file, which the engine does not load.
- **App catalog: Ornith-1.5-35B-A3B is no longer listed.** The architecture (`qwen35moe`) is still
  supported and streams unchanged; the model can be downloaded by URL like any other.
- `perplexity()` and `generate()` share their log-softmax, batch filling, prefill attribution and
  chat-turn rendering with `decide()` through internal headers, instead of each keeping a copy.
- The tiny test models no longer prepend a space to every text: on their byte-only vocabulary it
  made every string start with the same token.

### Fixed
- The engine reported version 0.23.0 (`bmoe-cli --version`, the `# engine=` line of the metrics
  CSV) through 0.24.0, 0.25.0 and 0.26.0: the CMake project version had not been bumped with them.
  It now matches the release.

## [0.26.0] - 2026-09-28

### Added
- **`--prefill-device`: prefill on the NPU, decode on the CPU, on a model larger than RAM.**
  Measured on a 12 GB phone with a Hexagon v81 NPU, Qwen3.6-35B-A3B Q4_0 streamed: a 1418-token
  prompt prefills in **8.2 s instead of 63.8 s (172 against 22.2 tok/s, 7.8x)**, a 1921-token one in
  11.2 s instead of 106.5 s. Short prompts do not gain (121 tokens: 9.5 against 9.95 s), because
  the device path reads the whole expert set once per graph and a short prompt is all read.

  Wide prefill graphs run on the device and decode stays on the CPU, exactly as without the flag.
  The weights are moved per graph, not per model: the scheduler runs each op where its weight
  lives and re-decides that for every graph, so rebinding a weight's buffer between graphs moves
  its ops, with no llama.cpp change. Device graphs and CPU graphs are kept to different widths so
  llama.cpp never reuses one for the other.

  With `--moe-stream` the device never holds the model: two layer-sized slots, filled from flash
  by loader threads while the device computes the other one (about 900 MB for the model above).
  The model state moves into the device's host buffer, so decode and prefill share one KV cache.
  A weight type the device's matmul refuses is carried to it in the nearest one it takes, converted
  once at load. Layer weights an op reads other than as a matmul's matrix (norms, biases, Gemma 4's
  per-expert scale) go to plain device memory, found from the ops of the capture graph: a backend
  may map its `WEIGHTS` buffers for its matmul alone (Hexagon with DMA64 does, and aborted the
  first Gemma 4 prefill on it). The buffers llama.cpp first held the model state in are handed
  back to the kernel after the move instead of doubling the KV cache (1760 MiB on Gemma 4 at an
  8192-token context). Gemma 4 26B-A4B prefills 238 tokens in 5.85 s instead of 16.2 s; at an
  8192-token context and a 2000 MiB cache it does not fit a 12 GB phone with the device path on.
  The compute-buffer reservation is redone with the weights on the device and the logit rows
  capped, which kept the CPU's buffer at 154 MB instead of 2.2 GB and decode at 3.25 tok/s (3.41
  without the flag).

  Off by default; the app has it as **Prefill on the NPU (Snapdragon only)** in its own **NPU**
  section of Settings, shown disabled with the reason on a phone without a Hexagon NPU. Needs a
  model whose expert tensors the NPU takes (Q4_0, Q4_1, Q8_0, IQ4_NL, MXFP4) and the Hexagon backend
  in the build (`scripts/build-hexagon-android.sh`). The NPU computes in fp16, so the output is not
  identical to the CPU's. A prefill device that is missing or does not open (a Snapdragon older than
  the backend's v73 floor) no longer fails the load: the engine says so and the run stays on the CPU. Gates G16 and G17 prove the placement against a loopback device on the
  host, bit for bit; that loopback RPC device is a test fixture only, and no front-end accepts an
  RPC endpoint. A ubatch narrower than `--prefill-min-tokens` is a config error (no piece could
  reach the device), and a failed expert read fails only the prefill it happened in. See
  [docs/npu-prefill.md](docs/npu-prefill.md).
- **Telemetry:** `prefill_dev_tokens`, `prefill_dev_nodes`, `prefill_dev_read_mib` and
  `prefill_dev_stall_s` in `BMOE_DONE` and the CSV trailer.

### Changed
- **llama.cpp submodule bumped** to upstream master `965f897` of 2026-09-26 (530 commits past
  `b10666`), as the one-commit fork branch `bmoe/expert-ready-hook-2609`. What it brings here is the
  Hexagon backend's K-quants: the NPU now takes a Q4_K_M, the quantisation the app's catalog ships,
  so the NPU prefill needs no special build of the model. Measured on Qwen3.6-35B-A3B Q4_K_M, 1418
  tokens: **80.6 s on the CPU, 10.0 s on the NPU (8.1x)**, with no weight converted. On the Q4_0 the
  prefill is unchanged (8.3 against 8.2 s): the device now waits on the flash, not on its own maths.
  Upstream's CPU `mul_mat_id` gained a tiled path that reads an expert before the classic loop, so
  the expert-ready hook now fires ahead of it (G4 proves it gates every read); upstream renamed the
  draft parameters' `n_past` to `pos0`.
- **`--prefill-loaders N`** (default 8) sets the arena's loader threads apart from `--io-threads`, the
  decode's read lanes; the app has it as **NPU loader threads**.
- **The release APK carries the Hexagon backend** and a DSP skel for each NPU generation it supports
  (v73, v75, v79, v81). It is built by `scripts/build-hexagon-android.sh` in upstream's Snapdragon
  toolchain image, in a CI job of its own with a read-only token, no secrets and the image pinned by
  digest; the job that signs the APK runs no third-party code. The CPU side keeps the CPU-only
  release's API level and ARM target, so decode is the same code on every phone as before.
- **A GPU-type device that cannot reach host memory stays out of a run that did not ask for it.**
  With no devices given, llama.cpp lists every GPU it finds and the context opens a backend on each;
  the engine never gives one a layer, so a device with neither a host buffer type nor buffers over
  host pointers (the Hexagon NPU) could only cost a DSP session on every Snapdragon, switch off.
  Only such devices are dropped, read off each device's capabilities: Metal, CUDA and Vulkan are
  listed exactly as before.

## [0.25.0] - 2026-09-28

### Added
- **Nemotron 3 / 3.5 MoE (`nemotron_h_moe`), the third expert layout: gate-less.** Each expert is
  up, ReLU², down, so a layer names two expert tensors (`ffn_up_exps`, `ffn_down_exps`) and the
  registry row leaves the tail slot empty, as the fused `gemma4` row does. The rest of the
  architecture sits on the resident side of the seam: a hybrid Mamba2 / attention / MoE stack
  (only the MoE blocks bind), optional latent projections the experts run between, an always-on
  shared expert, and a trailing MTP block that llama.cpp skips at load. Reference model:
  Nemotron-3.5-Lightning-30B-A3B. `make-tiny-moe.py --arch nemotron_h_moe` emits that whole shape
  in miniature, and it is a third byte-identity gate next to `qwen3moe` and `gemma4`.

  No two MoE blocks are adjacent in this architecture, so the forward predictors
  (`--predict-prefetch`, `--route-ahead`, the stale half of `--predict-log`) have no next layer
  to target and do nothing on it. The gates check that from the file and report those three
  checks as N/A there instead of passing them vacuously; their byte-identity halves still run.
- **Ornith-1.5-35B-A3B**, which is the `qwen35moe` architecture and needed no engine change.
- Both models in the Android catalog at Q4_K_M.

## [0.24.0] - 2026-09-07

### Added
- **`--release-mmap`: hand back the model file's mapping after load, which on Windows is worth
  +46% decode.** llama.cpp maps every gguf it loads and keeps the mapping for the model's
  lifetime. That is load-bearing for the streamer, which rebinds expert tensors onto the file's
  native layout, but it is not free: on Windows, while a section of a file is alive, NTFS
  serialises concurrent unbuffered reads on that file, and a lane opened while the section
  existed keeps serialising against it even after the section is gone. Four I/O lanes therefore
  deliver exactly one lane's throughput, which is why lanes and threads have always measured dead
  on the desktop host and why the engine read at roughly a third of what the drive can serve.

  Measured with `bmoe-iobench` at the engine's own request shape (576 KiB, 4 lanes, O_DIRECT) on
  an NVMe host: **2400-2660 MiB/s with no mapping, 895-930 MiB/s with one held open, 2400 again
  once the mapping is dropped and the lanes are reopened.** Confining the offsets (`--range-mb`),
  committing the destination afresh (`--fresh`) and running the lanes against a busy CPU
  (`--compute-load`) all move the number by a few per cent; the mapping moves it by 2.6x. The
  same three cells on the 12 GB Android phone are flat: f2fs does not serialise, so there is no
  read-path gain to collect there. The flag is implemented on POSIX anyway (munmap of the file's
  VMAs, inert placeholders left in their ranges), and the phone then turned out to gain for a
  different reason — see below.

  With the flag, once nothing reads through the mapping any more the engine unmaps the file,
  closes its section and reopens the reader lanes. Whether that is safe is decided by looking rather
  than by reasoning: the engine asks the OS whether any weight the capture pass observed still points
  inside a mapping of the model files, and declines if any does. One check covers every residency
  policy at once — a dense set deliberately left mmap'd, a table held back as oversized, or a tensor
  no name-based accounting could have found.

  It stays opt-in because the check answers for the pointers the capture pass saw and for no others:
  a graph shape this session never built could hold another one. That residue, plus the gguf tensors
  no policy owns once the MTP draft adds a second graph, is the remaining unknown. llama.cpp is not patched and
  still believes it owns its mapping, so what it will unmap and close at teardown is left in place
  as an inert placeholder. Off by default.

  Host A/B, Qwen3.6-35B-A3B Q4_K_M, cache 3000 MiB, 4 lanes, overlap, dense anon, 96 tokens,
  interleaved: **3.16 to 4.63 tok/s (+46%)**, flash stall per token **0.182 to 0.074 s**, per-read
  latency p50 **2.49 to 1.20 ms**, with bytes read, cache hit rate, evictions and re-reads
  identical to the digit and the generated text byte-identical.

  On the phone the flag pays too, by a different mechanism and by less. Flash stall is unchanged in
  every cell (0.087-0.093 s/token with the flag and without), exactly as the microbenchmark
  predicts, but CPU time per token falls about 9% and decode comes out 5-9% ahead: holding a 20 GB
  mapping registered is not free for a kernel already under memory pressure, and handing it back
  removes that. Prefill, model load and TTFT are unaffected. Preliminary — two 48-token cells per
  variant in both orders, against a 20% cell-to-cell spread on this device — so it is recorded as a
  direction, not a number, pending a long run.

  The demo app exposes it as **"Release the model mapping"**, enabled only under a dense policy that
  rebinds every weight into the app's own memory (Anon or Pinned) — under Mmap or Warm the engine
  looks at its own pointers and declines, so the switch would be one that silently does nothing. Off
  by default, for the same reason the flag is: the phone's number is a direction.
- **`--row-stream`: dense tables the graph only gathers rows from, served from flash.** The dense
  policy has one shape for every non-expert weight, and it is the right shape for a weight that is
  multiplied: read it whole, keep it resident. A token embedding table is not that. The graph
  gathers one row of it per decoded token out of a vocabulary of hundreds of thousands, and on a
  big model that is hundreds of MiB of RAM bought for a kilobyte of use. With this flag such a
  table is bound to reserved address space and only the rows the graph is about to read are pulled
  in, in 16 KiB slabs, inside a bounded LRU window (`--row-stream-mb`, default 64). The
  reservation mirrors the tensor's own byte layout, so ggml's address arithmetic is unchanged and
  the gather kernel is untouched: the trick the expert cache plays on `mul_mat_id`, at row
  granularity.

  Which tables qualify is not a list and not a name pattern. During the capture decode every
  reference to a dense weight is classified by how the node used it, and a table is served only if
  every reference was a row gather taking it as the table, with an index that is materialized
  before the node runs. Both are properties of the graph, so a model with tied embeddings, where
  the table is also the output head, fails the first test on that matmul and is left alone on any
  architecture without a special case; Qwen3.8-Flash-Next's 51B n-gram table is row-gathered but by
  hashes computed inside the graph, so it fails the second and keeps the size guard measured for it
  in 0.22.0. `IRowSource::materialize()` is the fallback if some later graph reads a served table
  any other way: the whole table is pulled in and the run says so on stderr.

  Measured, byte-identical to the resident reference in every cell. On the 12 GB test phone via
  adb: Qwen3.8-Flash-Next UD-IQ3_XXS, pinned dense **4313 to 3816 MiB** (-497), and
  Qwen3.6-35B-A3B UD-Q3_K_XL, pinned dense **2436 to 1921 MiB** (-515), for 0.3 MiB resident and
  0.4 MiB of flash read across the generation, against 4.5 GB of expert reads on the same run.
  Expert bytes, cache hit rate, evictions and re-reads are identical to the digit with the flag on
  and off. Throughput is neutral so far: interleaved host cells on Qwen3.6-35B-A3B Q4_K_M gave 2.22
  and 2.30 tok/s off against 2.27 and 2.31 on, a spread smaller than the one between the two
  baselines, so the flag ships off by default and the claim it makes is about RAM, not speed. The
  RAM is the point: on the model whose 4.3 GB pinned dense set is invisible to the kernel's reclaim
  accounting, half a gigabyte handed back goes straight to the cause of the 2 to 7 second tokens
  documented in 0.22.0. Gates `G15a`/`G15b` prove identity on all three tiny models, the second
  with a window of one slab so nearly every gather re-reads what the last one handed back. New
  port `bmoe/row_source.h`, adapter `core/src/moe/row_stream.cpp`, docs in
  `docs/row-gathered-tables.md`, app switch **Stream row-gathered tables**.
- **`--expert-substitute L`: cache-aware expert substitution (experimental).** Before a decode
  routing is committed, every expert already in the LRU cache gets its score raised by `L` times this token's
  score range and the top-k is taken again, so a resident expert takes a slot only when the router
  scored it within that margin of the one it displaces. The same number of experts runs; fewer of
  them cost a flash read, and the weights are the router's own. The mechanism is the
  cache-conditional rerouting of Skliar et al. (arXiv:2412.00099) with the cache in front of flash
  instead of DRAM and the range taken per token, so there is no calibration state. The scores are
  read from the tensor the graph itself sorted, which is exact for any gating function and alive at
  the callback by construction; the gate input is not (on Gemma 4 the allocator had recycled it),
  and a new gate cell caught that. Measured on the desktop on Qwen3.6-35B at `0.15`: 258 → 119 MiB
  of flash per token, 2.37 → 3.84 tok/s, perplexity up 1 to 4 % on two held-out texts and
  tinyMMLU 88 → 84 of 100 (94 identical predictions), HumanEval (first 50) 42 → 42 with 69 instead
  of 292 MiB of flash per token and 2.36 → 5.53 tok/s across a warm session; `0.30` costs 25 % in
  perplexity, `0.60`
  destroys the model (perplexity 31) while its prose still reads well. Experimental: one model,
  desktop numbers, the on-device A/B still owed. Off by default, decode only, refused without a cache and outside `[0, 1]`. The run summary, the CSV
  (`experts_reranked`, `experts_substituted`, `substitute_lambda`) and the app's metrics view carry
  its actual bite. See [docs/cache-aware-substitution.md](docs/cache-aware-substitution.md).
- **`--ppl FILE`: teacher-forced perplexity, and `--ppl-step` for the decode regime.** Scores a
  fixed text instead of generating one, with the mean NLL, the perplexity and the next-token hit
  rate, so a lossy setting gets a scale instead of a coin flip: greedy output only moves when a
  perturbation crosses an argmax boundary, whatever its size. `--ppl-step` scores one token per
  decode. A wide batch routes every position of a layer before reading any of it, so a policy that
  consults the cache barely fires there (1 to 3 % of the slots it examined, against 26 to 28 % in
  decode) and the number says nothing about it; stepping is the regime the policy acts in. Both
  policies report what they did (`ppl-policy:`), so an inert flag cannot pass unnoticed.
  `--ppl-list FILE` scores many texts in one session and `--ppl-choices A,B,...` reports each
  choice's log-probability after the text, which is what a multiple-choice benchmark compares;
  `scripts/tinymmlu-bench.py` runs tinyMMLU on top of them, one cell per setting, and
  `scripts/humaneval-bench.py` runs HumanEval over `--session` and grades the completions.
- **"Prefer cached experts" in the app**, under Speed / quality → Experimental, as a percentage of
  the score range (10 to 30, default off, disabled with the cache off), with a warning from 20 % up.
  Listed under the app's Experimental group.
- **Qwen3.8-Flash-Next Q2_K in the app catalog**, next to the UD-IQ3_XXS. Its dense side is at
  2-4 bit: 2.4 GB pinned instead of 4.3, on a phone where that set is walked every token and is what
  caps the expert cache. A plain `llama-quantize` build without an importance matrix, so the experts
  are coarser than the UD file's; the row says so. Six shards, ~80 GB, downloaded and resumed like
  the other sharded entries. Every published dynamic quant of this model keeps the dense side at
  5-8 bit whatever its overall size; this is the one file that does not.

### Changed
- **Every run says which mode it ran in, and `--moe-stream` now brings a cache with it.** A first
  command with nothing but `-m`, `-p` and `-t` ran plain llama.cpp on mmap: no streaming, no cache,
  and a dense policy that only applies once streaming is on. The report said none of this, because
  the `moe-stream:` block only prints when streaming is enabled, so a baseline run read as a
  measurement of this engine. Two changes: a `mode:` line is printed unconditionally, naming the
  flag when a MoE model ran without streaming, and `--cache-mb` defaults to `auto` whenever
  `--moe-stream` is on, since the previous default of 0 meant the cache was off. On a 16 GB host
  streaming Qwen3.6-35B-A3B Q4_K_M, the cache default alone is 1.16 to 2.35 tok/s and 585 to 238
  MiB read per token, same output. An explicit `--cache-mb` or `BMOE_CACHE_MB` still wins,
  including an explicit 0, and the library's own default is unchanged: the CLI resolves this, so an
  embedder passing 0 still means no cache. Reported by @eiffel31 (#186).
- The CSV summary trailer gains `row_table_MiB`, `row_resident_MiB`, `row_rows`, `row_reads`,
  `row_read_MiB`, `row_evictions` and `row_io_errors`, and a `moe-rows:` end-of-run line appears
  when a table qualified. All are absent from the per-token rows, which the policy does not touch.
- `--cache-mb auto` no longer reserves the size of a row-streamed table for a conversion that will
  not happen. It deducts the window instead, so the cache gets the RAM the policy freed rather than
  the engine planning for both.
- **macOS reads uncached (`F_NOCACHE`), and `o_direct` reports the open's real outcome everywhere.**
  A direct request on Apple applies `fcntl(F_NOCACHE, 1)` to every reader descriptor — the kernel
  stops caching that file's pages — instead of silently returning a buffered fd (Apple has no
  `O_DIRECT`). It is a caching hint, not an I/O mode: no alignment contract, no DMA promise, so a
  direct reader on Apple keeps ordinary `pread` semantics and skips the O_DIRECT bounce path rather
  than inheriting Linux-only alignment requirements. Independently, the `o_direct` telemetry field
  (CSV preamble, decode-trace header, streaming banner) now comes from what the shard opens
  achieved — the AND across shards, after every platform refusal and open-time downgrade — instead
  of from the requested configuration, so a run served buffered says `0` on every platform. Host
  measurements in the PR (#179).

### Fixed
- **A tied output head left half the biggest dense weight reading the model's mmap.** When a gguf
  carries no `output.weight`, llama.cpp builds the output head from the token embedding table, and
  the model then holds **two** `ggml_tensor` objects, identically named, over the same file bytes.
  The capture pass recorded weights in a map keyed by name, so it kept one of them, and
  `--dense-weights anon` / `ahwb` rebound that one: the twin went on reading the mmap for the whole
  run. On a model past RAM that is the output projection — a tensor whose per-token cost rivals all
  the routed experts together — served by page faults from flash, which is precisely what those two
  policies exist to prevent. Gemma is in this case, and so is any architecture that ties its
  embeddings.

  The capture now records every distinct leaf object, deduplicated by address, and the dense policy
  folds tensors over one file range into a single entry with an alias list, rebinding them all onto
  the same buffer. Bytes are still read once and memory is still allocated once — on the gemma4
  gate, the same 70 anon buffers as before — and every byte-identity gate passes unchanged. The
  same fix is what makes the mapping releasable on those models rather than a crash.

  What this is worth in throughput is **not measured**. The mechanism is certain, the size of it is
  not: it depends on how much of the mapped table the kernel was still holding, which on a phone
  under reclaim is a different story from a desktop with room to spare. Gemma-4-26B on the test
  phone is the cell that would price it, and it is owed.
- A sharded catalog entry (DeepSeek V4 Flash, Qwen3.8-Flash-Next) read as on-device as soon as its
  first shard landed, which is the smallest file of the set and arrives seconds into the download:
  the row lost its progress bar, a Run on the incomplete set failed at load, and if the download
  chain died there was no Download button left to resume it. The legacy-merged-file rule, meant for
  a single-file gpt-oss from an earlier release, now applies only when the entry's file name is not
  one of its shards. The app module gains its first JVM unit test for the rule, run in CI.
- `bench-report.sh` measured the model with `du` on the path as given, so a Hugging Face cache
  symlink reported 0.0 GiB and a split model counted only the shard on the command line; sizes now
  dereference (`stat -L`) and sum every sibling shard. The same raw-path size fed the storage
  probe's offset, and the probe trusted `dd`'s exit code while assuming a full 1 GiB was read: on
  a short read, or a filesystem that takes `iflag=direct` without honouring it, that printed rates
  no drive can do (110 GiB/s in the first community report, #192). The rate now comes from the
  byte count in `dd`'s own summary, and a physically impossible result prints `n/a` with a note
  instead of a number. (#193)

## [0.23.0] - 2026-08-29

### Fixed
- **Windows `bmoe-cli.exe` looked broken when double-clicked (#181).** A console program started
  from Explorer gets a console of its own, prints its usage because no model was given, exits,
  and the console vanishes with it: a window that flashes and disappears, indistinguishable from a
  crash. Now, when the console was created for this process alone, the no-model path says it is a
  command-line program and waits for Enter before closing; from a terminal nothing changes. The
  release archive's README.txt opens with the same fact and a complete Windows command, and no
  longer points a Windows user at a bash script as the only instruction. The Windows build also
  links the MSVC runtime statically, so the exe no longer needs the VC++ Redistributable to start.

### Added
- **Prefill-phase attribution in `BMOE_DONE` and the CSV `# summary`.** `prefill_cpu_s` /
  `prefill_read_mib` / `prefill_io_s` / `prefill_stall_s` / `prefill_mgmt_s`: the prompt phase's
  own split of the same cumulative counters the decode fields come from, as session-level deltas
  across the prefill chunks — the streamer is untouched. On a >RAM model the prompt is what the
  user actually waits for before the first token, and until now the phase carried a single bare
  wall number (`prefill_s`) with no compute/flash split; these keys are what settles which of
  the two binds at a given prompt length (#173). The CSV `# summary` trailer carries the same
  five raw fields under the same names and units — appended keys, so existing readers ignore
  them. Host measurements in the PR.

## [0.22.0] - 2026-08-28

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
  (~82 GB on disk). The README hero clip is a later in-app run of the same file: 2.03 tok/s over
  a 72-token answer, cache 1000 MiB, cold experts dropped at 100 %, 54 % cache hit, real time.
  On the final pin (`b10666`) three in-app runs of that prompt averaged 2.5-2.6 tok/s (second-half
  median 2.6-2.7), and the same argv over `adb shell` produced byte-identical output at 2.2 tok/s
  under the shell's lower CPU frequency cap. The one slow token each run shows (2-7 s, tens of
  thousands of major faults, RSS falling and zram swap rising by the same 200-500 MiB) is the
  kernel reclaiming the anonymous expert cache under the ~0.9 GB the pinned dense set leaves
  free; it does not appear over adb, where the app's own footprint is absent and 1.3 GB stays
  free. Freeing dense RAM (requantizing the dense side offline) is the lever, not the cache.
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

### Added
- **Community benchmarks.** `scripts/bench-report.sh MODEL.gguf` runs the fixed README protocol on
  any Linux/macOS host (256 greedy tokens, the reference prompt, auto cache, 4 lanes, overlap,
  dense weights out of the page cache), records CPU / RAM / drive and the drive's measured
  O_DIRECT rate at 512 KiB requests from the model file itself, and prints the two markdown tables
  a report needs, every column read from the CSV `# summary` trailer by name. A `benchmark-report`
  issue form collects them and [docs/community-benchmarks.md](docs/community-benchmarks.md) holds
  the protocol, the hardware wanted and the results table, seeded with the README rows. Any
  supported MoE in any quantization is a row; the catalog models are listed as the ones that line
  up with the README, not as a requirement.
- **Prebuilt `bmoe-cli` on every release.** A `release-host` workflow builds a static, portable
  CLI for Linux x86_64 / aarch64, macOS arm64 and Windows x86_64 from a clean checkout of the tag
  and attaches the archives to the release, so a benchmark contributor downloads one file and runs
  `BMOE_CLI=... scripts/bench-report.sh` with no toolchain. Portability over speed: `GGML_NATIVE=OFF`,
  x86_64 assumes AVX2, aarch64 assumes armv8.2-a+dotprod+fp16; anything older builds from source.

### Changed
- **llama.cpp submodule bumped** to upstream `4e97ac8` (tag `b10666`), the first master with
  [ggml-org/llama.cpp#27742](https://github.com/ggml-org/llama.cpp/pull/27742) (Qwen3.8-Flash-Next
  support) merged, with the expert-ready hook rebased on top, still a single-commit delta over
  stock upstream. The recipe was written against the PR head and needed nothing changed for the
  merged form: same `qwen4exp` architecture string, same expert suffixes, same
  `per_layer_token_embd` tensor, and the Unsloth ggufs the catalog points at are the files
  converted on release day. Byte-identity gates pass on the new base. App version 0.22.0
  (versionCode 37).
- **Telemetry attribution stops calling unmeasured runtime "compute".** Overlap `stall` is now the
  **union of stalled intervals** — the cumulative wall time during which at least one compute thread
  was blocked on a streamed expert — instead of the summed per-thread block time divided by the
  thread count, a mean that understated the stall whenever a minority of threads did the waiting and
  quietly dumped the difference into the `compute_ms` residual. The residual `compute_ms` /
  `compute_s_tok` fields keep their existing meaning for protocol compatibility. The app panel now
  draws four bars — compute (process CPU time over the compute threads, an attribution proxy),
  flash wait (measured `io`/`stall`, in the end-of-run summary too: `io_s_tok`/`stall_s_tok` instead
  of inverting the clamped residual), cache mgmt, and **unattributed** (the off-CPU wall time — zram,
  preemption, faults — the residual used to absorb).
- **Compatibility note:** under `--overlap`, `stall_ms` and `stall_s_tok` changed meaning — they are
  the critical-path union now — so these columns are **not comparable with CSVs produced by older
  releases**; compare them only within one engine version. Fixes #98.
- **The benchmark docs are a guide now, not a lab notebook.**
  [`community-benchmarks.md`](docs/community-benchmarks.md) opens with a "start here" for the three
  cases a contributor is actually in — PC or laptop, Android phone, Apple hardware — each with the
  command and what to paste back, plus the settings-override table that used to be one buried
  sentence and an explicit adb protocol for phones. "Hardware we want to see" moved to the end and
  became open questions: as the second section it read as a shopping list, when the point is that
  any hardware is a useful row. [`benchmark-method.md`](docs/benchmark-method.md) drops the
  reference device from its opening (named once at the end, as the provenance of the published
  numbers) and gains the section it was missing: what each knob does, when to move it, and which
  telemetry field says whether moving it worked. The issue form asks for the RAM regime explicitly.
- **The community protocol pins `--ubatch 512`**, which the Android app has always done and
  `scripts/bench-report.sh` never did. Prefill width costs resident memory (320 MiB reserved at a
  2048 context against 80 MiB at 512) and every reserved MiB is one the expert cache does not get,
  so a host row was quietly running a different configuration from the app it is compared against.
  Override with `UBATCH=`; `UBATCH=0` restores the old full-width behaviour. The five maintainer
  rows in the table predate this and are marked as such.
- **The app path says which settings to change.** An in-app row was being invited without saying
  that the app ships cold-expert dropping at 75 %, a lossy speedup the CLI protocol does not use, so
  a telemetry screenshot was not the configuration it was being compared against. The page now names
  the three settings to match (dropping off, cache auto, context 2048) and says that a phone row
  cannot fill the storage-rate column.
- **Two platform limits written down** in [`limitations.md`](docs/limitations.md). macOS has no
  `O_DIRECT` and the engine does not call the `F_NOCACHE` equivalent, so expert reads there go
  through the page cache — while the metrics still say `o_direct=1`, because that field records the
  requested configuration rather than what the open did. And there is no iOS target, so there is no
  supported way to run the engine on an iPhone or iPad. Both were already true and undocumented.

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
  Measured on the 12 GB test phone (indicative 96-token run, not yet the full 256-token protocol); see
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
  test phone — **5.2× device RAM** — at up to 7.7× a plain-mmap load (top-k 2). A top-k × lanes ×
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
