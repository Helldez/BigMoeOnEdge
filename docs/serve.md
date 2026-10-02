# Serving the engine to agent tooling

The engine has no HTTP server. It serves prompt requests over the `--session` stdin protocol
([telemetry.md](telemetry.md#session-mode)), which is what the Android app drives. For desktop
agent tooling — opencode, or anything that speaks the OpenAI Chat Completions API — a small
bridge wraps that protocol in an OpenAI-compatible HTTP endpoint, without adding an HTTP stack
or any dependency to the engine itself.

- `scripts/bmoe-serve.py` — stdlib-only Python bridge and the single entry point: spawns
  `bmoe-cli --session`, translates `POST /v1/chat/completions` (streaming and not) and
  `GET /v1/models` onto the line protocol, and keeps the model loaded between requests so the
  expert cache stays warm. It resolves engine and model itself (`--engine`/`--model`, else
  `$BMOE_ENGINE`/`$BMOE_MODEL`, else the host build of a repo checkout, else a `bmoe-cli`
  sitting beside the script in a staged bundle), with friendly errors instead of a raw
  subprocess failure.

```bash
python3 scripts/bmoe-serve.py -m ~/llm/models/LFM2.5-8B-A1B-UD-Q4_K_M.gguf \
    --model-id lfm2.5-8b-a1b --port 8017 \
    --engine-args "--chatml --moe-stream --ctx-size 16384 --ubatch 512"
```

`BMOE_ENGINE` overrides the engine binary, e.g. a cross-built ARM64 bundle. Point `--host
0.0.0.0` to serve other machines on the LAN; the default is loopback only.

What the bridge does with client fields:

- `messages` are forwarded verbatim to the engine when the conversation is plain text (see
  *Session residency* below); non-text content falls back to the flattened single-prompt path.
  `--chatml` makes the engine render the model's own GGUF-embedded chat template.
- `max_tokens` (default 2048 — thinking models spend completion tokens on reasoning) becomes
  `n_predict`, and is **clamped to the bridge's `--max-tokens` ceiling**: a client asking for
  more output than fits beside its prompt would otherwise sit in the n_ctx window for tens of
  minutes. OpenAI clients treat `max_tokens` as a ceiling, so clamping is behavior-preserving.
  If a request still overflows `n_ctx`, the bridge retries once with the budget halved
  (floor 256) rather than failing — and if the prompt alone overflows, the error surfaces,
  because that genuinely needs a shorter conversation.
- Requests are serialised: the engine is one session, one generation at a time.
- `think` (default true) toggles reasoning for templates that honor it (`think_ctl: 'template'`,
  e.g. the qwen35 family). Off means no reasoning span in output or history — cheaper turns and a
  history the append-reuse path can match. Templates that cannot be silenced (LFM2.5) ignore it.
- `preserve_reasoning` (default false) asks the template to keep reasoning in re-rendered history
  turns (LFM2.5's `preserve_thinking`). Echo the reply's reasoning back inside the assistant
  content (`<think>{reasoning}</think>{answer}`) and every following turn becomes an append:
  measured on LFM2.5, a continuation turn prefills **17 tokens instead of the whole conversation**
  (`n_reused` grows 101 → 235 → …). Without the echo, the next render diverges at the first
  reasoning token and the turn re-prefills from there — the fallback is always safe. The bridge
  response's `reasoning_content` field carries exactly the text to re-embed.
- With `--auto-echo` the bridge does that re-embedding itself ([ADR-003](adr/003-bridge-auto-echo.md)):
  assistant history turns matching a reply the bridge generated are rewritten to the exact
  generated span, so unmodified OpenAI clients get the same append reuse. Exact-match only —
  edited or regenerated answers fall back to the safe full re-prefill. Against aider the
  pipeline also canonicalizes the message array (aider appends its edit-format boilerplate to
  the newest user turn only, so prior turns shrink between requests): the boilerplate is
  stripped from user turns and relocated into the system prompt once, making the canonical
  history request-independent. Measured with aider on LFM2.5: edit turns still full-clear
  (aider re-adds the edited file with new content — semantically required, and a hybrid
  cannot rewind), but pure-question follow-ups prefill ~26 tokens reusing ~1700 in ~1.6 s
  instead of ~40 s of full prefill.
- Non-streaming responses carry a `bmoe` object with the `BMOE_DONE` perf block (tok/s, cache
  hit, stall) — the same numbers the CSV sink records. The SSE stream emits only OpenAI-shaped
  chunks: strict client SDKs validate every event, so no vendor-specific events ride the stream.

## Session residency: second turns are cheap

Plain-text conversations are forwarded to the engine **verbatim** (`messages` array, not the
flattened string), so the engine owns the conversation state and the bridge stays stateless. The
engine renders its chat template over the full array each turn and keeps the KV prefix of the
longest common conversation — compaction, edited messages and retries reduce to the same
truncate-and-extend path, and no cache-coherence logic exists anywhere because every request
carries the authoritative history.

The effect on a prefill-bound host is the whole point: the first turn prefills everything, and
every following turn prefills **only the new tokens** — a turn that appends one message re-prefills
a few hundred tokens instead of the whole conversation. `BMOE_DONE` reports this as `n_reused`
(KV prefix carried over) alongside `n_prompt` (tokens actually prefilled this turn). Conversations
with image parts or non-text content fall back to the flattened one-shot prompt path.

**Hybrid/recurrent architectures (lfm2moe, qwen35moe, …) are excluded from prefix reuse:** a
partial `seq_rm` rewinds positions but not the per-sequence cell state carried in the same memory,
so decoding after a mid-sequence rewind fails outright (`llama_decode: failed to decode, ret = 2`,
first seen as dead LFM2.5 sessions the moment a client rewrote its history). Those models
re-prefill every turn — the pre-residency behavior — while keeping the engine-held conversation,
and a cancelled turn likewise forces the next turn's full re-prefill there.

**One exception: pure appends.** When a hybrid turn's rendered prompt is a strict extension of the
resident token mirror — the client echoed the previous assistant reply verbatim and only added
messages — the diff finds the full prompt already resident and no `seq_rm` runs at all. Cells only
ever grow, so the reuse is as safe as a first prefill, and a continuation turn skips its whole
prefill. Two properties bound this in practice: the mirror includes the *generated* tokens, so the
client must echo the previous reply verbatim (aider and the bridge do; hand-built histories do
not); and any failed, cancelled or overflowed turn resets the mirror so the next turn full-clears —
cell state may sit past what the mirror describes. Thinking hybrids (LFM2.5) cannot hit this path
at all: the reasoning tokens live in the cache between the prompt and the answer, but a client
echoes only the answer, so the render always diverges before the append point. A third blocker is
template-side: qwen3.5's template silences thinking by baking an empty `<think>` span into the
generation prompt, so even with thinking off the next turn's re-rendered history (span-less)
diverges from what was generated (span included). Append reuse on hybrids therefore needs a
non-thinking stack whose template neither reasons nor bakes the span — rare, and the reason the
snapshot path below is the general fix.

**Experimental: snapshot-rollback reuse (`--rs-seq N`).** Upstream llama.cpp can snapshot
recurrent state per token (`n_rs_seq` budget, marked `[EXPERIMENTAL]`), which makes a bounded
partial `seq_rm` legal: the engine sets the budget at context creation when asked, and the generic
diff path then serves hybrids like transformers — rewind within budget restores from snapshots,
beyond it fails `seq_rm` and falls back to the same full clear. Measured on a 9B qwen35 (gated
delta net) at N=32: reuse engages (`n_reused` > 0 on continuation turns). The default is
**0 (off)**; the flag exists to track the upstream fix, and flipping it on is a deliberate
opt-in. A second
datum: lfm2moe is on upstream's rollback allowlist, but with snapshots enabled its graph
**crashes during reserve** — a fixed ~368-byte node-pool overflow (invariant to context, ubatch
and budget), i.e. upstream's node estimate does not cover the snapshot ops that graph emits.
Three upstream events gate hybrid reuse: state restore worth more than zero (ggml-org/llama.cpp
#25913), snapshot planes that survive single-token decode steps (see below), and the lfm2
reserve sizing.

**Why default-off: snapshot-plane staleness (measured 2026-09-18, ADR-004 Addendum 2).** An
earlier claim here — restore is not bit-exact, blamed on mid-chunk snapshots of the chunked
delta-net — was a harness artifact (the comparison wiped its own pending rollback) and is
withdrawn. With a corrected matched-feed harness, upstream's restore roundtrip is exact when
the rollback directly follows the last multi-token prefill ubatch, on both GDN families and
at every depth tested. What breaks it is what every real turn looks like: single-token decode
steps between the prefill and the rollback. A ubatch writes only `min(n_seq_tokens, K)`
snapshot planes, so a 1-token step refreshes only plane 0 and the depth-d plane the rollback
reads stays m tokens stale — restored streams diverge from the ground truth on both qwen35
and lfm2moe. Until upstream replays the trailing tokens through one ubatch before restoring
(or maintains the planes per token), the engine keeps the full-clear fallback.

## Warmup: the first query is cheap too

Residency helps from the second turn on; the **warmup** moves the first turn's cost to server
start. After each request the bridge saves the stable prefix — everything but the in-flight user
turn — to `~/.cache/bmoe-serve/warmup.json` (mode 0600; it holds conversation contents). At
startup the bridge replays that prefix through the messages path as short prefill-only segments
(`n_predict=0`), so the model's own template renders what the next session will send and the
engine holds it resident before any client connects. A client's first query then diffs against
the warm prefix and prefills only its own delta.

The design is speculative-safe by construction: if the next session sends a different system
prompt, tool schema or repo map, the residency diff truncates at the first divergence and only
the mismatched tail is prefilled — a mismatch wastes compute, never correctness. The saved prefix
is keyed to the model file, so switching models skips the replay, and `--no-warmup` disables it.
Segments release the engine lock between segments, so a request arriving mid-warmup preempts the
rest without losing what is already resident.

For observability the bridge prints one `TELEMETRY` line per request to its log — `n_prompt`,
`n_reused`, `tokens`, `prefill_s`, `tok_s`, `wall_s`, … — because agent clients (aider, opencode)
discard the perf block.

## Host results — the residency/warmup effect, measured

All rows from a 2015-era 8-thread laptop (12 GB RAM, SATA-class storage), `--ctx-size 16384
--ubatch 512 --moe-stream`, served through the bridge. These measure the *serving* mechanics
(warmup, residency, expert I/O), not peak throughput — see [benchmarks.md](benchmarks.md) for the
device matrix and its fixed protocol.

| Model (arch) | Load | Cold 1st turn | Warm turn | Decode | Notes |
|---|---|---|---|---|---|
| Ling-mini-2.0 Q4 (`bailingmoe2`, 16.5B/1.4B) | 27 s | 58 s | **12 s** (202 reused / 34 prefilled) | 3–4 tok/s | no thinking phase; best quality/speed balance |
| OLMoE-1B-7B Q4 (`olmoe`, 6.9B/1.0B) | 12 s | 17 s | **3 s** (219 reused / 21 prefilled) | 9–14 tok/s | wordy, older; lowest RAM (≈4.7 GB headroom) |
| Laguna XS 2.1 Q4 (`laguna`, 33B/3B) | 61–96 s | 127 s | 15–24 s | ~1.4 tok/s | strongest quality; cold-expert I/O dominates |

Reading the numbers:

- **Warm turns are the design point.** The delta between cold and warm is the expert I/O plus the
  prefix prefill that residency removed; what remains is decode plus a fixed per-request cost as
  the expert cache churns. On a model whose whole file fits in page cache (OLMoE, 4.2 GB), warm
  prefill reached 22 tok/s and the warm turn is decode-bound pure and simple.
- **Warmup frontloads the first turn.** Same conversation replayed after a restart: 219 tokens
  prefilled cold → **12 prefilled / 207 reused** with warmup replayed at startup.
- **Prefill speed is page-cache-bound, not compute-bound, on this host.** The same model measured
  3.7 tok/s prefill cold and 27 tok/s warm within one session; the engine's O_DIRECT path wins
  when RAM cannot hold the file, and the page cache wins when it can.
- **Client interaction quality (measured, same host):** aider's `whole` edit format applies
  single-turn edits reliably on Ling-mini but multi-turn rewrites silently drop earlier changes;
  the `diff` format exceeded the model's ability (aider retried until timeout). opencode's
  tool-call editing completed, but at ~1B-class quality the model hallucinated tool output —
  treat these models as Q&A/drafting engines, not autonomous agents.

### Measuring the features: `scripts/bench-features.sh`

One command re-proves the residency features on any model: it serves through the bridge on an
isolated port (default 8019 — a daily driver on another port is never touched), runs a 3-turn
arithmetic chain with **verified answers** (42/52/62; a faster run with wrong answers reports
FAIL, never a win), and prints one markdown row per feature off vs on:

- **warmup** — `--no-warmup` vs the startup replay (the first chain seeds the warmup file).
- **reasoning echo** — `preserve_reasoning` with the reply's reasoning re-embedded as
  `<think>…</think>answer`. Skipped *behaviorally* when the model emits no `reasoning_content`
  (a template merely mentioning thinking is not evidence).
- **`--rs-seq`** — snapshot rollback, answers verified against the fresh-prefill ground truth.

Measured on Ling-mini-2.0 (same host as the table above):

| scenario | T1 prefill s | T2 prefill s | T2 reused | T3 prefill s | T3 reused | T1 tok/s | verdict |
|---|---|---|---|---|---|---|---|
| warmup-off | 2.19 | 0.90 | 34 | 1.10 | 61 | 9.4 | ok |
| warmup-on | **0.06** | 0.72 | 34 | 0.71 | 61 | **16.1** | ok |
| rs-seq-off | 0.07 | 0.71 | 34 | 0.69 | 61 | 13.4 | ok |
| rs-seq-on | 0.06 | 0.74 | 34 | 0.71 | 61 | 15.9 | ok |

Warmup cuts the first turn's prefill ~35×; the echo scenario skipped (Ling-mini does not think);
`--rs-seq` is a verified no-op on a pure transformer — the flag only engages on hybrids, where
the script's answer verification is what guards against the snapshot-plane staleness the
rollback path still has after single-token decode steps (ADR-004 Addendum 2).

Same script, thinking hybrid (LFM2.5-8B-A1B, `N_PREDICT=192` — reasoning needs headroom):

| scenario | T2 prefill s | T2 reused | T3 prefill s | T3 reused | verdict |
|---|---|---|---|---|---|
| echo-off | 2.46 | 0 | 3.59 | 0 | ok (full clear every turn) |
| echo-on | **1.38** | 124 | **1.27** | 205 | ok (append reuse on a thinking hybrid) |

The `--rs-seq` rows reproduce the documented lfm2moe graph-reserve crash at load — the script
reports the engine death instead of hiding it. Thinking models also expose a benchmarking trap
the script now documents: with a small completion budget the reasoning consumes it and no answer
is ever emitted, so verification fails regardless of cache behavior.

## Memory budget on the host

Compute buffers are reserved for the widest graph, and the dominant term scales with
`ubatch × vocabulary` — at ctx 8192 on LFM2.5-8B that measured **4.1 GiB** on a desktop host,
enough to OOM-kill the session the first time a generation ran with ~4 GiB free. `--ubatch 512`
caps the reservation at ~258 MiB and leaves decode speed untouched (only prefill splits into
more graphs). On RAM-constrained hosts, keep `--ubatch 512` and let `--cache-mb auto` size the
expert cache to what is left.

The batch width is the same trap one level up. The prefill batch defaults to the whole context
(the right shape for chat-sized prompts: one graph per prompt), but the output buffer scales
with `batch × vocabulary` — a 32k-token document on a ~248k-token vocabulary asks for ~30 GiB
of logits and fails to reserve at all. Pair `--batch 512` with `--ubatch 512` on any long-
context server (`--ctx-size 32768` and up); the prompt is then fed in ubatch-wide slices.
Scoring and answers are chunk-invariant — each position attends to the same KV either way —
so nothing about the model's behavior changes, only the reservation shrinks to what the
compute buffers already reserved for the ubatch.

The KV cache is the third term, and on a large-context model it is the one that decides
whether the host survives. It scales linearly in `ctx-size`, and **sliding-window
attention does not reduce it**: llama.cpp allocates SWA layers at full context rather than
window width, so a model advertising `sliding_window = 512` can still cost full-price KV
per token. Budget it from the measured allocation, not from the architecture's metadata —
Poolside Laguna XS 2.1 measures **160 KiB/token f16** (40 full-attention layers), i.e.
5 GiB at 32k and 20 GiB at 128k.

### The three allocations stack, and anon memory is what kills you

A `--moe-stream` run holds three separate large allocations at once, and they are
**additive**:

1. the KV cache,
2. the dense (non-expert) weights, and
3. the expert LRU cache.

The failure mode is specific and worth naming. Under the default `--dense-weights anon` the
dense set is read `O_DIRECT` into *anonymous* memory, so it is reclaimable only to zram —
never dropped, never refaulted cheaply. `--cache-mb auto` then sizes the expert cache from
`MemAvailable` **before** those dense buffers exist, and `MemAvailable` counts the model's
own mmap'd weights as free (see [cache-sizing.md](cache-sizing.md)). The run therefore
over-asks by roughly the dense size. When the sum exceeds RAM the kernel does not fail the
allocation politely — it OOM-kills the largest anonymous process, which is yours.

Recognise it in `dmesg` / `journalctl -k` by the shape of the victim:

```
Out of memory: Killed process 1234 (bmoe-cli) total-vm:40839128kB, anon-rss:14399608kB, file-rss:8kB
```

`file-rss` near zero on a multi-gigabyte model is the signature: the weights are in anon
buffers, so nothing about the process is cheap to reclaim.

Keep the sum inside RAM on a host with little headroom:

- `--cache-type-k q8_0 --cache-type-v q8_0` — halves the KV term (measured on Laguna at
  32k: 5120 → 2720 MiB). Quantized V requires flash attention, which the CLI enables for
  you.
- `--batch 512 --ubatch 512` — caps the logits term on long-context servers.
- `--dense-weights mmap` (or an explicit small `--cache-mb`) — stops the anon dense copy
  from sitting on top of the cache. `anon` is the right default on a phone with zram and
  fast flash; on a host without them it converts reclaimable memory into unreclaimable
  memory.

## Prefill is the wall on small hardware

Prompt processing (prefill) is compute-bound, and on modest CPUs it dwarfs everything else:
a 2015 dual-core laptop measured **~20 tok/s**, so an agent client that re-sends its multi-
thousand-token system prompt pays minutes before the first token. The engine-side knobs do
not move it — decode threads (2 vs 4), `--ubatch` 256/512/1024, and the CPU governor all
land within run-to-run noise; `--n-expert-used` does, but it drops the model's real routing
and answer quality with it (a benchmark knob, not a service one). The lever that works is
shrinking what the client sends, which is a client configuration problem:

- Set the model's real limits so agent clients compact before overflowing
  (`context` deliberately below the engine's `--ctx-size` to leave output headroom):

```jsonc
"lfm2.5-8b-a1b": {
  "name": "LFM2.5-8B-A1B (bmoe streamed)",
  "limit": { "context": 12288, "output": 2048 }
}
```

- Prefer a minimal agent profile: a ~100-token custom prompt instead of the client's full
  agent stack, and fewer enabled tools — every enabled tool's schema rides in the prompt on
  every request. In opencode that is a `"mode": "primary"` agent with a short `prompt`, the
  mutating tools disabled, and the bmoe model pinned.
- Keep conversations short: clients re-send the whole history each turn, and `/new` is
  cheaper than any cache.

## ARM64 Linux bundles

`scripts/build-arm64.sh` cross-compiles the engine for ARM64 **GNU/Linux** (needs the
`aarch64-linux-gnu` GCC toolchain) and stages a self-contained `bmoe-arm64/` bundle — the CLI
plus its shared libraries, RUNPATH `$ORIGIN/lib`, no `LD_LIBRARY_PATH` required:

```bash
scripts/build-arm64.sh            # stage bmoe-arm64/ (and bmoe-arm64.tar.gz with --tar)
scp -r bmoe-arm64/ target:/opt/   # then on the target:
/opt/bmoe-arm64/bmoe-cli -m model.gguf --moe-stream -p "hello"
```

Baseline is `armv8.2-a+dotprod+fp16` (any 2018+ ARM64 SoC; no i8mm, so older SoCs do not
SIGILL). The expert-ready hook is compiled in, so `--overlap` works. This is **not** the
Android build — Android binaries link bionic and only run on Android. See the bundle's own
`bmoe-arm64/README.md` for running the server from it (`BMOE_ENGINE=bmoe-arm64/bmoe-cli`).

Configuring the client (opencode) is a user-level concern, not a repo one: add an
`@ai-sdk/openai-compatible` provider pointing at `http://127.0.0.1:8017/v1`, either per project
or in `~/.config/opencode/opencode.json` for every session, and give the model the `limit`
from the prefill section above.
