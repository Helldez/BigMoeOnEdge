# Architecture

BigMoeOnEdge is a small ports-and-adapters engine that sits **on top of** llama.cpp's
public API. Its guiding constraint: drive streaming through the public API so upstream
updates cost a submodule pointer bump and nothing else. The serial streamer holds to this
against stock upstream; the one exception is the optional `--overlap` feature, which carries
a single ~25-line hook on a fork branch with an explicit sunset (see below and
[seam.md § 3](seam.md)).

## Layers

```
cli/            bmoe-cli — parses flags, the only place env vars are read
core/
  include/bmoe/ ports (interfaces) + config, pure policy, no llama.cpp dependency
    config.h        RunConfig + validate()
    expert_source.h IExpertSource — the residency strategy port
    recipe.h        MoeRecipe + registry
    metrics.h       TokenMetrics / RunSummary + IMetricsSink
    runtime.h       run() entry point
  src/
    io/         platform_io — O_DIRECT reads + reserve/commit/evict VM, cross-platform
                file_reader — pooled positioned reader, per-consumer O_DIRECT
    moe/        gguf_offsets, arch_registry, expert_stream_source, router_hook
                dense_weights — non-expert weight policy + the residency sensor
    engine/     session — composition + the generation loop (open/generate/close)
                runtime — the one-shot run() wrapper over a Session
                chat_parse — reasoning-parser wiring (llama.cpp `common`, see seam.md)
    metrics/    csv_metrics_sink, route_trace_sink, decode_trace_sink
third_party/
  llama.cpp     upstream submodule; public-API consumer, plus one optional overlap hook
tests/          byte-identity gates
examples/android an APK that drives bmoe-cli via ProcessBuilder
```

Dependencies point inward: adapters depend on the port headers, the CLI composes them.
The pure-policy code (`config.cpp`, `arch_registry.cpp`) compiles with no native
dependency, so a subset of the project builds and is testable before llama.cpp is
fetched.

## Why the streaming seam needs no fork

Streaming experts serially needs three things from the inference engine. All three are
already public in llama.cpp:

1. **A hook at routing time.** `llama_context_params.cb_eval` is called for every graph
   node. We ask for only the routing nodes (`ffn_moe_topk-<il>`); ggml computes and
   synchronizes each alone, then calls us back with the selected expert ids materialized.
2. **The expert tensor pointers.** During a one-token warm-up we scan each graph node's
   sources for tensors named `blk.<il>.ffn_{gate,up,down}_exps.weight` and record the
   live `ggml_tensor*`. We then rebind their `->data`.
3. **The file layout.** `gguf_get_tensor_offset` (public) gives each tensor's byte offset
   so we can `pread` individual expert slices.

Loading with `use_mmap=true, use_extra_bufts=false` keeps the weights in their native
gguf layout (a repacked buffer would break the rebind). That is a public model
parameter.

Because none of this touches llama.cpp internals, the serial streaming path runs against
the unmodified upstream repository. Contrast with approaches that patch the model files:
those must be rebased on every release. Here, `git submodule update --remote` and a rebuild
is the whole upgrade.

The one place we do carry an extension is the optional `--overlap` feature. Overlapping a
token's expert reads with its expert matmuls needs a per-expert wait point *inside* the CPU
MoE kernel, which no public API exposes, so the submodule pins a fork branch adding one
~25-line readiness hook on top of the upstream commit. It is zero-cost when unregistered,
the serial path still builds against stock upstream, and it is dropped the moment upstream
ships an equivalent callback. Details and the sunset condition are in
[seam.md § 3](seam.md).

See [seam.md](seam.md) for the exact callback contract and the ggml behaviour it relies
on.

## The generation loop

The composition root is `Session` (core/src/engine/session.cpp):

1. `open()` — load model (mmap on, repack off, experts on CPU); if streaming, resolve the
   architecture recipe, install the router hook, do the capture warm-up, bind the expert
   source, clear the warm-up KV. Done **once** per model.
2. `generate()` — prefill the prompt, then greedily decode `n_predict` tokens, reporting
   per-token metrics. Callable repeatedly; the expert cache stays warm between calls (see
   [session.md](session.md)). Cancellable mid-flight via the abort callback.
3. Destructor — tear down in order: I/O pool, context, hook, model, backend.

`run()` (core/src/engine/runtime.cpp) is a thin one-shot wrapper — open, one generate, close —
so the gates and the interactive session share the same code path.

Greedy sampling makes the output a deterministic function of the graph — the property the
[byte-identity gates](../tests/moe_gates.cpp) assert.
