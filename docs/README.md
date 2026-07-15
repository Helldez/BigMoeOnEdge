# Documentation

Start with [architecture.md](architecture.md) for the layer map, or [moe-streaming.md](moe-streaming.md)
for the idea the project is built on.

## Understanding the design

| Doc | What it answers |
|---|---|
| [architecture.md](architecture.md) | How the layers fit together, and why llama.cpp is not forked. |
| [moe-streaming.md](moe-streaming.md) | Why streaming experts from flash makes a >RAM model run at all. |
| [seam.md](seam.md) | The exact contract with llama.cpp's public API, and how to upgrade the submodule. |
| [limitations.md](limitations.md) | What this does not do, what it cannot do, and the prior art it builds on. |
| [roadmap.md](roadmap.md) | Themes worth exploring next. |

## Using and extending it

| Doc | What it answers |
|---|---|
| [adding-a-model.md](adding-a-model.md) | How to support a new MoE architecture (a recipe row plus a gate). |
| [telemetry.md](telemetry.md) | The `BMOE_*` line protocol and CSV schema — the integration contract. |
| [session.md](session.md) | Session lifecycle, KV prefix reuse, cancellation. |
| [adaptive-cache.md](adaptive-cache.md) | `--cache-mb auto`, the cache ceiling, and dense warm-up. |
| [prefetch.md](prefetch.md) | `--prefetch K`: the design and why it cannot change output. |
| [android-memory.md](android-memory.md) | What reclaims the engine's memory on a phone, which levers exist (almost none), and why the cache hit rate is what the kernel judges you by. |
| [rewarm.md](rewarm.md) | `--rewarm`: restoring reclaimed memory, why it was built, and the measurements that show it does not work. |

## Measurements

| Doc | What it answers |
|---|---|
| [benchmarks.md](benchmarks.md) | Measured results per model, with device-pressure numbers. |
| [benchmark-method.md](benchmark-method.md) | How the numbers are produced, so you can reproduce them. |
| [warmup-analysis.md](warmup-analysis.md) | Why first tokens are slow, and the two regimes behind it. |
| [bench-data/](bench-data/) | Raw per-run CSVs and session notes. A dated archive — see its README. |

Every benchmark figure in these docs is measured on the hardware named beside it. If you
re-measure, update [benchmark-method.md](benchmark-method.md) and the affected table together.
