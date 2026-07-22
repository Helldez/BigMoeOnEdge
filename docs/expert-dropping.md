# Cache-aware expert dropping

`--drop-cold-experts F` skips a routed expert when it is **not in the cache** *and* the router
weighted it below `F × (1 / top-k)` — that is, below `F` of the uniform share each of the `k`
selected experts would get if the router split its mass evenly. Off by default.

It is the second lossy knob in the engine, after
[turbo top-k](../README.md#turbo-top-k--the-one-lossy-option), and it exists because the first one
spends quality in a place it does not have to.

## Why cache state belongs in the decision

`--n-expert-used k` drops the routing's tail unconditionally: slot 7 and slot 8 go, whether or not
they were already sitting in RAM. But an expert that is already resident costs **no flash read** —
and on a streamed decode, flash reads are what the token is waiting for
([decode is I/O-bound](benchmarks.md)). Dropping a resident expert pays quality for nothing.

Turn that around and the policy writes itself: **spend quality only where it buys I/O**. Keep every
resident expert however small its weight; consider dropping only the ones that would cost a read,
and only when the router says they barely matter.

## What it costs and what it buys

Replayed over the committed route traces (`docs/bench-data/2026-07-15-route-trace/`), decode phase,
threshold at the uniform share (`F = 1.0`):

| policy | flash reads avoided | router weight discarded |
|---|---|---|
| `--drop-cold-experts 1.0` | **66%** | **9.5%** |
| `--n-expert-used 5` | 23% | 10.6% |
| `--n-expert-used 3` | 59% | 36.8% |

(Qwen3-30B-A3B at k=6; Gemma-4-26B-A4B is within a point on both columns. On gpt-oss-120b at k=2 the
policy matches `--n-expert-used 1`'s read saving while discarding 25% of the weight mass instead of
42%.)

At a comparable quality cost the cache-aware policy avoids roughly **three times** the reads. The
reason is visible in the third column of the trace: about 80% of decode routings are cache hits, and
the policy leaves every one of them alone.

`F` is a curve, not a switch. At `F = 0.75` the same model trades 4.4% of the weight mass for 37% of
the reads — still better than `--n-expert-used 5` on **both** axes.

These are replay numbers and an **upper bound**: skipping a read changes what the cache holds later,
so the real hit pattern drifts from the recorded one. The on-device A/B is what settles it.

## Two properties worth knowing

**A routing is never emptied.** The largest weight in a routing is always at least the uniform
share, so at `F ≤ 1.0` the top expert can never fall below the threshold. `validate()` rejects
`F > 1.0` for that reason, and the implementation additionally pins the top-weighted expert, so the
guarantee does not rest on the bound alone.

**Prefill is excluded by default.** With a cold cache almost every expert is a miss, and the same
threshold discards ~42% of the weight mass instead of ~9%. Prefill is compute-bound anyway, so there
is little to win. `--drop-in-prefill` arms it for experiments.

## The output is no longer reproducible

This is the real novelty, and the reason the flag is off by default and named the way it is.

`--n-expert-used` is lossy but **deterministic**: same prompt, same config, same tokens. Dropping is
lossy and **state-dependent** — what gets discarded depends on what the cache happened to hold,
which depends on everything decoded before it. The same prompt can produce different text across
runs, and a benchmark cell is noisier because the drop rate itself varies.

The greedy byte-identity gates therefore do not cover the policy's output, and cannot: there is
nothing stable to compare against. They cover the machinery instead (see below).

## How it is implemented

The decision needs the **final** router weights, and those are produced several graph nodes after
the topk node where the streamer normally loads. So with the policy armed, `load_layer()` is
postponed from the topk node to the terminal node of the layer's weight chain — the last node before
the expert matmul consumes either the ids or the weights.

Which node is terminal depends on the model's gating (`_norm`, `_softmax`, `_scaled`, or none), so
the hook **learns** it from the graph instead of carrying a per-architecture table: the first graph
of a run records the chain, and dropping starts from the second. A layer whose shape has not been
seen yet simply loads at its topk node, undropped. That costs a run its first token's dropping and
nothing else, and it keeps [hard rule 4](../CLAUDE.md) — no model-specific constants in the
streaming path.

At the decision point two edits happen, both before anything reads them:

1. the dropped slot's **weight is zeroed**, and with `drop_renorm` (default on) the survivors are
   scaled so the routing keeps its original total mass;
2. the dropped slot's **id is repointed** at the routing's top-weighted expert.

The second edit is not cosmetic. An expert the engine declines to read may sit in a
reserved-but-uncommitted slot, and `mul_mat_id` would still touch it. Pointing the slot at an expert
that is certainly resident makes the kernel read valid memory and multiply it by exactly zero. It
costs a duplicate matmul — the right trade on a decode bound by flash rather than arithmetic.

Renormalisation matters more than it looks: without it the layer's expert output is systematically
scaled down by the discarded mass, a perturbation of the residual stream the model never sees in
training. `--drop-no-renorm` exists to A/B that claim.

Cost of the extra barriers: the policy asks for each layer's weight nodes, a handful more
synchronisation points per MoE layer on tensors of a few floats. The same asks a route trace makes.

## Measuring it

The engine reports what the policy actually did, which the flag alone cannot tell you — the
threshold is fixed, the drop rate is not:

```
moe-drop: 1183/2304 routed experts dropped (51.3%), threshold 1.00 x uniform
```

The route trace gains a `dropped` column: `weight` and `residency` stay as the **router** produced
them, `dropped` records what the policy then did, and `expert_bytes` is 0 for a dropped routing
because it costs no read. That is enough to replay a real run against the offline model and check
whether the upper bound held. See [telemetry.md](telemetry.md).

## Gates

`bmoe_moe_gates` covers the machinery, not the policy's output:

- **G8a** — with a threshold below any weight the router can produce, nothing is dropped and the
  output is **byte-identical** to the undropped stream. This proves the deferral and the learned
  terminal node are transparent, separating "the plumbing is correct" from "the policy is lossy" —
  a regression in the first would otherwise hide behind the expected difference.
- **G8b** — at full strength with the cache off, where every routing is a miss and the policy bites
  as hard as it can, generation still completes. The id repointing means no matmul ever reads an
  unloaded slot.

## Status

Built and gated on the host; **not yet measured on device**. The claims above are a replay of
recorded traces, not a benchmark. What is owed before this is recommended anywhere:

- a decode A/B against `--n-expert-used` at matched tok/s, on device;
- a quality comparison at that matched speed — the whole thesis is that this knob buys the same
  throughput for less damage, and only a side-by-side can support it;
- a re-run of the replay against a real traced run with the `dropped` column, to see how far the
  static upper bound overstated the win.

Until then it stays **off by default** everywhere. The example app exposes it under
**Speed / quality → Drop cold experts**, as a percentage of the uniform share (50 / 75 / 100), so
the A/B can be run where the engine actually ships — through the app, not a pushed CLI binary. It is
disabled in mmap mode: the policy has to ask the expert source what is resident, and there is no
expert source without the streamer.
