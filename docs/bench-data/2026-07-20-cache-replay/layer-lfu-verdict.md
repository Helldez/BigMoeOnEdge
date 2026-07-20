# layer-lfu on device: wins the metric it was designed for, loses the run

`--cache-policy layer-lfu` (per-layer budget partition, least-frequently-used eviction inside a
layer) was built because the offline replay put it 3.5-5 points of hit rate above global LRU at
shipped budgets on top-6 models. On device it delivers that hit rate and is **~30 % slower**.

## The A/B

Qwen3-30B-A3B-Q4_K_M, 256-token decode, `--cache-mb 3000 --io-threads 4 --n-expert-used 6
--overlap --dense-weights anon -t 4`. Two passes with the order **reversed** between them, so a
result that follows the policy can be told apart from one that follows position:

| cell | order | policy | tok/s | hit % | compute s/tok | mgmt s/tok | io s/tok | **majflt/tok** |
|---|---|---|---|---:|---:|---:|---:|---:|
| `lru_a` | 1st | lru | **5.310** | 70.8 | 0.097 | 0.027 | 0.286 | **17.5** |
| `lfu_a` | 2nd | layer-lfu | 3.567 | 72.8 | 0.189 | 0.036 | 0.262 | **2379.9** |
| `lfu_b` | 1st | layer-lfu | 4.016 | 72.8 | 0.156 | 0.036 | 0.264 | **2367.1** |
| `lru_b` | 2nd | lru | **5.072** | 70.8 | 0.110 | 0.028 | 0.269 | **6.1** |

LRU wins in both orders, including the pass where it ran second and throttled (entry 1.90 GHz
against layer-lfu's 2.19 GHz). The contamination pushes against the winner, so the margin is a
lower bound.

## What worked, exactly as predicted

Cache behaviour is deterministic and reproduces to the decimal across passes: hit rate
70.8 % → **72.8 %** (+2.0 points), flash read 211.48 → **196.55 MiB/token** (−7 %), and
`io_s/tok` duly falls. The replay predicted +3.6 points decode-only; the mechanism is doing
precisely what it was designed to do.

## What it costs

`majflt/tok` goes from **6-18 to ~2370** — 150-400x, and reproducible to within 0.5 % between
the two layer-lfu cells, so this is the policy, not the environment. The extra time lands in
`compute_s/tok` (0.10 → 0.17), which is the known signature of fault-service time billed to the
compute residual, not of slower kernels: the gates prove both policies produce identical bytes,
so no kernel changed.

The mechanism: a **hard per-layer cap removes the cache's ability to self-balance**. Under one
global budget a hot layer simply keeps more entries and a cold layer fewer — the allocation
follows demand at no cost. Under the partition every layer is capped at 3000/48 = 62.5 MiB
(~21 experts) whether or not it needs that many, so any layer whose working set exceeds its
share evicts and re-admits on *every visit*. Each eviction is an `MADV_DONTNEED` over the
entry's pages and each admission re-commits them, so the partition converts a cheap pointer
move into continuous page-table churn.

That churn is what triggers the fault storm. This device runs kswapd at swappiness 160 against
anonymous memory and nothing protects it, so sustained allocation pressure gets paid for by
reclaiming the `--dense-weights anon` buffers into zram — after which every dense access is a
major fault. The cache buys 2 points of expert hit rate and hands back the dense weights.

## Conclusion

**Do not ship layer-lfu as a throughput feature.** It stays behind `--cache-policy`, default
`lru`, which is what every published measurement was taken against.

The lesson generalises: the replay models *which* entries a policy keeps, and it models that
correctly, but it cannot model *what keeping them costs*. A hit rate is not a proxy for
throughput when the two policies do different amounts of memory work to reach it. Any future
policy proposal has to be measured on device before it is believed, however good its curve.

The partition's other property — immunity to the sub-cycle collapse global LRU suffers (see
`findings.md`) — is real but is not worth this price. The cheap fix for that remains a guard:
the worst-case token cycle is computable at init from shape alone, so a budget below it can be
refused or warned about without changing the eviction policy at all.
