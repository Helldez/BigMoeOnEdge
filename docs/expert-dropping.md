# Cache-aware expert dropping

`--drop-cold-experts F` skips a routed expert when it is **not in the cache** *and* the router
weighted it below `F × (1 / top-k)` — that is, below `F` of the uniform share each of the `k`
selected experts would get if the router split its mass evenly. Off by default.

It is the second lossy knob in the engine, after
[turbo top-k](../README.md#turbo-top-k--the-measured-lossy-option), and it exists because the first one
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

(Qwen3-30B-A3B at k=6; Gemma-4-26B-A4B is within a point and a half on both columns: 67.4% / 8.2%. On gpt-oss-120b at k=2 the
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

**It requires the expert cache.** With `--cache-mb 0` every expert reads as a miss, so the policy
would stop being cache-aware and become an unconditional weight cut — which is what
`--n-expert-used` already does, without claiming to consult residency. `validate()` rejects the
combination, the same way it rejects `--prefetch` without a cache.

**It changes what `--prefetch` means.** Speculation is normally output-neutral by construction. Here
residency is an *input* to the policy, so a correct guess un-drops an expert that would otherwise
have been discarded: prefetch depth becomes an output-affecting setting. The decision point also
settles pending speculation a few nodes after it was issued, which shortens the overlap window the
prefetch exists for — treat the two as interacting, not composable.

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
moe-drop: <dropped>/<routed> routed experts dropped (<pct>%), threshold <F> x uniform
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
  a regression in the first would otherwise hide behind the expected difference. **G8a'** asserts
  the count separately (`experts_routed > 0`, `experts_dropped == 0`), so "a weight happened to fall
  under the threshold" fails legibly instead of as a mysterious byte mismatch.
- **G8b** — at full strength against a cache small enough to be evicting constantly, so dropped
  experts really do land on slots the cache has released. Generation still completes: the id
  repointing means no matmul ever reads reserved-but-uncommitted memory. (The gates deliberately do
  *not* run this with the cache off — there the shared-slot path has no uncommitted memory, so the
  safety property the repointing exists for would go untested.)
- **G8c** — forcing top-k to 1 makes every routed expert the top one, so dropping must be a no-op at
  any threshold and the output must match the undropped k=1 run byte for byte. This pins both the
  top-expert guarantee and the fact that the threshold is taken against the **effective** top-k
  discovered at runtime — a hardcoded width would not survive the override.

## Defaults, and where the numbers do and do not come from

The **CLI defaults it off**, and will keep doing so: the byte-identity gates need a deterministic
default, and an instrument should not quietly change the thing it measures.

The **app ships it at 75%** — under **Speed / quality → Drop cold experts**, with rungs 50 / 75 /
100 as percentages of the uniform share. It is disabled there in mmap mode and with the cache off,
the same two conditions `validate()` enforces.

That default is a product decision taken on the maintainer's own device measurement. **It is not
backed by a published benchmark in this repository**, and the tables in the README deliberately
carry no rows for it — they are a deterministic protocol and this knob is not deterministic. Nothing
here should be read as "75% is worth X%"; the honest claim is narrower: the replay above says the
shape of the trade is favourable, and the default was chosen after checking it on hardware.

What is still owed before this is recommended beyond that:

- a published decode A/B against `--n-expert-used` at matched tok/s, with the device state recorded
  the way [benchmark-method.md](benchmark-method.md) requires;
- a quality comparison at that matched speed — the whole thesis is that this knob buys the same
  throughput for less damage, and only a side-by-side can support it;
- a re-run of the replay against a real traced run with the `dropped` column, to see how far the
  static upper bound overstated the win.

The [`layer-lfu` entry in the roadmap](roadmap.md) is the standing reminder for why the third one
matters: it simulated exactly as predicted and was ~30% slower in reality.
