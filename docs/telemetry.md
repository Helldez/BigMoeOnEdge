# Telemetry contract

With `--progress`, `bmoe-cli` emits machine-readable lines the Android app (and any other
consumer) parses. The format is versioned by this document; keep it stable.

## Per-token lines

Emitted once per generated token, in order:

```
BMOE_LOAD {"mb":<float>,"ms":<float>}
BMOE_PROGRESS {"step":<int>,"steps":<int>,"wall_ms":<float>,"io_ms":<float>,
               "compute_ms":<float>,"mgmt_ms":<float>,"stall_ms":<float>,"read_mb":<float>,
               "cache_hit_pct":<float>,"majflt":<int>,"cpu_ms":<float>,"resident_frac":<float>,
               "text":"<string>"}
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
- `majflt` / `cpu_ms` **decompose the `compute_ms` residual** — the whole point being that "compute"
  above is a catch-all that silently absorbs page faults and scheduler stalls, not just matmul.
  They are measured directly around `llama_decode` (no submodule patch needed): `majflt` is the
  major page faults served this token — a non-zero count means a mmap-resident (dense) weight was
  re-faulted from flash *inside* the decode, i.e. a >RAM residency stall masquerading as compute.
  `cpu_ms` is CPU time summed across all threads; compare it to `wall_ms × threads` for occupancy —
  near 100% is genuinely compute-bound, well below means the cores were throttled, preempted, or
  blocked (a low-clock frequency cap or a co-resident process), not doing more math. Both are `0`
  when the platform can't report them (the Windows host build); treat `0` as "unmeasured".
- `resident_frac` is the sampled fraction of the expert cache's own pages the kernel still had in
  RAM at the last probe. Below `1` the device is reclaiming the cache mid-run — the pressure
  `--cache-dynamic` sizes against (see [pressure.md](pressure.md)). It is `-1` when unmeasured: the
  sampler is throttled (it probes every ~2-3 tokens), streaming is off, or the platform cannot
  report residency. Treat `-1` as "no sample", never as "nothing resident".
- `text` is the full generated text so far, JSON-escaped (for streaming into a UI).

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

Under `--overlap` the `moe-stream:` line additionally reports `stall_s/tok=<s>` — the mean
wall time per token that compute threads waited for expert reads to complete. It is `0` in
serial mode (where the read wait is already folded into decode time).

`moe-cache:` is present only when a cache is active. The `=== answer ===` / `=== perf ===`
banners appear only in `--progress` mode; without it the CLI streams the answer inline and
prints just the summary lines.

## CSV sink

`--csv PATH` additionally writes one row per token:

```
step,steps,wall_ms,io_ms,compute_ms,read_bytes,cache_hit_pct,stall_ms,mgmt_ms,majflt,cpu_ms,resident_frac
```

followed by a `# summary ...` comment line. Intended for the benchmark sweep.

`stall_ms`, `mgmt_ms`, `majflt`, `cpu_ms` and `resident_frac` are trailing columns appended (in that
order) after the original seven. `stall_ms` is the wall time compute threads waited for expert reads
that token (`0` in serial mode); `mgmt_ms` is the cache-management time described above;
`majflt`/`cpu_ms` are the fault + CPU-time decomposition of the compute residual (see the
`BMOE_PROGRESS` notes above), `0` when unmeasured; `resident_frac` is the sampled cache residency,
`-1` when unmeasured. All are additive: older CSVs have fewer columns, so consumers must read by
column NAME (from the header row) and treat any as optional. The `# summary` line likewise gains
`stall_s/tok=<s>`, `mgmt_s/tok=<s>`, `majflt/tok=<f>`, `cpu_s/tok=<s>`, `token_demand_MiB=<f>` (the
expert bytes one token routes, measured — where cache hits start, NOT a floor to defend; see
[pressure.md](pressure.md)), `layer_demand_MiB=<f>` (the widest layer's routed bytes: the mechanical
floor the governor may not cut below) and `cache_cuts=<int>` (times `--cache-dynamic` shrank the budget under reclaim); see the
`io_ms` note above for how the read-time columns are reinterpreted under overlap.

## Route trace

`--route-trace PATH` writes the per-step, per-layer MoE routing trace. Everything above answers
*how long* a token took; this answers *what the router asked for* — which experts each layer
routed, how strongly, and whether they were already resident. It needs `--moe-stream` (without
streaming there is no routing to observe) and it is a **diagnostic, not telemetry**: capturing it
asks the compute graph for extra nodes, which adds a barrier per MoE layer, and it writes a row
per routed expert. **A traced run is not a benchmark run** — the numbers in the `--csv` of a
traced run are slower than the real thing, and `mgmt_ms` in particular shifts, because settling
speculative prefetch moves outside the window that times it.

The file is long format: a `#` preamble carrying the run's static facts, then one row per routed
expert. Conceptually it is a matrix — rows are steps, columns are layers — and a **cell** is the
`n_expert_used` rows sharing `(turn, phase, step, layer)`.

```
# route_trace v1
# model=<path> arch=<string> n_layer=<int> n_expert=<int> n_expert_used=<int>
# layer=<int> expert_bytes=<int> dense_bytes=<int>        (one per layer)
turn,phase,step,layer,slot,expert,weight,residency,expert_bytes
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
BMOE_READY {"load_s":<float>,"arch":"<string>","n_ctx":<int>}          # once, after the model loads
BMOE_BEGIN {"id":<int>}                                                # a generation started
BMOE_LOAD / BMOE_PROGRESS ...                                          # per token, as above
BMOE_DONE  {"id":<int>,"cancelled":<bool>,"tokens":<int>,"tok_s":<float>,
            "prefill_s":<float>,"prefill_tps":<float>,"load_s":<float>,"cache_hit_pct":<float>,
            "n_prompt":<int>,"n_past":<int>,"compute_s_tok":<float>,"io_s_tok":<float>,
            "cache_resident_mib":<float>,"cache_budget_mib":<float>,"read_mib":<float>,
            "stall_s_tok":<float>,"mgmt_s_tok":<float>,"majflt_tok":<float>,"cpu_s_tok":<float>,
            "token_demand_mib":<float>,"cache_cuts":<int>,"text":"<string>"}
BMOE_ERROR {"id":<int>,"fatal":<bool>,"msg":"<string>"}
```

`BMOE_DONE` carries the end-of-generation summary (the one-shot mode's `generation:` /
`moe-stream:` text lines are not emitted in session mode). `n_prompt` is the tokens actually
prefilled **this turn** (the suffix after any reused KV prefix), and `n_past` is the total context
length after the turn — so a multi-turn UI can show both per-turn prefill cost and how full the
context is. `prefill_tps` is the prompt prefill rate; `compute_s_tok`/`io_s_tok` are the per-token
AVERAGES over the run (so a UI can show an average compute-vs-I/O split, not just the last token).
`cache_resident_mib`/`cache_budget_mib` track the (possibly auto-adapting) cache, `read_mib` is the
total flash streamed this generation, and `stall_s_tok`/`mgmt_s_tok` the per-token overlap stall and
cache-management cost. `BMOE_ERROR` with `fatal:false` is a rejected
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
