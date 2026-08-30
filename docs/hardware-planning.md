# Hardware planning (`--auto`)

The engine exposes a lot of knobs, and almost every one of them has a right answer that depends on
the machine rather than on taste. `--auto` derives them instead of asking, and prints the fact
behind each choice so a run can still be explained afterwards.

```
bmoe-cli -m model.gguf --auto --plan-explain
bmoe-cli -m model.gguf --plan-only              # print the plan and exit, without loading anything
bmoe-cli -m model.gguf --probe-io --plan-only   # measure the storage first (implies --auto)
```

It is opt-in. Without it nothing changes.

## What it is not

It does not place tensors across compute devices. That is a capacity problem, it is exactly
solvable from tensor sizes, and llama.cpp already solves it (`--fit`, on by default upstream). The
comment above its solver reads *"assumes system memory is unlimited"*, and everything below that
line is what this plans: what to do when the bytes fit nowhere and have to come off flash.

## The two invariants

**Decline instead of guessing.** Every probed fact is a tri-state, and `Unknown` is an input rather
than a defect: a knob whose deciding fact is missing keeps its default and is reported as
`[unprobed]`. A plan full of `unprobed` lines is a list of the measurements the project still owes
itself, which is worth more than a plan that invented values for them.

**Nothing lossy arms itself.** Expert dropping, substitution and route-ahead change the output and
make a run irreproducible. They are never turned on by a plan, at any quality budget, on any
machine. A caller that arms one keeps it, attributed to `[operator]`.

A third rule falls out of the first two: **a knob you passed by hand is never overwritten.** Flags
and `BMOE_*` env overrides both count as the caller speaking.

## The flow

1. **Probe** the model from its gguf metadata (nothing loaded, no tensor data read) and the machine
   from what it reports for free.
2. **Classify the regime** — a ratio of the two, never a property of either. The whole model inside
   the residency budget is `fits`; only the dense set is `experts-stream`; not even that is
   `dense-oversized`.
3. `fits` **declines streaming and says so**: on that machine reading experts from flash only adds
   reads that resident weights do not need.
4. **Dense policy**, from what a reclaim costs here.
5. **Cache budget** = usable memory, less what the dense policy is about to make non-reclaimable,
   capped at the experts' own size — and floored at the model's token cycle.
6. **I/O policy** from the storage facts.
7. **Emit the plan with its rationale.** A decline is a plan too: the config it carries still runs.

## The facts, and which knob each one decides

| fact | decides |
|---|---|
| residency budget: how much this process may hold | the regime, and the cache budget |
| what happens to anonymous memory under pressure | the dense policy, and the margin |
| whether a reclaim-exempt allocation exists | whether the dense set can be pinned |
| whether mapped file pages count against the fatal limit | whether leaving the dense set mapped is free |
| whether uncached reads work on this path | `--no-odirect` |
| whether a live mapping serialises concurrent reads | `--release-mmap` |
| read rate by request size and lane count | `--io-threads` |
| model: expert slice size | which point of that rate curve applies |
| model: token cycle | the cache floor, below which streaming is declined |

There are no platform names in the rules — Windows, Android, iOS, CUDA and Metal appear only in
`core/src/plan/hardware_probe.cpp`, the adapter that fills the profile. That is what lets the rules
be unit-tested against machines nobody here owns (`tests/planner_test.cpp`), and it is why the same
rule that picks anonymous buffers on a desktop picks a pinned allocation on a phone and leaves the
weights mapped on a platform that kills a process for holding too much: one rule, three facts,
three answers, no branch.

## The token cycle

The cache floor is the model's own arithmetic: for every MoE layer, one expert's bytes across that
layer's expert tensors, times the routing width. Under it the cache evicts what the same token
still needs, so it costs its memory and returns no hits at all — the cliff `docs/cache-sizing.md`
documents.

It is summed over the tensors the file carries rather than multiplied out, because the factors are
not uniform: a fused `gate_up` projection is twice the stride of a split one, an architecture with
leading dense blocks has layers that demand nothing, and a trailing multi-token-prediction block
names expert tensors that llama.cpp never loads. On Qwen3.6-35B-A3B-Q4_K_M this derivation returns
581 MiB against the 582 MiB the engine computes at load from the tensors it actually bound — an
independent check that the metadata-only path sees the same model.

Because the floor is per-model, it also overrides the generic `cache_min_mb` guard: a budget above
this model's cycle is not pathological however small it looks, and the plan says so when it forces
past it.

## The storage probe (`--probe-io`)

Under a second, a few tens of MiB read. It is a separate opt-in rather than part of `--auto`,
because a caller that wants a plan without touching the drive should get one.

It measures the rate curve around the model's own expert slice, at one, two and four lanes, and
picks the **smallest** lane count that reaches within 5% of the best rate: where two lane counts
deliver the same throughput the cheaper one is strictly better, and a difference inside the probe's
own noise would otherwise flip the answer between runs.

That prediction is falsifiable, and on the desktop it was checked against the engine: the probe
picked two lanes, and a real run at the same cache budget gave 3.400 / **3.969** / 3.535 tok/s at
one, two and four lanes. Two is right, and it is not the four the CLI ships as its default.

### What it will not tell you

The same probe tries to answer whether a live mapping of the model serialises concurrent uncached
reads, by reading with a mapping alive and again after releasing it. **It reports `Yes` or nothing,
never `No`**, and that is a deliberate limitation rather than an oversight.

On the desktop where the engine gains 24% of decode from `--release-mmap` — its own read rate goes
from 871 to 1680 MiB/s — this probe sees the two arms within 2% of each other. Whatever it is
failing to reproduce (most likely the access pattern: it reads uniformly at random where the engine
walks expert slices layer by layer against a warm cache), its fidelity is established in the
positive direction only. A negative from an instrument that missed a known positive is not evidence
of absence, and printing one as `measured` would be exactly the confident wrong answer this design
exists to avoid. So the plan prints the two rates, says the probe saw nothing, and tells you to
measure `--release-mmap` yourself.

## What is not probed yet

- **Device memory bandwidth.** It is the number that decides an offload, and nothing here measures
  it, so a machine with a discrete GPU gets a plan that says exactly that.
- **Threads and `--ubatch`** have no rule at all: nothing relates core topology to decode
  throughput here, and the compute-buffer reservation's crossover against the cache is unmeasured.

One caveat on the budget itself: it is a one-shot reading of a quantity that moves. The same
machine and model planned twice minutes apart produced 4.3 GB and 8.2 GB of available memory, and
therefore two different cache budgets. The plan quotes the number it used, which is what makes the
difference visible rather than mysterious.

Compute devices are enumerated through `ggml_backend_dev_*` and recorded, but no rule reads them
yet; a device whose memory is host memory frees nothing when a tensor moves off it, which is why
the capacity tier is nearly inert on an integrated GPU and only the bandwidth tier is left.
