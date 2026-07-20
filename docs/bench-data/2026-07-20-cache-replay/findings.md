# Expert-cache policy replay — is the low hit rate policy or working set?

Offline replay of the committed route traces (`../2026-07-15-route-trace/`) through
hypothetical cache policies, via `scripts/route-replay.py`. No device time, no engine change.
Answers the question issue #33 asks: a low hit rate is either LRU choosing badly (fixable)
or nothing fitting (not fixable). The two have opposite conclusions.

## Simulator validation

The replay reproduces the engine's accounting: one lookup per unique expert per `load_layer`
batch (duplicates promote only), a miss costs `expert_bytes(layer)`, eviction runs after the
batch and never evicts an entry the batch itself touched. Prefetch was off in all three
captures (`spec_read_MiB=0`), so the traces are pure LRU with nothing speculative to model.

| model | recorded `cache_hit_pct` | LRU replay | budget |
|---|---|---|---|
| gpt-oss-120b | 32.4 % | **32.4 %** | 3000 MiB |
| Qwen3-30B-A3B | 74.9 % | **74.9 %** | 4000 MiB |
| gemma-4-26B-A4B | 76.8 % | **76.8 %** | 4000 MiB |

Second, independent check on a budget the simulator was **not** calibrated against:
`cache-sizing.md:28-32` records a device measurement where shrinking Gemma's budget
4000 → 2909 MiB cost ~10 points of hit rate. The replay predicts 81.9 % → 72.3 %, a
−9.6 point delta. The simulator tracks budget sensitivity, not just absolute level.

## Result 1 — the LRU cliff is real, and it is total

Below one token cycle `T` (all layers × top-k, worst case) **global LRU returns exactly
0.0 % hit** on all three models, while the offline optimum still scores 18–48 % at the same
budget. This is textbook sequential flooding: the layer cycle 0..N-1 is deterministic, so the
least recently used entry is the *soonest* to be needed again. Once the budget cannot hold a
full cycle, every entry is evicted just before its next use. It does not degrade gracefully —
it goes to zero.

| model | `T` (one token cycle) | LRU below `T` | belady | per-layer partition |
|---|---|---|---|---|
| gpt-oss-120b (top-2) | 908 MiB | 0.0 % @ 750 | 37.2 % | 19.1 % |
| Qwen3-30B (top-6) | 785 MiB | 0.0 % @ 500 | 45.3 % | 38.3 % |
| gemma-4-26B (top-6) | 676 MiB | 0.0 % @ 500 | 48.2 % | 36.0 % |

Today nothing walks into this by accident: `cache_min_mb = 1500` (`config.h:101`) happens to
sit above `T` for all three. That is luck, not a guard — `T` scales with `--n-expert-used`,
so raising top-k (gpt-oss at k=4 → `T` ≈ 1816 MiB) or setting a small `--cache-mb` by hand
lands under the cliff with no warning and no way to tell from the outside.

**The existing cache floor does not protect against this.** The floor is one *layer*
(mechanical, deliberately); the cliff is at one *token cycle* — all layers. Different
quantities, roughly `n_layer` apart.

## Result 2 — at today's operating points, the policy is not the bottleneck

The gap to the offline optimum is real but no *online* policy closes it. Every realistic
alternative lands within a few points of LRU:

| model @ budget | belady | lru (today) | lfu | layer | layer+lfu |
|---|---|---|---|---|---|
| gpt-oss @ 3000 | 67.8 | 44.8 | 43.6 | 45.9 | 44.5 |
| Qwen3 @ 3000 | 87.8 | 70.6 | 71.3 | 71.1 | **74.2** |
| Qwen3 @ 4000 | 92.7 | 80.3 | 80.8 | 79.7 | **81.7** |
| gemma @ 3000 | 88.6 | 73.1 | 75.5 | 74.8 | **78.1** |
| gemma @ 4000 | 92.9 | 81.9 | 83.4 | 82.2 | **84.3** |

`layer+lfu` (per-layer budget partition, frequency-ordered eviction inside a layer) is the
best online policy on both top-6 models: **+3.5 to +5 points** at the budgets actually shipped,
more at smaller budgets (+6.8 on Qwen @ 2000). On gpt-oss (top-2) it is a wash — the plain
per-layer partition is marginally best there and the whole spread is ~1 point.

A 5-point hit gain removes roughly 1/5 of the remaining expert reads. With flash I/O at
~half the token budget, that is order **5–10 % tok/s** — worth one device run, not a
rewrite.

## Result 3 — issue #78 would cost hit rate, as previously measured

Subtracting the dense anon bytes from the `auto` budget shrinks it by the model's dense
total. Reading that shrink off these curves:

| model | dense total | budget after | hit before → after |
|---|---|---|---|
| gpt-oss-120b | 790 MiB | 3000 → 2210 | 44.8 → ~34 % |
| Qwen3-30B | 541 MiB | 4000 → 3459 | 80.3 → ~76 % |
| gemma-4-26B | 1224 MiB | 4000 → 2776 | 81.9 → ~70 % |

This reproduces, offline, the reason the earlier fold-dense-into-the-floor attempt was
rejected on device. The over-ask is real, but paying for it out of the expert budget costs
5–11 points of hit rate. Any #78 fix has to come from somewhere other than the cache budget,
or be justified by reclaim savings that outweigh a double-digit hit-rate loss.

## Device confirmation (same day, gpt-oss-120b, 256-token decode)

Three single runs, `--moe-stream -t 4 --overlap --dense-weights anon --no-think`. Entry state
logged per cell (`driver2.log`). CSVs: `lanes4_k2.csv`, `lanes8_k2.csv`, `cliff_k4_c1500.csv`.

### The cliff is real on hardware, and it is exactly zero

`cliff_k4_c1500` runs top-k 4 with a **1500 MiB budget — a legal value** (it is exactly
`cache_min_mb`; no `--force-cache` needed) against a measured `token_demand_MiB = 1815.4`.
The replay predicted a collapse. The device delivered:

| | cache 2000 @ k=4 ‡ | **cache 1500 @ k=4** | cache off @ k=4 ‡ |
|---|---|---|---|
| cache hit | 27.1 % | **0.0 %** | — |
| flash read / token | 1292 MiB | **1817.05 MiB** | 1817.0 MiB |
| cache mgmt / token | — | **0.233 s** | 0 |
| tok/s | 0.998 | 1.047 | 0.711 |

‡ from `../2026-07-17/`, a different thermal session — the tok/s column is not comparable
across the three, the **hit rate and read volume are**.

A 1500 MiB cache reads *exactly what having no cache at all reads* (1817.05 vs 1817.0 MiB per
token) while holding 1500 MiB of RAM and spending **0.233 s/token — 24 % of the token —
managing it**. Dropping the budget 2000 → 1500 (−25 %) does not cost a quarter of the hit
rate; it costs all of it. `token_demand_MiB` (1815.4) and `layer_demand_MiB` (50.4) are both
already in the summary, 36× apart: the existing floor is one layer, the cliff is one cycle.

### I/O lanes scale: +9.7 %, measured against the handicap

The 2026-07-17 lane pair was unusable (the 4-lane cell entered at 52.9 °C and peaked at
93.8 °C, the 8-lane cell entered at 44.4 °C). Here the order is **reversed on purpose** — the
4-lane cell runs first on the cold device, the 8-lane cell second from a hotter one:

| cell | entry CPU | entry freq | exit freq | read/token | hit | **tok/s** |
|---|---|---|---|---|---|---|
| `lanes4_k2` | 34.7 °C | 2.27 GHz | 2.27 GHz | 587.39 MiB | 32.3 % | 2.202 |
| `lanes8_k2` | 44.4 °C | 2.27 GHz | **2.19 GHz** | 587.39 MiB | 32.3 % | **2.415** |

Read volume, hit rate and resident bytes are **identical to the decimal** — the lane count is
the only variable. 8 lanes wins by **+9.7 %** while entering ~10 °C hotter and throttling down
mid-run, so the contamination pushes against the claim and this is a **lower bound**.
Effective flash bandwidth 1294 → 1419 MiB/s.

8 is the current cap (`io_threads_max`, `config.h:102`), so this says the cap is reached with
headroom left, which is what #75 proposes to unlock. It does **not** say how far past 8 the
scaling continues — that needs either raising the constant or a purpose-built microbench.

### Runtime read coalescing is not worth building on the native layout

Merging adjacent routed slices (#75's second half) needs the routed experts of a layer to sit
at consecutive ids. Measured over the decode traces:

| model | routed / layer visit | consecutive-id pairs | reads after an ideal merge |
|---|---|---|---|
| gpt-oss (top-2) | 2.00 | 0.6 % | 1.99 |
| Qwen3 (top-6) | 6.00 | 4.0 % | 5.76 |
| gemma (top-6) | 6.00 | 3.9 % | 5.77 |

An ideal coalescer removes 0.5–4 % of the read *count* and zero bytes. The
expert-contiguous repack listed in `roadmap.md:19` is the enabler; without it this half of
#75 has nothing to merge.

## Decisions

- **H1 (working set)** — partly. The optimum is 11–23 points above LRU, so the working set is
  not the whole story, but no online policy recovers more than ~5 of those points.
- **H2 (cliff)** — **confirmed, and worse than stated in the issue**: exactly 0 %, not
  "collapsing toward zero". Worth a cheap guard regardless of the policy question.
- **H3 (wrong axis)** — confirmed as the right axis: per-layer partitioning is what removes
  the cliff by construction, and combined with frequency it is the best online policy tested.

A detail worth keeping: `expert_stream_source.h:213-217` already describes this boundary —
below `token_demand_` the cache "stops holding anything BETWEEN tokens, so its hit rate
collapses to inter-token correlation only". The replay says global LRU does **worse than that
comment assumes**: it keeps 0 %, not the inter-token correlation. Measured token-to-token
routing overlap on gpt-oss is 17.9 %, and the per-layer partition scores 19.1 % at the same
sub-cycle budgets — i.e. the partition is what actually delivers the behaviour the comment
describes, while global LRU churns the whole cache within one cycle and loses even that.

## Suggested order of work

1. **Guard the cliff.** Both quantities are already measured and printed. Nothing sizes from
   them today (they are "pure telemetry now that the governor is gone"), but the worst-case
   cycle is computable at init from shape alone — `Σ_layers min(n_expert_used, n_expert) ×
   expert_bytes(layer)` — so refusing or warning on a budget under it costs almost nothing.
   The current protection is `cache_min_mb = 1500` happening to exceed the cycle on today's
   models; raise top-k and it stops being true, silently.
2. **#75 lanes past 8.** The only measured double-digit tok/s lever. Needs the cap raised or
   a microbench to find where the scaling stops.
3. **`layer+lfu` policy.** +3.5-5 points of hit at shipped budgets on the top-6 models,
   ~5-10 % tok/s, and it removes the cliff by construction rather than guarding it.
4. **Not #78 as specified**, and not runtime coalescing — both are measured to cost more than
   they return.

## Reproduce

```
python scripts/route-replay.py docs/bench-data/2026-07-15-route-trace/qwen.route.csv --validate 4000
python scripts/route-replay.py docs/bench-data/2026-07-15-route-trace/qwen.route.csv --budgets 250,500,1000,2000,3000,4000
```

Per-model curves: `*-curve.csv` (machine readable), `*-curve.txt` (as printed).
