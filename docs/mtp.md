# MTP decoding (`--mtp`)

`--mtp` lets the model draft its own continuation with the multi-token-prediction (MTP) head
trained into the gguf, then verifies every drafted token in a single wider decode. Off by default.

Unlike [turbo top-k](../README.md#trading-quality-for-speed) and
[expert dropping](expert-dropping.md), this one is **not** a quality trade: a draft is accepted only
when it equals the target model's own argmax at that position, so nothing is approximated and no
weight is skipped. The quality is the full model's.

## It is exact, but it is not bit-reproducible

Read that claim precisely, because it is weaker than the one the rest of this engine makes.

`--overlap`, `--prefetch` and the whole streaming path are **byte-identical**: they change how
weights reach the CPU and nothing else, and the [gates](../tests/moe_gates.cpp) prove the streamed
output equals the resident one exactly. `--mtp` is not in that class. Verification evaluates
`1 + N` positions in a single batch, and a batched matmul is not bit-identical to `N` separate
single-token matmuls — different blocking, different summation order, different last bits. On a
near-tie between two candidate tokens, that is enough to flip the argmax. The accepted sequence is
always exactly the argmax sequence **of the batched forward pass**; that pass is just not the same
arithmetic as the unbatched one.

Measured on Qwen3.6-35B-A3B-MXFP4, 128 greedy tokens, streamed with `--overlap --cache-mb auto`:

| comparison | result |
|---|---|
| plain vs plain (control) | identical, 608/608 chars |
| **plain vs plain at `--ubatch 1`, no MTP at all** | **differs, 608/612 chars** |
| plain vs `--mtp --draft 1` | diverges at char 151 — one token |
| plain vs `--mtp --draft 3` | diverges at char 151 — the same token |
| `--draft 1` vs `--draft 3` | **identical** |

The second row is the one that settles it, and it involves **no speculation**: `--ubatch 1` only
forces the prompt to be prefilled one token at a time instead of in one wide pass. Same model, same
weights, same flags otherwise — and the greedy output already changes. Batch width moves the last
bits on this backend, full stop; speculation merely makes every decode a wide batch and inherits it.

The last row rules out the other candidate. Rolling back a rejected tail has to restore recurrent
state (Qwen3.5/3.6 is hybrid, which is why the target context is created with `n_rs_seq`), and a
broken rollback is a real failure mode — it is what SGLang diagnosed on Ascend NPU
([#25587](https://github.com/sgl-project/sglang/issues/25587)). But a rollback bug scales with how
much gets rolled back: draft 1 rewinds at most one position, draft 3 up to three. Those two runs are
byte-identical to each other, so the rollback is not what is moving the output.

One token differed (`Request` → `Query`), the continuation stayed coherent, and every speculated run
agreed with every other regardless of draft width. A logic error in the accept/rollback path would
do neither of those things: it would vary with the draft width and compound over the generation.

This is expected, not a defect of this engine, and it is documented upstream:

- **llama.cpp**, in its own server README, on why prompt caching can change results: *"Because
  (depending on the backend) the logits are **not** guaranteed to be bit-for-bit identical for
  different batch sizes (prompt processing vs. token generation) enabling this option can cause
  nondeterministic results."* Speculation makes every decode a wide batch, so it lands in exactly
  that case.
- **vLLM**, whose speculative decoding is validated by a "Greedy Sampling Equality" test, still
  states that it is *"theoretically lossless up to the precision limits of hardware numerics"* and
  that *"changes in batch size may cause variations in logprobs and output probabilities."*
- The failure mode of a genuinely **broken** batched speculative implementation looks different:
  ["Batch Speculative Decoding Done Right"](https://arxiv.org/abs/2510.22876) describes
  desynchronised position ids, attention masks and KV state producing repetitive tokens or
  gibberish and *near-zero* exact-match against standard decoding — not one flipped near-tie inside
  an otherwise identical transcript. A duplicated token (`"renowned for for"`) is the classic
  accept/rollback off-by-one; a substituted one is not.

Whether it shows up at all is a property of the **backend**, not of the idea. SGLang reports MTP
output identical to non-speculative decoding on NVIDIA, where the kernels happen to be
batch-invariant, and divergent on Ascend NPU. On ggml's CPU path — GEMV for one token, blocked GEMM
plus `mul_mat_id` expert grouping for several — batch invariance does not hold, as the `--ubatch 1`
row above shows directly. Do not port the NVIDIA expectation here.

So: same model, same weights, no approximation — but do not expect a `--mtp` transcript to match an
unspeculated one character for character, and do not use it in a byte-identity gate.

## Why a wider decode can be faster

A decode's cost is dominated by moving weights, not by arithmetic. One token reads the dense weights
plus its routed experts and does a handful of GEMVs against them; the hardware spends its time
waiting for bytes. Verifying `N` positions in one decode reads those same weights **once** and turns
the GEMVs into small GEMMs. If the draft is usually right, the engine confirms several tokens for
roughly the cost of one.

That is the same lever in both regimes this engine runs in, for different reasons:

- **DRAM-bandwidth-bound** (the desktop host): the dense weights are re-read from DRAM every token,
  and that traffic is the measured ceiling. Amortising it over a group attacks the bottleneck head-on.
- **Flash-streamed** (a >RAM model on a phone): what binds decode is latency-to-ready of the expert
  slices, not raw bandwidth ([the sidecar refutation](benchmarks.md)). A group pays that latency once.

## Why it can lose

The `N` verify positions route **independently**. Each one picks its own top-k experts, so a layer's
read set widens from `k` toward `N × k` distinct experts wherever routing diverges across adjacent
tokens. In the streamed regime those extra experts are extra flash reads, and they can cost more
than the amortisation saves. The draft itself is not free either: the MTP block carries its own MoE
FFN, so every draft pass routes through one additional expert layer.

The engine therefore treats it as a measurement, not an improvement: default off, and the numbers
below decide whether it ships enabled on a given device.

## Requirements

The gguf must carry the `nextn` block — and on Qwen3.6 the **ordinary quantisations already do**.
Verified from the gguf headers: bartowski's `Q4_0` and `Q4_K_M` and unsloth's `MXFP4_MOE` are all
`qwen35moe` with `nextn_predict_layers = 1` and the same twenty `blk.40.*` tensors (full attention,
its own 256 routed experts, shared experts, plus `nextn.eh_proj` / `enorm` / `hnorm` /
`shared_head_norm`). The KV blocks differ only in provenance keys. **A file named `-MTP-` buys a
quantisation, not a head** — do not go looking for a special conversion.

There is no silent-fallback case to worry about either. `llama_model_n_layer_nextn()` reads the KV
key `nextn_predict_layers`, and llama.cpp loads the MTP tensors as *required* when it is greater than
zero, so a gguf that kept the key but dropped the tensors would not open at all. If a model opens
with `--mtp`, the head is genuinely there; if it has no head, the engine fails at load with a
message rather than quietly decoding one token at a time.

One consequence for measurement: **an MTP A/B must be the same file with the flag on and off.**
Comparing an `-MTP-`-named file against a differently-quantised baseline confounds the feature with
the quantisation change.

## On device it loses, and the counters say why

Measured on the test phone (12 GB), Qwen3.6-35B-A3B-Q4_K_M streamed with the shipping recipe
(`--overlap`, 3000 MiB cache, pinned dense, `--drop-cold-experts 0.75`), same file with the flag on
and off:

| | tok/s | MiB/token | majflt/token | cpu-s/token | cache hit | acceptance |
|---|---|---|---|---|---|---|
| off | 5.82 / 6.14 | 69.3 | 69–109 | 0.43–0.45 | 73.5% | — |
| `--draft 2` | 5.59 | 93.8 | 230 | 0.57 | 65.4% | 69% |
| `--draft 3` | 4.38 | 106.6 | 633 | 0.74 | 66.7% | 52% |

Speculation is working — 2.35–2.52 tokens per verify decode — and still losing, because **the prize
does not exist in this regime**. `stall_s/tok` is 0.025–0.027 in every one of those runs, MTP on or
off: 11–16% of the token. This configuration is compute-bound, and what MTP amortises is weight
*movement*. Meanwhile all three costs are real and monotonic in the draft width: the read set widens
(+35%, +54% flash bytes per token), the draft context's memory tips the device into a fault storm
(major faults per token 3–9×), and the drafting itself adds CPU (+28%, +67%).

Two mitigations came out of that measurement and are in the engine now. The draft context's graph
width was cut from 256 to 32, because compute buffers are reserved for the widest ubatch and the
output buffer scales with `ubatch × vocabulary` — 493 MiB reserved on device for a context that
evaluates one token at a time. And `--mtp-p-min` makes the draft width adaptive. Neither changes the
regime: the honest expectation is nearer break-even, not a win.

The two `off` runs did byte-identical work and still differ by 5.6% in tok/s, so treat that as the
noise floor: draft 2 is at its edge, draft 3 is well outside it. The runs were also back-to-back
without thermal gating, so the *mechanism* counters are the trustworthy part, not the exact deltas.

## Stopping early when the head is unsure (`--mtp-p-min`)

The draft loop already knows how confident the head is in each token it proposes. `--mtp-p-min F`
stops drafting as soon as that confidence falls below `F`; at the default `0` it never stops and
always drafts the full width.

On a streamed device this pays twice, which is why it is the cheapest lever available: a draft not
made is a pass through the MTP block — with its own MoE FFN, and therefore its own expert reads —
that never happens, **and** one fewer position in the verify batch, so one fewer independent routing
widening the layer's read set. A draft that is made and then rejected costs both and buys nothing,
and at draft 3 above roughly half of them were rejected.

It is a knob rather than a default because the useful value is a property of the device's balance
between drafting cost and acceptance, and 0 is what the host numbers were measured at.

Speculation requires greedy decoding: `validate()` rejects `--mtp` together with `--temp > 0`,
because acceptance under a sampling chain would depend on which draws happened to agree and the run
would no longer be the single-token run it claims to be.

## How it is wired

The whole draft/verify orchestration is llama.cpp's, reached through `common/speculative.h`, which
depends only on public headers. **No llama.cpp patch, no fork, no submodule pin of our own** — the
same rule the rest of the engine follows ([seam.md](seam.md)).

Self-speculation means one model and two contexts over it:

- the **target** context, created with `n_rs_seq = --draft` so a rejected tail is rewound from a
  bounded snapshot instead of replayed;
- the **draft** context, created with `ctx_type = LLAMA_CONTEXT_TYPE_MTP` so llama.cpp builds the
  nextn graph, and carrying the **same eval callback** as the target — the router hook is per-context,
  not per-model.

That callback is what makes the MTP block visible to the streamer. The head lives at layer index
`n_layer` — contiguous with the trunk, same `ffn_{gate,up,down}_exps` tensor naming, same expert
count — so with `--mtp` on, the hook and the expert source are sized `n_layer + n_layer_nextn`
and the head's experts are streamed, cached and dropped like any other layer's. Two things follow
from that and are easy to get wrong:

- The capture warm-up has to run on the **draft** context too. The MTP graph is only ever built by a
  decode there, so a warm-up on the target alone never harvests the head's expert tensors, and they
  would stay mmap-resident — the fault storm streaming exists to avoid.
- Prefill goes through the driver as well. The draft context's KV must reach the last prompt
  position or the first draft is conditioned on a state that never saw the prompt.

Per step the loop is: draft `--draft` tokens from the head, decode
`[confirmed token, drafts…]` asking for logits at every position, accept the longest prefix whose
argmax matches, let the draft context catch up on **that prefix**, roll the target's KV back over the
rejected tail, and read the next token for free out of the logits at the first unverified position.

### The catch-up runs on the accepted prefix, not the whole batch

That ordering is deliberate and it is not the obvious one. The catch-up re-runs the MTP block over
the batch so the draft context ends up holding rows conditioned on the target's own hidden states
rather than the head's guesses, and the next draft is seeded from the row at the accepted position.
The natural place to put it is right after the decode, on the whole verify batch — which is what
upstream's own loop does. But that computes the rejected tail too, and the rejected tail is deleted a
few statements later: those drafts were wrong, and nothing downstream ever reads their rows.

Acceptance depends only on the target's logits, which are already in hand once the decode returns, so
the tail never has to be submitted at all. Accepting first and handing the catch-up only
`[confirmed token, accepted drafts…]` leaves **identical state** — the driver seeds from row
`min(n_accepted, n_rows-1)`, which is the same row under either batch, and the surviving KV is
exactly the range the rollback used to carve out — while skipping `n_draft − n_accepted` positions
through the MTP block. It also removes the draft context's rollback entirely: it is never given a
tail to drop.

The saving is small on a host, where that block is one layer of arithmetic. It is not small on a
streamed device: **the MTP block carries its own MoE FFN**, so every skipped position is a routing
that no longer happens and a set of expert slices that no longer has to be read. At draft 3 with
acceptance around 60%, roughly one position per group stops being computed and streamed.

## Reading the numbers

```
mtp: 41/63 drafts accepted (65.1%), 2.31 tokens per verify decode (57 decodes for 132 tokens)
```

- **Acceptance** is a property of the model's trained head, not of this engine. It bounds everything
  else: at 0% the feature is pure overhead.
- **Tokens per verify decode** is what was actually bought — the factor the decode count fell by. It
  is always below `1 + --draft`.
- **The effective rate**, on the second line, is the one to believe. `tok/s` is computed from decode
  time alone and drafting happens *between* decodes, so the headline rate leaves out everything
  speculation adds. `mtp_draft_s/tok` in the CSV trailer and `mtp_draft_ms` per row measure it
  directly, instead of leaving it to be inferred by differencing against an unspeculated run.
- **The flash split**, on the third line, says how much of the run's streamed MiB the head's own
  routing pulled and how much was the widened verify batch. Speculation grows bytes per token for
  two unrelated reasons and they need opposite fixes: a narrower draft attacks the head's share,
  while the verify union only shrinks if adjacent positions agree more. The route trace cannot
  separate them — it brackets the target decode, and the head only ever runs in the draft context.

`token_demand_MiB` keeps its mechanical meaning but changes scope: it is the distinct expert bytes
one **decode** routes, and under speculation a decode is a group, so the figure is the group's union
— which is exactly the working set the cache has to stage. It is the same widening the "why it can
lose" section describes, measured. Read it against `cache_budget_MiB` as usual, but never compare it
across a speculated and an unspeculated run.

In a CSV, `mtp_batch` says how many tokens each decode confirmed. The group's **entire** cost is
charged to its first row and the rest carry zeros, so a group's per-token cost is that row's
`wall_ms / mtp_batch`. Never average rows from an `mtp=1` file together with an unspeculated one.
See [telemetry.md](telemetry.md).

## Measured

Desktop host (8 cores, 14.8 GB RAM, NVMe), Qwen3.6-35B-A3B-MXFP4_MOE (20.7 GiB, ~1.4× RAM), streamed,
greedy, 256 tokens, one prompt, single session. This host is
[DRAM-bandwidth-bound](benchmarks.md), which is the regime the amortisation targets.

With the host's measured best recipe (`--overlap --cache-mb auto --drop-cold-experts 0.75`):

| draft width | tok/s | vs off | acceptance | tokens / verify decode | flash MiB/token |
|---|---|---|---|---|---|
| off | 7.12 | — | — | 1.00 | 19.7 |
| `--draft 2` | 7.66 | +7.6% | 71.4% | 2.42 | 30.3 |
| **`--draft 3`** | **8.19** | **+15.1%** | 60.1% | 2.78 | 33.7 |
| `--draft 4` | 7.64 | +7.3% | 52.4% | 3.08 | 31.1 |

Without the lossy drop knob (`--overlap --cache-mb auto`, 128 tokens), the gain is larger — 4.28 →
5.52 tok/s, **+29%** at draft 3 — because dropping had already removed some of the compute the
amortisation is buying back.

Two things to read off the table. First, **acceptance falls as the draft widens** while tokens per
decode still rises, and the two effects cross: 3 is the optimum here, 4 is worse than 2. Second, the
widening is real and visible — flash bytes per token rise from 19.7 to 33.7 MiB because the verify
positions route independently. It still wins because compute per token falls further (0.115 →
0.088 s) than the extra I/O costs on this host. **On a flash-I/O-bound device that balance can
invert**, which is why the flag ships off and the phone A/B is a separate measurement.
