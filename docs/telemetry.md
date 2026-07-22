# Telemetry contract

With `--progress`, `bmoe-cli` emits machine-readable lines the Android app (and any other
consumer) parses. The format is versioned by this document; keep it stable.

## Per-token lines

Emitted once per generated token, in order:

```
BMOE_LOAD {"mb":<float>,"ms":<float>}
BMOE_PROGRESS {"step":<int>,"steps":<int>,"wall_ms":<float>,"io_ms":<float>,
               "compute_ms":<float>,"mgmt_ms":<float>,"stall_ms":<float>,"read_mb":<float>,
               "cache_hit_pct":<float>,"majflt":<int>,"cpu_ms":<float>,"dense_resident_frac":<float>,
               "reasoning":"<string>","text":"<string>"}
```

- `BMOE_LOAD` appears only when experts were read this token; `mb` is the flash bytes read,
  `ms` the read time.
- `BMOE_PROGRESS.step`/`steps` are 1-based index and target token counts.
- `read_mb` is the flash bytes read this token; `stall_ms` is the overlap-only wall time compute
  lost to reads (0 in serial mode).
- `wall_ms` = total token time; `io_ms` = flash read time. `compute_ms` is a **residual, not a
  measured quantity**: no clock runs around llama.cpp's matmul kernels in a normal run, so compute
  is whatever wall time is left after the measured terms are subtracted — `wall_ms − io_ms −
  mgmt_ms` in serial, `wall_ms − stall_ms − mgmt_ms` under overlap. When that residual is the
  number in question, `--compute-trace` measures it directly instead (see [Decode
  traces](#decode-traces)) — at a cost that makes it a diagnostic, not telemetry.
  `compute_ms` is **clamped at 0**: the subtraction can go slightly negative under overlap (where
  `stall_ms` is a per-thread mean, not a critical path), and a negative compute would be nonsense.
  That clamp means the wall-additive identity is not exact in the pathological case — a consumer
  that recovers the flash-wait term as `wall_ms − compute_ms − mgmt_ms` gets `wall_ms − mgmt_ms`
  when the clamp fires, over-attributing to flash. Read the wall-additive flash term straight from
  `io_ms` (serial) / `stall_ms` (overlap) instead of inverting the residual.
  In serial mode `io_ms` is the wall time blocked on reads (a subset of `wall_ms`). Under
  `--overlap` its meaning changes: it is the **sum of per-lane busy time**, so it can exceed
  `wall_ms` because lanes read in parallel with compute. Use `stall_ms` for the wall time
  compute actually lost to reads under overlap.
  - **`stall_ms` has a structural floor above zero** — overlap cannot hide *all* flash. Which experts
    a token needs is only known once the router gate runs, immediately before the FFN consumes them,
    so on a cache miss the first missed expert's read is issued *after* routing and the FFN waits for
    it (overlap still hides experts 2…k of that layer behind expert 1's compute). The residual stall
    therefore tracks the miss rate: it approaches zero only at ~100 % cache hit (the whole expert set
    resident, i.e. no streaming) or with perfect speculative `--prefetch`. Empirically it never
    reaches zero — measured per-token minimum ≈ 5–15 ms at 76–81 % hit (Qwen/Gemma), rising to
    hundreds of ms at 12–18 % hit (gpt-oss). A run that reads as "compute-bound" once warm is the
    *expected* success case: streaming has hidden the bulk of I/O, leaving compute as the bottleneck.
- `mgmt_ms` (not emitted in the JSON, but folded into the `compute_ms` residual above and written
  to the CSV) is the cache-management time this token: virtual-memory commit of newly cached
  pages, eviction of cold pages, and the LRU bookkeeping. On the first few tokens after prefill it
  can be a large share of the token; at steady state it is near zero. Surfacing it stops the "all
  compute" reading on warm-up tokens where the real cost is cache churn, not matmul.
- `cache_hit_pct` is the cumulative cache hit rate, or `-1` when no cache is used.
  **Under [`--drop-cold-experts`](expert-dropping.md) read it with care:** a dropped routing is a
  miss that is never looked up, so it leaves both sides of the ratio and the reported hit rate
  rises without the cache having served anything more. Compare runs at the same drop rate, or read
  `experts_dropped` next to it.
- `majflt` / `cpu_ms` **decompose the `compute_ms` residual** — the whole point being that "compute"
  above is a catch-all that silently absorbs page faults and scheduler stalls, not just matmul.
  They are measured directly around `llama_decode` (no submodule patch needed): `majflt` is the
  major page faults served this token — a non-zero count means a mmap-resident (dense) weight was
  re-faulted from flash *inside* the decode, i.e. a >RAM residency stall masquerading as compute.
  `cpu_ms` is CPU time summed across all threads; compare it to `wall_ms × threads` for occupancy —
  near 100% is genuinely compute-bound, well below means the cores were throttled, preempted, or
  blocked (a low-clock frequency cap or a co-resident process), not doing more math. Both are `0`
  when the platform can't report them (the Windows host build); treat `0` as "unmeasured".
- `dense_resident_frac` is the sampled fraction of the DENSE (non-expert) weights still in RAM (by
  `mincore`, throttled). Under `--dense-weights anon` it samples our own buffers (is zram holding
  them?); under mmap/warm the model's mmap (is the kernel dropping it?). A diagnostic read alongside
  `majflt` — nothing acts on it. `-1` when unmeasured.
- `text` is the full generated **answer** so far, JSON-escaped (for streaming into a UI), with any
  reasoning span stripped out.
- `reasoning` is the thinking span so far, JSON-escaped, when a reasoning model's chat template
  separated it from the answer. Empty with chat off, on a non-reasoning model, or when thinking was
  disabled (`think=false`). It is display-only and kept apart from `text` so a UI can render it as a
  distinct thinking block rather than inline in the answer.

## End-of-run lines

```
=== answer ===
<full generated text>
=== perf ===
generation: <n> tokens, <s> s/token (<t> tok/s)
compute: <pct>% CPU occupancy (<c> cpu-s/token over <n> threads), <f> major faults/token
moe-stream: read <mib> MiB (<mib/tok> MiB/token), decode <s> s/token (compute <c> + cache mgmt <m> + flash I/O <i> s/token, <bw> MiB/s)
moe-cache: <pct>% hit, resident <mib> MiB
```

The `compute:` line decomposes the residual: low CPU occupancy points at a throttled/preempted
core (a frequency cap, a co-resident process) rather than heavy math, and non-zero major
faults/token means dense weights were re-faulting from flash inside the decode. It is omitted on
platforms that can't measure it (the Windows host build).

`compute` in the `moe-stream:` line is the same residual described for `compute_ms` above; `cache
mgmt` is the per-token mean of `mgmt_ms`.

With `--prefetch K` a `moe-prefetch:` line is added:

```
moe-prefetch: <mib> MiB speculative, <useful>/<prefetched> experts useful (<pct>%)
```

`<mib>` is the flash read done speculatively this generation (a subset of the total read),
`<prefetched>` the experts fully read ahead, and `<useful>` how many of those a later routing
actually hit. See [prefetch.md](prefetch.md).

With `--drop-cold-experts F` a `moe-drop:` line is added:

```
moe-drop: <dropped>/<routed> routed experts dropped (<pct>%), threshold <F> x uniform
```

The flag fixes a *threshold*, not a rate: how much is actually discarded depends on what the cache
held, so this line — not the flag — is what a run traded. See
[expert-dropping.md](expert-dropping.md).

Under `--overlap` the `moe-stream:` line additionally reports `stall_s/tok=<s>` — the mean
wall time per token that compute threads waited for expert reads to complete. It is `0` in
serial mode (where the read wait is already folded into decode time).

`moe-cache:` is present only when a cache is active. The `=== answer ===` / `=== perf ===`
banners appear only in `--progress` mode; without it the CLI streams the answer inline and
prints just the summary lines.

## CSV sink

`--csv PATH` additionally writes one row per token:

```
step,steps,wall_ms,io_ms,compute_ms,read_bytes,cache_hit_pct,stall_ms,mgmt_ms,majflt,cpu_ms,
dense_resident_frac,turn,majflt_mib,cache_budget_mib,rss_mib,rss_anon_mib,rss_file_mib,swap_mib,
mem_available_mib,mem_free_mib,swap_free_mib
```

followed by a `# summary ...` comment line. Intended for the benchmark sweep.

`stall_ms`, `mgmt_ms`, `majflt`, `cpu_ms` and `dense_resident_frac` are trailing columns appended
after the original seven. `stall_ms` is the wall time compute threads waited for expert reads that
token (`0` in serial mode); `mgmt_ms` is the cache-management time described above (plus the throttled
dense-residency probe); `majflt`/`cpu_ms` are the fault + CPU-time decomposition of the compute
residual (see the `BMOE_PROGRESS` notes above), `0` when unmeasured; `dense_resident_frac` is the
sampled dense-weight residency, `-1` when unmeasured. All are additive: older CSVs have fewer columns,
so consumers must read by column NAME (from the header row) and treat any as optional. The `# summary`
line likewise gains `stall_s/tok=<s>`, `mgmt_s/tok=<s>`, `majflt/tok=<f>`, `cpu_s/tok=<s>`,
`token_demand_MiB=<f>` (the expert bytes one token routes, measured — where cache hits start, NOT a
floor to defend; see [pressure.md](pressure.md)), `experts_routed=<n>` / `experts_dropped=<n>` (what
[cache-aware dropping](expert-dropping.md) actually discarded during generation — the flag sets a
threshold, not a rate, so this is the only record of the trade a run made) and
`layer_demand_MiB=<f>` (the widest layer's routed
bytes: the mechanical floor the cache must be able to stage); see the `io_ms` note above for how the
read-time columns are reinterpreted under overlap.

The trailing block is the memory picture, added so a run can be diagnosed from its own file:

| column | meaning |
| --- | --- |
| `turn` | which `generate()` this token belongs to (0 for a one-shot run). A session CSV spans every turn; without this the two-turn shape — a fast turn, an idle, then the turn that pays for it — is unreadable. |
| `majflt_mib` | what those faults moved: `majflt` x page size. The same fact as the count, in the unit the rest of the row uses — directly comparable to `read_bytes`, i.e. the reads we chose against the reads the kernel forced on us. |
| `cache_budget_mib` | the expert-cache budget in effect. Fixed for the run now (an explicit `--cache-mb`, or what `auto` sized to once at load) — the runtime governor that moved it is retired. |
| `rss_anon_mib` | resident anonymous memory — **the expert cache lives here** (and, under `--dense-weights anon`, the dense buffers). Falling while `cache_budget_mib` stays put means the kernel is taking it. |
| `rss_file_mib` | resident file-backed memory — the mmap'd model. Reclaimed by being dropped, not swapped, so it never shows in `swap_mib`. |
| `dense_resident_frac` | fraction of the DENSE (non-expert) weights still in RAM, by `mincore` (throttled). Under `--dense-weights anon` it samples our own buffers — is zram holding them? Under mmap/warm the model's VMAs (`/proc/self/maps`) — is the kernel dropping the model? Read with `majflt`. `-1` when unmeasured. |
| `swap_mib` | anonymous memory already lost to zram (`VmSwap`). |
| `rss_mib` | total resident (`VmRSS`). |
| `mem_available_mib` / `mem_free_mib` / `swap_free_mib` | what the device claims about itself. `MemAvailable` counts this process's own mmap'd weights as reclaimable, so it over-states headroom — it is recorded next to what we measured ourselves because the gap between them is the story. |

All are `0` where the platform cannot report them (the Windows host build reports device memory but not the per-process split).

## Route trace

`--route-trace PATH` writes the per-step, per-layer MoE routing trace. Everything above answers
*how long* a token took; this answers *what the router asked for* — which experts each layer
routed, how strongly, and whether they were already resident. It needs `--moe-stream` (without
streaming there is no routing to observe) and it is a **diagnostic, not telemetry**: capturing it
asks the compute graph for extra nodes, which adds a barrier per MoE layer, and it writes a row
per routed expert. **A traced run is not a benchmark run** — the numbers in the `--csv` of a
traced run are slower than the real thing, and `mgmt_ms` in particular shifts, because settling
speculative prefetch moves outside the window that times it.

Columns are **append-only** within `v1`, like the metrics CSV: `dropped` was added after
`expert_bytes`, so consumers must read by column NAME and treat any column as optional rather than
indexing by position.

The file is long format: a `#` preamble carrying the run's static facts, then one row per routed
expert. Conceptually it is a matrix — rows are steps, columns are layers — and a **cell** is the
`n_expert_used` rows sharing `(turn, phase, step, layer)`.

```
# route_trace v1
# model=<path> arch=<string> n_layer=<int> n_expert=<int> n_expert_used=<int>
# layer=<int> expert_bytes=<int> dense_bytes=<int>        (one per layer)
turn,phase,step,layer,slot,expert,weight,residency,expert_bytes,dropped
```

| column | meaning |
| --- | --- |
| `turn` | session-mode turn; `0` for a one-shot run. One file per run, appended across turns. |
| `phase` | `0` = prefill (one batched decode over many tokens), `1` = decode (one token per step). |
| `step` | absolute context position of the token being routed, so prefill and decode share one axis. |
| `layer` | MoE layer. Dense layers never appear. |
| `slot` | `0..n_expert_used-1`, the router's rank order — slot 0 is its top choice. |
| `expert` | **the routed expert id**: the cell's payload, and what every reuse question is asked of. |
| `weight` | the final applied routing weight, after whatever softmax/normalise/scale the architecture uses. `nan` when the graph exposed no weight node — "unknown", never `0`. |
| `residency` | `0` = miss (this routing reads from flash), `1` = hit, `2` = hit on a speculative prefetch's first touch. |
| `expert_bytes` | flash bytes this routing reads; `0` unless `residency=0`. |
| `dropped` | `1` when [cache-aware dropping](expert-dropping.md) discarded this routing — a miss weighted below the threshold, never read, weight zeroed. Always `0` with `--drop-cold-experts` off. |

`(turn, phase, step, layer, slot)` is unique. Two asymmetries are deliberate:

- **`residency` is per routing, `expert_bytes` is per read.** During prefill many tokens of one
  batch may route the same expert; the streamer reads it once, so only the first row carries the
  bytes while every row keeps `residency=0` — each of those routings *did* face a cold cache.
  Summing `residency==0` therefore over-counts misses versus `cache_hit_pct` in prefill; summing
  `expert_bytes` is right. In decode (one token per step) the question does not arise.
- **`dense_bytes` is static, `expert_bytes` is not.** Dense weights are mmap-resident and never
  streamed, so there is nothing to measure per step: `dense_bytes` is what a cold layer costs to
  page in, stated once. Per-layer *I/O time* is absent for the same kind of reason — under
  `--overlap` reads complete asynchronously, so any per-layer timing would be fiction.
- **`weight` and `residency` describe the router; `dropped` describes the policy.** When dropping is
  on, a discarded routing keeps the weight the router gave it and the residency it faced — the trace
  records the routing that was *chosen* — while `expert_bytes` falls to `0`, because a dropped
  expert is never read. Summing `expert_bytes` therefore still measures real flash traffic, and
  `dropped` is what explains the gap against `residency==0`.

**The last layer has only one prefill step, and that is real.** Before the final layer's FFN,
llama.cpp gathers only the tokens whose logits were asked for (`inp_out_ids`; see `il == n_layer
- 1` in `third_party/llama.cpp/src/models/*.cpp`). The engine asks for the last token only, so
during prefill the last MoE layer routes exactly one token while every other layer routes the
whole prompt. The trace reports this faithfully — that layer's row carries the *final* prompt
position, not position 0 — so do not read the gap as lost rows. It also means a long prompt warms
every layer's experts except the last one's.

A `step` below zero would mean a row that could not be attributed to a position (more than one
output token in a batch). The CLI's greedy loops never produce one.

Join it to `--csv` on `step` (subtracting the prompt length from the trace's `step` for the
decode phase) to put per-token wall time next to what was routed.

`scripts/route-analyze.py` reads the file — stdlib only, nothing to install:

```
python scripts/route-analyze.py trace.csv                        # the default view set
python scripts/route-analyze.py trace.csv --view matrix --steps 0-15 --layers 0-11
python scripts/route-analyze.py trace.csv --view reuse           # reuse distance -> cache policy
```

Size: roughly `steps x moe_layers x n_expert_used` rows — ~200k rows (~8 MiB) for a 500-token
decode on a 48-layer, top-8 model. Prefill adds a row per prompt token, so a long prompt
dominates the file; `--phase decode` is the usual lens.

Real traces from Qwen3-30B-A3B, Gemma-4-26B-A4B and gpt-oss-120b on device, with the analysis they
support, are archived in
[bench-data/2026-07-15-route-trace/](bench-data/2026-07-15-route-trace/findings.md).

## Session mode

With `--session`, `bmoe-cli` keeps the model loaded and the expert cache warm across prompts
instead of exiting after one generation (see [session.md](session.md)). Requests arrive as one
JSON object per line on **stdin**; responses interleave control lines with the same per-token
lines above on **stdout**. The control lines are also `BMOE_<TAG> {json}`, so a per-token parser
extends to them naturally.

Requests (stdin):

```
{"cmd":"generate","id":<int>,"prompt":"<string>","n_predict":<int>,"think":<bool>,"clear_kv":<bool>}
{"cmd":"cancel"}          # interrupt the in-flight generation; the session stays loaded
{"cmd":"close"}           # end the session (EOF on stdin does the same)
```

`prompt` is JSON-escaped (newlines as `\n`); `n_predict`/`think`/`clear_kv` are optional and
default to the process's flags / `true`. `clear_kv:true` starts a **new chat** (drops the KV and
the engine-held conversation); `clear_kv:false` **continues** the conversation — send only the new
user message, the engine re-renders the whole history and reuses the KV prefix (see
[session.md](session.md)). `cancel` may arrive at any time, including mid-generation.

Responses (stdout):

```
BMOE_READY {"load_s":<float>,"arch":"<string>","n_ctx":<int>,
            "think_ctl":"template|prefill|none"}                        # once, after the model loads
BMOE_BEGIN {"id":<int>}                                                # a generation started
BMOE_LOAD / BMOE_PROGRESS ...                                          # per token, as above
BMOE_DONE  {"id":<int>,"cancelled":<bool>,"tokens":<int>,"tok_s":<float>,
            "prefill_s":<float>,"prefill_tps":<float>,"load_s":<float>,"cache_hit_pct":<float>,
            "n_prompt":<int>,"n_past":<int>,"compute_s_tok":<float>,"io_s_tok":<float>,
            "cache_resident_mib":<float>,"cache_budget_mib":<float>,"read_mib":<float>,
            "stall_s_tok":<float>,"mgmt_s_tok":<float>,"majflt_tok":<float>,"cpu_s_tok":<float>,
            "token_demand_mib":<float>,"reasoning":"<string>","text":"<string>"}
BMOE_ERROR {"id":<int>,"fatal":<bool>,"msg":"<string>"}
```

`BMOE_READY`'s `think_ctl` states how a `"think":false` request can be honoured on the model that
was just loaded, so a UI need not offer a control that does nothing. It is decided by rendering the
model's own chat template, not from a list of model names:

| value | meaning | what the UI should do |
|---|---|---|
| `template` | passing the flag is all there is to do: either the template reads it (Qwen3, Gemma 4), or the model does not reason and there is nothing to suppress | offer the toggle |
| `prefill` | the template ignores it, but reasoning is a structural section the turn can start past (harmony/gpt-oss) | offer the toggle |
| `none` | the model reasons on every turn and cannot be asked not to (LFM2.5) | show the control disabled, and say why |

Which one a model gets is decided from data the model supplies, never from its name.

`none` requires positive evidence that the model reasons *and* owns the span it reasons in: it
declares a `<think>`-style pair **and** its template actually uses it. A span the model opens and
closes itself is its own, so handing it one already closed and empty is only a suggestion — LFM2.5
reasons straight past it and emits the reasoning *untagged into the answer*, worse than not asking
at all. Both halves of the test matter, because handlers publish the tag pair for a whole family:
the non-reasoning members (LFM2-8B-A1B, LFM2.5-Instruct) advertise a `<think>` they never emit, and
reporting those uncontrollable would tell the user "this model always reasons" about a model that
never does.

`prefill` is the opposite shape: no span of the model's own, but the format separates reasoning
structurally (a channel), and starting the turn past that section is not something it can decline.

`BMOE_DONE` carries the end-of-generation summary (the one-shot mode's `generation:` /
`moe-stream:` text lines are not emitted in session mode). `n_prompt` is the tokens actually
prefilled **this turn** (the suffix after any reused KV prefix), and `n_past` is the total context
length after the turn — so a multi-turn UI can show both per-turn prefill cost and how full the
context is. `prefill_tps` is the prompt prefill rate; `compute_s_tok`/`io_s_tok` are the per-token
AVERAGES over the run (so a UI can show an average compute-vs-I/O split, not just the last token).
`cache_resident_mib`/`cache_budget_mib` track the fixed cache, `read_mib` is the
total flash streamed this generation, and `stall_s_tok`/`mgmt_s_tok` the per-token overlap stall and
cache-management cost. `text` is the final answer and `reasoning` the final thinking span (empty
unless the model reasoned), same split as the per-token lines. `BMOE_ERROR` with `fatal:false` is a rejected
request (e.g. the prompt plus `n_predict` exceeds `n_ctx`) and leaves the session usable;
`fatal:true` means the process is ending.

## Decode traces

`--compute-trace PATH` and `--io-trace PATH` decompose the two terms the per-token CSV can only
report as totals. The route trace answers *what the router asked for*; these answer *where the
time went*, and they exist because the headline number they decompose is not measured at all —
`compute_ms` is a residual, so every cost the engine does not itself clock (page faults, scheduler
stalls, the matmuls) is pooled into it.

Both are **diagnostics, not telemetry**, and both perturb what they measure. **A traced run is not
a benchmark run.** Read the shares, not the absolutes.

### `--compute-trace` — one row per graph node

Returning `true` from the eval callback makes ggml compute exactly up to that node, synchronize,
and call back — so the wall delta between consecutive boundaries is that node's real compute time,
measured, not inferred. The same boundaries sample major faults, which is the point: on a >RAM
model most of "compute" is flash faults, and no residual can show that. The cost is a barrier per
node and no operator coalescing, so the total is inflated well above an untraced run.

Unlike the other traces this one does **not** need `--moe-stream`: it times the graph, which a
plain mmap run has too, so a dense baseline can be traced and compared against a streamed one.

```
# compute_trace v1
# model=... arch=qwen3moe n_layer=48 n_threads=4 io_threads=4 o_direct=1 overlap=0
turn,phase,step,seq,layer,op,name,wall_ns,majflt
0,1,29,0,-1,GET_ROWS,embd,428500,0
0,1,29,1,0,RMS_NORM,norm-0,19500,0
```

`seq` is the node's execution order in the decode; `layer` is parsed from the node name's `-<il>`
suffix (`-1` = belongs to no layer: embeddings, the output head, masks). `op` and `name` are raw —
which node is attention vs dense FFN vs expert matmul is naming policy that varies by
architecture, so the engine reports what the graph said and the analysis script classifies.

### `--compute-trace-layers` — one row per layer segment

The per-node trace's barrier count is also its distortion: ~3000 barriers per token serialize the
graph against the expert stream, so on a model that streams heavily the trace mostly measures its
own serialization. Layer granularity isolates only the **first node of each layer** — ~`n_layer`
barriers per token — which preserves operator coalescing and, crucially, the async expert
prefetch: the io lanes keep reading across a boundary, so the traced numbers sit close to an
untraced run and can be compared across models. The trade is per-op detail: a row aggregates
everything since the previous boundary.

Rows share the per-node schema with `op` fixed to `LAYER`. `name` says which segment: `blk.<il>`
is layer il's, `pre` is the embedding lookup before layer 0, and `post` — emitted when the batch
closes — is the last layer's tail plus the final norm and LM head. The routing nodes the streamer
isolates anyway also close a segment (a barrier that exists untraced too, so it costs nothing
extra); those rows carry the same `blk.<il>` name and simply sum into their layer.
`scripts/decode-analyze.py compute` detects the granularity and prints the per-segment table.

### `--io-trace` — one row per flash read

Needs `--moe-stream` (no engine-issued reads without it). Records every `pread` the streamer
issues, tagged with the `(layer, expert, projection)` it serves.

```
# io_trace v1
turn,phase,step,layer,expert,proj,lane,spec,offset,req_bytes,read_bytes,latency_ns
0,1,29,0,87,1,0,0,1526304,65536,69632,416800
```

`req_bytes` is what the caller wanted; `read_bytes` is the aligned window actually pulled — the
gap is O_DIRECT alignment waste, and `read_bytes` is what effective bandwidth must be judged
against. `spec=1` marks a speculative prefetch read. Rows are stamped with the decode they were
drained after, so a read straddling a token boundary is attributed to the decode that flushed it.

### Reading them

`scripts/decode-analyze.py` (stdlib only) reports what each file is for:

```bash
scripts/decode-analyze.py compute ct.csv --layers   # share by op, fault attribution, by layer
scripts/decode-analyze.py io io.csv --adjacent      # latency percentiles, size/bandwidth, lanes,
                                                    # and the coalescing ceiling
```
