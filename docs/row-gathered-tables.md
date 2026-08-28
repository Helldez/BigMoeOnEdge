# Row-gathered dense tables (`--row-stream`)

A dense weight that is *multiplied* is read whole, every token: making it resident is the only
sensible policy, and that is what `--dense-weights` decides between. A dense weight the graph only
**gathers rows from** is a different object. The token embedding table is the pure case, one row
per decoded token out of a vocabulary of hundreds of thousands, and on a big model it is hundreds
of MiB of RAM bought for a kilobyte of use per token.

`--row-stream` binds such a table to *reserved address space* and pulls in only the rows the graph
is about to read, from flash. Measured on the 12 GB test phone:

| model | table | resident dense, off | resident dense, on | flash read for it |
| --- | ---: | ---: | ---: | ---: |
| Qwen3.6-35B-A3B UD-Q3_K_XL | `token_embd` 515 MiB | 2436 MiB | 1921 MiB | 0.4 MiB / 12 tokens |
| Qwen3.8-Flash-Next UD-IQ3_XXS | `token_embd` 497 MiB | 4313 MiB | 3816 MiB | 0.4 MiB / 24 tokens |

Output is byte-identical in every cell. That is not a quality claim but a structural fact: see
[Why identity is the whole test](#why-identity-is-the-whole-test).

## Which tables qualify, and why there is no list of them

Nothing in the engine names a tensor. During the streamer's capture decode every graph node is
already offered to the eval callback, and each reference to a dense weight is classified by *how
the node used it*. A table qualifies only when both hold:

1. **Every** reference to it in the captured graph is a row gather (`GGML_OP_GET_ROWS`) with the
   table in the source position. One reference of any other kind disqualifies it.
2. Every such gather's index tensor is already materialized when the node runs: a graph input, or
   a pure view of one. An index computed inside the graph cannot be read before the node that
   produces it has run.

Both conditions are properties of the graph, so the rule carries to architectures nobody had in
mind when it was written:

- A model with **tied embeddings**, where `token_embd` is also the output head, fails rule 1 on
  the head's matmul and keeps the residency it had. No special case, on any architecture.
- Qwen3.8-Flash-Next's **n-gram table** (`per_layer_token_embd`, ~27 GB) *is* row-gathered, but by
  hashes computed inside the graph, so it fails rule 2 and falls through to the size guard that
  was measured for it: mmap'd with random-access advice (see [architecture.md](architecture.md)
  and the 0.22.0 changelog entry).

The policy is also restricted to tables that could have been resident at all, which is to say it
runs after that size guard. That is deliberate: the fallback below has to be able to pull a whole
table in, and a table larger than memory could not honour it.

## Mechanism

The reservation mirrors the tensor's own byte layout, so ggml's address arithmetic
(`data + i*nb1`) is unchanged and the gather kernel is untouched. It is the same trick the expert
cache plays on `mul_mat_id`, at row granularity instead of expert granularity.

- **Slabs, not rows.** A row is ~1 KiB, far below any device's request floor, so the unit fetched
  is a 16 KiB page-aligned slab: a read that size costs what the floor costs anyway, and adjacent
  vocabulary rows recur. This is a property of the storage, not of a model.
- **A bounded window.** `--row-stream-mb` (default 64) caps what all row-streamed tables hold
  resident together, evicted LRU. In practice a run never approaches it, since the tables above
  hold well under a MiB, but the ceiling is what makes the residency claim a guarantee rather than
  a hope. A single gather that alone exceeds the window overshoots it for that one node rather
  than reading a row at a time, and the slabs the gather in flight needs are never evicted under
  it.
- **Ordered reads.** A gather's slabs are deduplicated and issued in ascending file order.
- **A fallback with teeth.** `IRowSource::materialize()` pulls the whole table in and says so on
  stderr. It fires if a graph shape the capture pass never saw reads a served table any other way,
  or if a gather's index turns out to be unreadable. The engine is then simply back to the
  behaviour it would have had without the flag.

The policy applies **under every `--dense-weights` mode**, because what it changes is not how a
mode works but which tensors the mode is applied to. A table it takes over leaves the resident
dense set entirely: not read, not rebound, not warmed, and outside the residency sensor. A sensor
that counted it would report a set that is resident by design only in part.

## Why identity is the whole test

The tensor is bound to **reserved**, not allocated, memory. A row the policy failed to fetch is
therefore not a slightly wrong weight: it is memory that was never written, and the output
diverges on the first token that touches it. Byte-identity to the resident reference is proof that
every gathered row arrived.

Gates `G15a` and `G15b` in `tests/moe_gates.cpp` assert exactly that on all three tiny models,
`G15b` with a window of one slab so that nearly every gather has to re-read what the previous one
handed back. What a gate cannot prove is that a table qualified at all. That is what the run's own
`moe-rows:` line reports, on the real model.

## What it costs, and what it is for

The read cost is negligible in both directions measured: 0.4 MiB across a whole generation,
against 4.5 GB of expert reads on Qwen3.8. Expert bytes, cache hit rate, evictions and re-reads
are identical to the digit with the flag on and off, so the expert path is not touched.

Throughput is, so far, **neutral**. Interleaved host cells on Qwen3.6-35B-A3B Q4_K_M gave 2.22 and
2.30 tok/s with the flag off, 2.27 and 2.31 with it on, a spread smaller than the one between the
two baseline runs. The flag is off by default until a long on-device run says more.

What it is actually for is the RAM. On the 12 GB phone, Qwen3.8-Flash-Next pins 4.3 GB of dense
weights, and the kernel's reclaim accounting cannot see them: `MemAvailable` falls under ~0.9 GB
and the anonymous expert cache starts being compacted to zram, which is what a 2 to 7 second token
is (see the 0.22.0 changelog entry). Half a gigabyte handed back to the system addresses that
cause directly, and is also half a gigabyte the expert cache can take instead.

## Flags

```
--row-stream        serve row-gathered dense tables from flash instead of RAM
--row-stream-mb N   resident window for those tables, in MiB (default 64)
```

Requires `--moe-stream`, since the tables are discovered by the capture pass and fed by the eval
callback. In the Android app: **Stream row-gathered tables**, next to the dense-weight mode.

Telemetry: the `moe-rows:` end-of-run line and the `row_*` keys in the CSV summary trailer, both
in [telemetry.md](telemetry.md).
