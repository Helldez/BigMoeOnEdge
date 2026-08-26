# Cache-aware expert substitution

`--expert-substitute L` re-ranks a decode routing toward the experts already in the cache. Before
the routing is committed, every resident expert's score is raised by `L × (this token's score
range)` and the top-k is taken again. A resident expert wins a slot only when it was within that
margin of the one it displaces: a confident routing is untouched, a near-tie goes to the expert
already in RAM. The same number of experts runs; fewer of them cost a flash read. Off by default.

It is the third lossy knob in the engine, after [turbo top-k](../README.md#speed-and-quality) and
[cache-aware dropping](expert-dropping.md), and it sits one step upstream of the second: dropping
decides whether to **pay** for a missing expert, substitution decides whether to **need** one.

## Where the idea comes from

The mechanism is the cache-conditional rerouting of Skliar, van Rozendaal et al., *Mixture of
Cache-Conditional Experts for Efficient Mobile Device Inference*
([arXiv:2412.00099](https://arxiv.org/abs/2412.00099), TMLR 2025). Their observation is the one
this engine had already measured from the other side: MoE routers tolerate careful deviations in
expert selection, and the expensive part of an expert on a memory-bound device is fetching it, not
running it. They add `λ · Δ · m` to the router logits, where `m` masks the experts resident in a
DRAM cache and `Δ` is a running average of the logit range, use the modified logits only to
re-rank, and keep the weights the router computed. On four MoE models under an LRU cache they
report the miss rate cut by more than half for a 0.1 to 3 % perplexity increase.

Two earlier lines of work bracket it. [SwapMoE](https://arxiv.org/abs/2308.15030) (Kong et al.,
2023) restricts routing to a resident set of "virtual experts" outright, which is the same lever
at its hard limit. [AdapMoE](https://arxiv.org/abs/2408.10284) (Zhong et al., ICCAD 2024) adapts
the number of experts a token activates to avoid on-demand loads, which is the axis
[dropping](expert-dropping.md) works on.

What differs here:

- **The cache is RAM in front of flash**, not on-chip memory in front of DRAM, so the byte a
  substitution saves is a 4 KiB-to-1 MiB flash read on a device where
  [decode is I/O-bound](benchmarks.md), and the throughput win is correspondingly larger.
- **The range is the token's own**, not a running average. The margin is `L` times `max − min` of
  the scores the graph is about to sort, per token, so there is no state to warm up and one `L`
  means the same thing on a router whose logits span 2 and one whose logits span 20.
- **The scores are the tensor the graph sorted**, read in log space when it is a softmax (so the
  range is the logit range, and `L` matches the paper's λ). See the next section for why that
  choice is load-bearing rather than cosmetic.

## How it is wired

The hook already owns the routing node: `ffn_moe_topk-<il>` is asked for, ggml computes it in
isolation, and the callback sees the selected ids with the expert matmuls still ahead. Substitution
runs there, first, before route-ahead and before the gather, so everything downstream (the trace,
the drop policy, `load_layer`, the weight chain the graph is about to run) sees the routing that
will actually execute. It is the same seam [dropping](expert-dropping.md) writes through; see
[seam.md](seam.md).

The scores it re-ranks are read from the source of the argsort (or `top_k`) that produced the ids:
`ffn_moe_probs`, or its biased or group-masked variant where the architecture has one. That tensor
is **exact**, because whatever gating function, selection bias or group mask the model applies is
already in it, and it is **alive** at the callback by construction, because the graph gathers the
routing weights from it after the top-k, so the allocator cannot have recycled its buffer. The
gate *input* does not have that property. On Gemma 4 the router runs on a private intermediate of
the logits matmul that dies the moment the matmul is done, and re-scoring from it read another
node's bytes: two layers' "inputs" measured at the same address. The first version of this feature
did exactly that, and its gate caught it: an inert margin substituted 21 of 144 slots on the Gemma
4 fixture. Ranking on what the graph ranked is what made the gate pass on every fixture.

Residency comes from the expert source's `query_residency`, once per layer per batch, after
speculative reads have settled. The weights are not touched: the graph applies whatever the router
gives the selected experts, so a substituted expert carries the weight the router would have given
it. Substitutions are counted as **set** changes; a re-ranking that returns the router's own
experts in another order writes nothing and counts nothing.

Cost: one residency query and one `n_expert`-wide re-rank per layer per token, on the eval thread.
No barrier is added (the top-k is already one), no matmul is added, and no flash read is issued.

## Measured

Host, `Qwen3.6-35B-A3B-Q4_K_M` (40 streamed MoE layers, 256 experts, top-k 8), `--cache-mb 2000
--overlap`, 8 threads. Two texts written for this purpose and used for nothing else, scored with
`--ppl --ppl-step`: teacher-forced, one token per decode, so the cache is in the state decode
leaves it in. Evidence in
[bench-data/2026-08-26-cache-aware-substitution](bench-data/2026-08-26-cache-aware-substitution/findings.md).

| `L` | text B perplexity | next-token hit | slots substituted | text C perplexity | next-token hit | slots substituted |
|---|---:|---:|---:|---:|---:|---:|
| 0 (off) | 4.235 | 66.0 % | 0 | 4.406 | 64.1 % | 0 |
| 0.15 | 4.402 | 65.1 % | 27.9 % | 4.446 | 64.4 % | 25.8 % |
| 0.30 | 5.360 | 61.4 % | 45.7 % | 5.322 | 60.7 % | 41.9 % |
| 0.60 | 31.27 | 36.4 % | 75.1 % | | | |

Generation on the same setup, 64 tokens, one prompt:

| `L` | decode | flash per token | cache hit | slots substituted |
|---|---:|---:|---:|---:|
| 0 (off) | 2.37 tok/s | 258 MiB | 48.8 % | 0 |
| 0.15 | **3.84 tok/s** | **119 MiB** | 69.4 % | 27.1 % |

At `L = 0.15` a quarter of the routed slots go to a resident expert, the flash traffic halves, decode
is 62 % faster, and perplexity rises 1 to 4 %. That is the band the paper reports, and it is the
strongest single lever measured on this engine at that quality cost. At `0.30` the cost is 25 % and
the win in bytes is no longer worth it.

### A task, not only prose: tinyMMLU

Perplexity on prose is the kindest metric a routing perturbation can face, so the same cells were
run on [tinyMMLU](https://huggingface.co/datasets/tinyBenchmarks/tinyMMLU) (tinyBenchmarks,
[arXiv:2402.14992](https://arxiv.org/abs/2402.14992)): 100 MMLU questions chosen so that accuracy
on them estimates accuracy on the full 14k. Zero-shot, plain prompt, no chat template, no
thinking; each question is one `--ppl-step --ppl-choices` pass, so the question and its options
are decoded token by token under the policy and the answer is the letter with the highest
log-probability at the end (`scripts/tinymmlu-bench.py`).

| `L` | tinyMMLU | slots substituted |
|---|---:|---:|
| 0 (off) | 88 / 100 | 0 |
| 0.15 | 84 / 100 | 27.9 % |

94 of the 100 predictions are identical; 5 flip from right to wrong and 1 the other way. Four
points on a 100-item set is at the edge of what the sample can resolve, and it is in the same
direction as the perplexity. Read together: at `L = 0.15` the lever costs something measurable, and
it is still the largest byte saving measured on this engine at that cost. The absolute score is
below the model's published one (quantized, zero-shot, no reasoning); what the table measures is
the difference between the two cells on the same questions, deterministically.

### A generation, not one token: HumanEval

tinyMMLU scores a single token after a teacher-forced prompt. A reply is a generation, where a
perturbed routing feeds the next token's routing, so the same two cells were run on the first 50
problems of [HumanEval](https://github.com/openai/human-eval) (`scripts/humaneval-bench.py`):
greedy completion of the function body, cut at the usual stop sequences, graded by the canonical
tests, one `--session` per cell so the cache stays warm between problems as it does in a
conversation.

| `L` | pass@1 | mean decode | flash per token | cache hit |
|---|---:|---:|---:|---:|
| 0 (off) | 42 / 50 | 2.36 tok/s | 292 MiB | 45.5 % |
| 0.15 | 42 / 50 | **5.53 tok/s** | **69 MiB** | 81.5 % |

34 of the 50 completions are byte-identical; one problem flips each way. On ~190 generated tokens
per problem, a quarter of the routing steered toward resident experts leaves pass@1 where it was,
cuts the flash traffic by 4x, and more than doubles decode. The throughput gain is larger than in
the 64-token one-shot above because a warm cache is what this lever feeds on: across 50 problems
the hit rate sits at 81 % instead of 69 %.

### The two negatives, kept on purpose

- **`L = 0.60` destroys the model and the text does not show it.** Perplexity 31 against 4.2, the
  next-token hit rate down from 66 % to 36 %, three quarters of the routing replaced, and the
  generated prose is fluent and on topic. Nothing about this knob may be judged by reading the
  output. Judge it on `--ppl-step`, or on answers you can check.
- **A wide scoring batch cannot price it.** The first measurement of this feature used `--ppl` in
  its one-batch form and found `L = 0.15` *lossless* and `L = 0.30` costing 11 %. Both were wrong. A
  wide batch routes every position of a layer before reading any of it, so the cache holds what
  the previous layers left and the policy touched 1 to 3 % of the slots it examined, against 26 to
  28 % in decode. `--ppl-step` exists because of this, and the same caveat applies to any
  measurement of [dropping](expert-dropping.md) that does not step.

## Gates

`bmoe_moe_gates` covers the machinery, not the output, exactly as for dropping:

- **G8d** — a margin far below any real gap (`1e-6`) must be byte-identical to the same cached run,
  and must have examined routings while substituting none. On every fixture (split, fused, split
  multi-shard) it examines 144 to 192 slots and changes 0.
- **G8e** — the full margin (`1.0`) must generate and must demonstrably fire: 122 of 192 slots on
  the qwen3moe fixtures, 66 of 144 on gemma4. A count of zero would mean the flag is inert, which
  is the failure mode a lossy knob with plausible output can hide indefinitely.

`validate()` rejects the flag without a cache (nothing resident to prefer) and outside `[0, 1]`,
NaN included; `bmoe_config_test` pins both.

## Defaults, and what is owed

The CLI defaults it off and will keep doing so, for the reason every lossy knob does: the
byte-identity gates need a deterministic default.

The app exposes it under **Speed / quality → Experimental → Prefer cached experts**, as a
percentage of the score range with rungs 10 / 15 / 20 / 30, default off, disabled with the
cache off. From 20 % up the screen says in red that the replies will read well while the model
behind them is worse.

Every number above is from a desktop, where flash is a large share of a token. On the phone that
share is between 15 % and 56 % depending on the model and the budget, so the throughput column will
compress, and by how much is the device A/B this feature is owed before it earns a default or a
place outside Experimental. The quality evidence is two held-out texts, 100 tinyMMLU questions and
50 HumanEval problems, on one model; a second architecture (Gemma 4, gpt-oss) is the obvious next
cell.
