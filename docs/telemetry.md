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
- `wall_ms` = total token time; `io_ms` = flash read time; `compute_ms` = `wall_ms − io_ms`.
  In serial mode `io_ms` is the wall time blocked on reads (a subset of `wall_ms`). Under
  `--overlap` its meaning changes: it is the **sum of per-lane busy time**, so it can exceed
  `wall_ms` because lanes read in parallel with compute. Use `stall_ms` for the wall time
  compute actually lost to reads under overlap.
- `cache_hit_pct` is the cumulative cache hit rate, or `-1` when no cache is used.
- `text` is the full generated text so far, JSON-escaped (for streaming into a UI).

## End-of-run lines

```
=== answer ===
<full generated text>
=== perf ===
generation: <n> tokens, <s> s/token (<t> tok/s)
moe-stream: read <mib> MiB (<mib/tok> MiB/token), decode <s> s/token (compute <c> + flash I/O <i> s/token, <bw> MiB/s)
moe-cache: <pct>% hit, resident <mib> MiB
```

Under `--overlap` the `moe-stream:` line additionally reports `stall_s/tok=<s>` — the mean
wall time per token that compute threads waited for expert reads to complete. It is `0` in
serial mode (where the read wait is already folded into decode time).

`moe-cache:` is present only when a cache is active. The `=== answer ===` / `=== perf ===`
banners appear only in `--progress` mode; without it the CLI streams the answer inline and
prints just the summary lines.

## CSV sink

`--csv PATH` additionally writes one row per token:

```
step,steps,wall_ms,io_ms,compute_ms,read_bytes,cache_hit_pct,stall_ms
```

followed by a `# summary ...` comment line. Intended for the benchmark sweep.

`stall_ms` is a trailing column added for `--overlap`: the wall time compute threads waited
for expert reads that token. It is `0` in serial mode, and older CSVs written before it was
added have only the first seven columns — consumers must treat it as optional. The
`# summary` line likewise gains `stall_s/tok=<s>` under overlap (see the `io_ms` note above
for how the read-time columns are reinterpreted when lanes run in parallel with compute).
