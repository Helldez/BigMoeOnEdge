# Telemetry contract

With `--progress`, `bmoe-cli` emits machine-readable lines the Android app (and any other
consumer) parses. The format is versioned by this document; keep it stable.

## Per-token lines

Emitted once per generated token, in order:

```
BMOE_LOAD {"mb":<float>,"ms":<float>}
BMOE_PROGRESS {"step":<int>,"steps":<int>,"wall_ms":<float>,"io_ms":<float>,
               "compute_ms":<float>,"cache_hit_pct":<float>,"text":"<string>"}
```

- `BMOE_LOAD` appears only when experts were read this token; `mb` is the flash bytes read,
  `ms` the read time.
- `BMOE_PROGRESS.step`/`steps` are 1-based index and target token counts.
- `wall_ms` = total token time; `io_ms` = flash read time. `compute_ms` is a **residual, not a
  measured quantity**: there is no clock around llama.cpp's matmul kernels (adding one would mean
  patching the submodule), so compute is whatever wall time is left after the measured terms are
  subtracted — `wall_ms − io_ms − mgmt_ms` in serial, `wall_ms − stall_ms − mgmt_ms` under overlap.
  In serial mode `io_ms` is the wall time blocked on reads (a subset of `wall_ms`). Under
  `--overlap` its meaning changes: it is the **sum of per-lane busy time**, so it can exceed
  `wall_ms` because lanes read in parallel with compute. Use `stall_ms` for the wall time
  compute actually lost to reads under overlap.
- `mgmt_ms` (not emitted in the JSON, but folded into the `compute_ms` residual above and written
  to the CSV) is the cache-management time this token: virtual-memory commit of newly cached
  pages, eviction of cold pages, and the LRU bookkeeping. On the first few tokens after prefill it
  can be a large share of the token; at steady state it is near zero. Surfacing it stops the "all
  compute" reading on warm-up tokens where the real cost is cache churn, not matmul.
- `cache_hit_pct` is the cumulative cache hit rate, or `-1` when no cache is used.
- `text` is the full generated text so far, JSON-escaped (for streaming into a UI).

## End-of-run lines

```
=== answer ===
<full generated text>
=== perf ===
generation: <n> tokens, <s> s/token (<t> tok/s)
moe-stream: read <mib> MiB (<mib/tok> MiB/token), decode <s> s/token (compute <c> + cache mgmt <m> + flash I/O <i> s/token, <bw> MiB/s)
moe-cache: <pct>% hit, resident <mib> MiB
```

`compute` in the `moe-stream:` line is the same residual described for `compute_ms` above; `cache
mgmt` is the per-token mean of `mgmt_ms`.

With `--prefetch K` a `moe-prefetch:` line is added:

```
moe-prefetch: <mib> MiB speculative, <useful>/<prefetched> experts useful (<pct>%)
```

`<mib>` is the flash read done speculatively this generation (a subset of the total read),
`<prefetched>` the experts fully read ahead, and `<useful>` how many of those a later routing
actually hit. See [prefetch.md](prefetch.md).

With `--spec-gate` a `moe-spec-gate:` line is also added:

```
moe-spec-gate: <pct>% router prediction recall
moe-spec-gate: <pct>% router prediction recall (auto-disabled below <P>%)
```

the fraction of experts the speculative router predicted that the layer actually routed; the
`(auto-disabled below <P>%)` suffix appears when the recall self-governor turned the feature off
mid-run. Note the spec-gating prediction runs on its own worker thread, so its CPU time is **not**
part of the `compute` residual above — only the eval-thread hidden-state snapshot is. See
[spec-gating.md](spec-gating.md).

Under `--overlap` the `moe-stream:` line additionally reports `stall_s/tok=<s>` — the mean
wall time per token that compute threads waited for expert reads to complete. It is `0` in
serial mode (where the read wait is already folded into decode time).

`moe-cache:` is present only when a cache is active. The `=== answer ===` / `=== perf ===`
banners appear only in `--progress` mode; without it the CLI streams the answer inline and
prints just the summary lines.

## CSV sink

`--csv PATH` additionally writes one row per token:

```
step,steps,wall_ms,io_ms,compute_ms,read_bytes,cache_hit_pct,stall_ms,mgmt_ms
```

followed by a `# summary ...` comment line. Intended for the benchmark sweep.

`stall_ms` and `mgmt_ms` are trailing columns appended (in that order) after the original seven.
`stall_ms` is the wall time compute threads waited for expert reads that token (`0` in serial
mode); `mgmt_ms` is the cache-management time described above. Both are additive: older CSVs have
fewer columns, so consumers must read by column NAME (from the header row) and treat either as
optional. The `# summary` line likewise gains `stall_s/tok=<s>` and `mgmt_s/tok=<s>` (see the
`io_ms` note above for how the read-time columns are reinterpreted under overlap).

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
default to the process's flags / `true`. `cancel` may arrive at any time, including mid-generation.

Responses (stdout):

```
BMOE_READY {"load_s":<float>,"arch":"<string>","n_ctx":<int>}          # once, after the model loads
BMOE_BEGIN {"id":<int>}                                                # a generation started
BMOE_LOAD / BMOE_PROGRESS ...                                          # per token, as above
BMOE_DONE  {"id":<int>,"cancelled":<bool>,"tokens":<int>,"tok_s":<float>,
            "prefill_s":<float>,"load_s":<float>,"cache_hit_pct":<float>,"text":"<string>"}
BMOE_ERROR {"id":<int>,"fatal":<bool>,"msg":"<string>"}
```

`BMOE_DONE` carries the end-of-generation summary (the one-shot mode's `generation:` /
`moe-stream:` text lines are not emitted in session mode). `BMOE_ERROR` with `fatal:false` is a
rejected request (e.g. the prompt plus `n_predict` exceeds `n_ctx`) and leaves the session usable;
`fatal:true` means the process is ending.
