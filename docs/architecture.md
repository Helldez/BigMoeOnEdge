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
server/         bmoe-server: the application framework every UI speaks to (docs/application-framework.md),
                and the engine behind the desktop web UI (docs/server-api.md)
                openai_chat (the chat protocol), turn_queue, settings_store, event_bus, json_util,
                model_files (which gguf names make one model, the one-level folder scan, and what a
                file is for by its header: model, projector, draft), plan_view (the plan as the Plan
                screen draws it, every word included), download_policy (which download failures
                are retried and when, what a resume needs on disk, where a .part may be deleted):
                pure policy (bmoe_server_core)
                engine_host: the one Session, loads, the turn line, over the HostedSession seam
                (hosted_session.h) its test drives with a scripted session; model_library: files, catalog,
                downloads; planner_adapter: /api/plan when the planner is built in;
                access: who may reach the API (host, origin, key, content type), pure and tested;
                platform: the only file with platform #ifs; http_server: the HTTP routes and
                run(); the whole is the bmoe_server library, main.cpp the process around it
ui/             the web UI (Svelte + TypeScript); settings rendered from the parameter table
catalog/        models.json: the curated model catalog, as data
core/
  include/bmoe/ ports (interfaces) + config, pure policy, no llama.cpp dependency
    config.h        RunConfig + validate()
    params.h        the parameter table: every RunConfig tunable described once (flag, type,
                    bounds, help); the CLI parses from it, front-ends render settings from its JSON
    expert_source.h IExpertSource — the residency strategy port
    row_source.h    IRowSource - the row-gathered residency port
    expert_layout.h ExpertLayout: which tensors are the routed experts, read from the file's shapes
    metrics.h       TokenMetrics / RunSummary + IMetricsSink
    runtime.h       run() entry point
    session.h       Session: load once, then serve many prompts against the resident state
    chat.h          a conversation as a front-end hands it over: messages, tools, the calls made
    decide.h        a decision over a fixed set of choices from one prefill (decide.md)
    ngram_draft.h   prompt-lookup drafting, the --ngram draft source (ngram.md)
    route_trace.h, decode_trace.h, predict_stats.h
                    the analysis instruments: per-layer routing, what a token's time is made of,
                    expert-prediction accuracy (telemetry.md, expert-prediction.md)
    version.h       the engine's version string, so a benchmark file names its build
    run_error.h     RunError: why a request failed, as a kind every front-end decides on, and its
                    stable name, the `code` of the CLI protocol and the server API
    log.h           the log port: where the engine's diagnostics go (set_log_sink, a level). The
                    default sink writes `bmoe: <line>` to stderr as the engine always has; bmoe-server
                    also publishes each line as a `log` event. Protocol lines never pass through it
    hardware_profile.h, model_profile.h, placement.h
                    the planner's inputs: a machine, a model and llama.cpp's capacity placement as
                    facts, with no platform, vendor or architecture name in them
    planner.h, allocate.h, plan.h
                    the planning rules: (machine, model, request) -> a resolved RunConfig with the
                    reason for each knob; the cost model and the memory ledger; the plan as a value
    probe.h         the adapters' entry points: what fills the two profiles
    auto_plan.h     probe_and_plan: the one probe-and-plan sequence every front-end calls, the
                    adapters in their order and then the rules; the options are what differs
  src/
    io/         platform_io — O_DIRECT reads and their per-file alignment, reserve/commit/evict VM,
                  the spin-wait pause: the OS and ISA branches the streaming path needs
                file_reader — pooled positioned reader, per-consumer O_DIRECT
                mapping_release - releases the model file's mapping after load (--release-mmap)
    moe/        gguf_offsets (tensor → (shard, offset), split ggufs included), expert_layout,
                expert_stream_source (one reader per shard), router_hook
                stall_union - the wall time at least one thread was stalled, not the sum of stalls
                tensor_names - how llama.cpp's tensor and graph-node names, and the capacity
                  fitter's override patterns, are read: the one place, no regex, unit-tested
                dense_weights — non-expert weight policy + the residency sensor
                row_stream - row-gathered tables served from flash (see row-gathered-tables.md)
                device_arena - one or two layer slots on a prefill device, held for the session or
                  only during a prefill, filled from flash, whole layers or only the routed experts
                  (npu-prefill.md)
                decide_probe - experimental per-decision expert usage and layer-exit answers (decide.md)
    engine/     session — composition + the generation loop (open/generate/close); open() and
                  generate() are short sequences of named steps (see "The generation loop")
                prefill_device - moves layer weights and model state onto a device per graph
                prefill_path - the prefill device for one session: setup, placement, device decode
                runtime — the one-shot run() wrapper over a Session
                llama_log — what of llama.cpp's own log reaches stderr: DEBUG dropped unless
                  asked for, a requested cancel's decode errors demoted while it is in flight
                chat_messages — bmoe/chat.h to and from llama.cpp's chat types, in one place
                chat_parse — reasoning-parser wiring (llama.cpp `common`, see seam.md)
                ngram_draft — the prompt-lookup draft source (see ngram_draft.h)
                thinking_control — how "thinking off" is honoured, probed per model
                logits, prefill_support — log-softmax, batch filling and prefill attribution,
                  shared by generate / perplexity / decide
                decide/ — Session::decide() (see decide.md): pure policy over an IDecideBackend
                  port (prompt split, choice scoring, IPrefixCache), plus one llama.cpp adapter
    plan/       the hardware planner (hardware-planning.md). Two halves that must not mix:
                planner, allocate, plan - pure rules over the profiles; no I/O, no platform names
                hardware_probe, model_probe, storage_probe, headroom_probe, bandwidth_probe,
                  device_probe, placement_probe, isolated_probe (a device asked in a second
                  process, so a driver that ends it ends only that) - the adapters that measure a
                  machine and read a gguf; the only place in the planner where a platform or
                  llama.cpp may appear
                auto_plan - the composition of the two: the probes in the order a plan is
                  measured in, then plan_run(). With the adapters because it starts the backends;
                  it decides nothing, and bmoe-cli --auto and the server's /api/plan both call it
    params.cpp  the parameter table's rows (see params.h)
    log.cpp     the log port's default sink and level filter (see log.h)
    metrics/    csv_metrics_sink, route_trace_sink, decode_trace_sink
third_party/
  llama.cpp     upstream submodule; public-API consumer, plus one optional overlap hook
tests/          byte-identity gates
examples/android the Android app: bmoe-server from the APK as a child process, five screens over its API
examples/ios    app/: the iOS app, bmoe_server linked and run on a thread, five SwiftUI screens over
                its API; BmoeKit/: the app's client side (API, chat, downloads, metrics), a Swift
                package tested on the host; harness/: bmoe-cli compiled into an app
```

Dependencies point inward: adapters depend on the port headers, the CLI and the server compose
them. The server and the web UI meet only at the HTTP API; the UI knows no engine parameter by
name, it renders whatever `GET /api/params` describes.
The pure-policy code (`config.cpp`, `expert_layout.cpp`, `tensor_names.cpp`) compiles with no native
dependency, so a subset of the project builds and is testable before llama.cpp is
fetched.

## Why the streaming seam needs no fork

Streaming experts serially needs three things from the inference engine. All three are
already public in llama.cpp:

1. **A hook at routing time.** `llama_context_params.cb_eval` is called for every graph
   node. We ask for the routing nodes (`ffn_moe_topk-<il>`); ggml computes and
   synchronizes each alone, then calls us back with the selected expert ids materialized.
   The route trace and [cache-aware dropping](expert-dropping.md) additionally ask for each
   layer's `ffn_moe_weights*-<il>` chain — and dropping and
   [substitution](cache-aware-substitution.md) are the two paths that *write into* a graph
   tensor's contents (the weights, and the ids) rather than only rebinding `->data`. See
   [seam.md](seam.md).
2. **The expert tensor pointers.** During a one-token warm-up we scan each graph node's
   sources for tensors named `blk.<il>.ffn_{gate,up,down}_exps.weight` and record the
   live `ggml_tensor*`. We then rebind their `->data`.
3. **The file layout.** `gguf_get_tensor_offset` (public) gives each tensor's byte offset
   so we can `pread` individual expert slices.

Loading memory-mapped (`load_mode = LLAMA_LOAD_MODE_MMAP`) with `use_extra_bufts=false` keeps the
weights in their native gguf layout (a repacked buffer would break the rebind). That is a public model
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

1. `open()` — load model (mmap on, repack off, experts on CPU); if streaming, read the
   expert layout off the file, install the router hook, do the capture warm-up, bind the expert
   source, clear the warm-up KV. Done **once** per model. It reads as its steps, in order —
   `load_model`, `read_model`, `build_hook`, `create_contexts`, `build_sampler`,
   `install_abort_callback`, `init_speculation`, `bind_streaming` (capture, offsets, dense
   weights, source init, release-mmap, traces), `open_prefill_device`, `arm_decode_traces`,
   `describe_run`, `warn_about_load` — each a method of the session's `Impl` that fails through
   one `Opening` context, so the order is visible in one screen and every early return is kept.
2. `generate()` — prefill the prompt, then greedily decode `n_predict` tokens, reporting
   per-token metrics. Callable repeatedly; the expert cache stays warm between calls (see
   [session.md](session.md)). Cancellable mid-flight via the abort callback. One call is a
   `Turn` (the turn's state and what it may have to undo) run through `render_prompt`,
   `tokenize_prompt`, `reuse_kv_prefix`, `prefill`, `decode` (each step: `draft`,
   `verify_decode`, `accept`, `emit`), `trim_speculative_kv`, `summarize` and `finish`.
   `decide()` is the other way to use an open session: one prefill and no decode, reading which of
   a list of choices the model would answer (off unless the session is opened with it; see
   [decide.md](decide.md)).
3. Destructor — tear down in order: I/O pool, context, hook, model, backend.

`run()` (core/src/engine/runtime.cpp) is a thin one-shot wrapper — open, one generate, close —
so the gates and the interactive session share the same code path.

Greedy sampling makes the output a deterministic function of the graph — the property the
[byte-identity gates](../tests/moe_gates.cpp) assert. That holds with the lossy knobs off. Under
[`--drop-cold-experts`](expert-dropping.md) and
[`--expert-substitute`](cache-aware-substitution.md) the hook edits routing weights or ids from
live cache state, which is not in the graph, so output becomes a function of the graph *and* the
run's history.
