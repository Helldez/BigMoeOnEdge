# N-gram decoding (`--ngram`)

`--ngram` is the second draft source for the same self-speculative loop [`--mtp`](mtp.md) drives. It
drafts by **looking the recent tokens up in the text**: take the last few tokens, find where that
exact run occurred before — in the prompt or in what has been generated — and propose whatever
followed it. No head, no draft context, no decode, no expert read. Off by default.

Like `--mtp` it is not a quality trade. A draft is accepted only where it equals the target's own
argmax, so nothing is approximated and no weight is skipped. It inherits the same caveat about
bit-reproducibility, for the same reason — the verify batch, not the source — so read
[mtp.md's "It is exact, but it is not bit-reproducible"](mtp.md#it-is-exact-but-it-is-not-bit-reproducible)
as written for both, and never put either in a byte-identity gate.

**Verdict up front: it does not win, on either platform, and it ships off.** It holds its floor on
free prose and lands below baseline on a copy-heavy prompt — on the host by 4%, where `--mtp` gains
15%; on device by 29%, where `--mtp` loses 18%. Acceptance, not drafting cost, is what pays for a
widened verify batch, and on device the speculative loop's rollback snapshots cost memory that no
choice of draft source avoids. The [host](#measured-it-holds-the-floor-and-it-does-not-win) and
[device](#on-device-it-loses-too-and-one-new-cost-shows-up) measurements are the point of the page;
what follows first is the mechanism that makes them readable.

## Why this source exists

Not because drafting was expensive. Because of what the flash split measured when we went looking
for *where* MTP's cost actually was.

On the desktop host, Qwen3.6-35B-A3B-MXFP4 streamed at `--draft 3`, the counter added for exactly
this question ([telemetry.md](telemetry.md)) said:

```
mtp: of 30826.3 MiB streamed, 909.0 MiB (2.9%) was the head's own routing,
     29917.3 MiB the widened verify batch
```

**The head's own routing was 2.9% of the extra bytes. The other 97.1% was the widened verify
batch** — the `N` verify positions routing independently, which every draft source pays and no
choice of source can avoid.

So a cheaper draft producer, by itself, is worth almost nothing. What made this one worth building is
a different property, and it is the only one that matters here:

> **The n-gram source can decline to draft, at zero cost. The head cannot.**

Below its match threshold it returns nothing, the verify batch is never widened, and the step is
*exactly* a plain single-token decode — same batch, same single logits row, no rollback. The head has
already spent a pass through the MTP block by the time it knows how unsure it is; `--mtp-p-min` can
stop the *next* draft but not refund that one.

That changes the shape of the bet. MTP is a wager on every token: pay the widening, hope acceptance
covers it. The n-gram source pays only where the text is repeating — code being edited, a quotation
from the context, structured output — and elsewhere costs nothing. Its ceiling is bounded by how
often that happens, which is why `drafted_steps` is reported next to everything else.

That was the hypothesis. **The measurement below refutes the useful half of it**: the floor is real
per step, but the steps that *do* draft turned out to be net-negative on this engine, so the run can
land below baseline anyway. The section is kept as written because the reasoning is what the counters
supported at the time, and because the refutation is only legible against it.

It also removes what the device A/B blamed for the fault storm: MTP's draft context reserved memory
the expert cache would otherwise have had (measured on the host: the auto budget fell from 3219 to
2697 MiB and the hit rate from 82.5% to 71.6%). The n-gram source allocates nothing.

## How it drafts

`core/include/bmoe/ngram_draft.h` — a pure function over token ids, no llama.cpp at all (see
[seam.md](seam.md)).

The corpus is the prompt plus everything generated so far, plus the token just confirmed; the pattern
is its suffix. Every position strictly before the corpus end is a candidate match end, so the pattern
is never matched against itself. For each, the longest common suffix is measured, capped at
`ngram_max_match`. **The longest match wins; among equal lengths the most recent one does** — recency
is what a code edit or a repeated quotation wants. A match may overlap the pattern, which is how a run
of one repeated token predicts itself. Then the tokens that followed that match become the draft, up
to `--draft` of them, clipped at the corpus end.

If the best match is shorter than `--ngram-min-match`, it drafts nothing. That is the gate.

Cost is one backward scan with an early exit, `O(corpus × ngram_max_match)` on a corpus bounded by
`n_ctx` — microseconds, and measured: `mtp_draft_s/tok` reads `0.0000` on every run below. There is no
incremental index to keep in step with the KV cache, which is deliberate; the corpus vector the loop
already maintains for the head is the whole data structure.

`tests/ngram_test.cpp` pins the selection rules — tie-breaks, clipping, the self-match exclusion, the
gate boundary — deterministically, with no model.

## Flags

| flag | meaning |
|---|---|
| `--ngram` | draft by prompt lookup (mutually exclusive with `--mtp`) |
| `--draft N` | tokens drafted per verify batch (default 3, max 8) — shared with `--mtp` |
| `--ngram-min-match N` | shortest run of matching tokens allowed to draft (default 3) |

`--ngram-min-match` is the knob that trades coverage for precision. At 3, free prose rarely matches —
which is the point: the floor holds and prose runs stay on the baseline. Lower it for coverage on
text that repeats loosely, raise it for fewer and better drafts.

Unlike `--mtp` there is **no model requirement**: it drafts from the text, not from the weights, so it
runs on any gguf. Verified on Qwen3-30B-A3B, which has no `nextn` block and which `--mtp` refuses to
open at all.

## Reading the numbers

```
ngram: 42/95 drafts accepted (44.2%), 1.20 tokens per verify decode (214 decodes for 256 tokens)
ngram: drafting costs 0.0000 s/token on top of decode → 5.24 tok/s effective (vs 5.24 reported)
ngram: drafted on 32 of 214 steps (15.0%); the rest decoded plainly
```

The first two lines mean what they mean under [`--mtp`](mtp.md#reading-the-numbers), and the effective
rate on the second is still the one to believe.

The third line is this source's own, and it is what makes a result interpretable. **The steps that did
not draft ran at exactly the unspeculated cost**, so the same delta over baseline says something quite
different at 10% coverage than at 90%. It cuts both ways, and in practice it cut the wrong way here: a
4% loss at 15% coverage is not a small effect, it is a large per-drafted-step loss diluted by the 85%
of steps that abstained. Divide by the coverage before believing a delta is small. Also in the CSV as
`drafted_steps=` against `mtp_decodes=`.

There is no flash-split line: the n-gram source reads no weights, so its share is zero by construction
rather than by measurement.

## Measured: it holds the floor, and it does not win

Desktop host (8 cores, 14.8 GB RAM, NVMe), Qwen3.6-35B-A3B-MXFP4 (20.7 GiB, ~1.4× RAM) streamed with
`--overlap --cache-mb auto --drop-cold-experts 0.75`, greedy, 256 tokens, `-c 4096`. All cells
back-to-back in one session, `off` run twice as the noise floor. **Effective** tok/s includes drafting;
it is the one to read.

One confound to read these with, because `--cache-mb auto` sizes to free RAM at load and this host's
free RAM moves between runs: the auto budget landed at 4411 MiB on the first prose cell and
5659–6016 on the other three, which is most of why that prose pair spreads 5.80 to 6.59 — a 13.6%
noise band on identical work. The copy-heavy cells all sized within 6195–6883 MiB, so that group is
the one to argue from, and it is where the verdict comes from. Never compare a cell here against a
number measured on another day.

Free prose ("Explain how a solid-state drive stores data, and why it wears out."):

| cell | tok/s | effective | MiB/token | hit | acceptance | tok/verify | drafted steps |
|---|---|---|---|---|---|---|---|
| off | 5.80 | 5.80 | 46.0 | 88.2% | — | 1.00 | — |
| off (repeat) | 6.59 | 6.59 | 34.5 | 90.6% | — | 1.00 | — |
| `--mtp --draft 3` | 8.80 | **7.32** | 42.2 | 88.6% | 60.4% | 2.81 | 100% |
| `--ngram --draft 3` | 6.51 | **6.51** | 32.3 | 91.6% | 22.2% | 1.05 | **7.4%** |

Copy-heavy (a C file in the prompt, "repeat it unchanged, adding a comment above each function"):

| cell | tok/s | effective | MiB/token | hit | acceptance | tok/verify | drafted steps |
|---|---|---|---|---|---|---|---|
| off | 5.45 | 5.45 | 58.1 | 81.1% | — | 1.00 | — |
| off (repeat) | 5.65 | 5.65 | 48.0 | 83.0% | — | 1.00 | — |
| `--mtp --draft 3` | 7.40 | **6.43** | 65.5 | 75.6% | 82.4% | 3.46 | 100% |
| `--ngram --draft 3` | 5.24 | **5.24** | 67.2 | 80.5% | 44.2% | 1.20 | **15.0%** |

Qwen3-30B-A3B-Q4_K_M, which has **no `nextn` block** and which `--mtp` refuses to open, same
copy-heavy prompt: off 8.84 tok/s at 44.0 MiB/token, `--ngram --draft 3` 8.52 at 58.2, coverage 8.2%,
acceptance 41.1%. The capability is real — this source speculates where the head cannot — but it does
not pay here either.

Read three things off this.

**The zero-cost claim holds exactly.** `mtp_draft_s/tok` is `0.0000` in every n-gram cell; reported
and effective rates are the same number. The head costs 0.020–0.023 s/token, and needs a draft
context that on this host took ~500 MiB of expert cache with it.

**The floor holds per step, not per run.** On prose the run lands at 6.51 against a 5.80/6.59 noise
band — inside it, as designed, because 92.6% of steps drafted nothing and ran as plain decodes. But
on the copy prompt it lands at 5.24, *below* a 5.45/5.65 floor. The 15% of steps that did draft
widened the read set (67.2 MiB/token against 48–58 at baseline) and, at 44% acceptance, bought
1.20 tokens per decode — not enough to earn the widening back. A step that abstains costs exactly
baseline; a step that drafts badly costs more than baseline, and a modest fraction of those is enough
to sink the run.

**Acceptance is what pays for the widening, and a trained head has it.** On the same prompt where
n-gram accepted 44% and lost, the MTP head accepted 82% and gained 15%. Both paid the same kind of
cost; only one bought enough with it. That is the honest summary of this feature: cheap drafting was
never the constraint, which the 3% flash split had already said. What the head buys is not a cheaper
guess — it is a guess right often enough to justify a batch that has already been widened.

### The gate is not the fix, and the sweep says why

`--ngram-min-match` does exactly what it is supposed to — and it does not rescue the result. Same
copy-heavy prompt, same session:

| cell | tok/s | MiB/token | acceptance | drafted steps | tok/verify |
|---|---|---|---|---|---|
| `--draft 3 --ngram-min-match 3` (defaults) | 5.24 | 67.2 | 44.2% | 15.0% | 1.20 |
| `--draft 3 --ngram-min-match 5` | 5.31 | 71.1 | 71.8% | 5.7% | 1.12 |
| `--draft 3 --ngram-min-match 8` | 5.49 | 66.1 | 75.0% | 3.4% | 1.08 |
| `--draft 1 --ngram-min-match 5` | **5.56** | 59.7 | **82.6%** | 9.7% | 1.08 |
| off | 5.45 / 5.65 | 58.1 / 48.0 | — | — | 1.00 |

Precision improves exactly as designed: 44% → 75% acceptance as the gate tightens, and narrowing the
draft to 1 — which is the smallest widening a speculated step can have — reaches **82.6%**, matching
what the trained head managed on this prompt. But coverage collapses alongside it, and **every cell
climbs toward baseline from below without crossing it.** The best n-gram configuration found is the
one that speculates least, and it lands on the floor rather than above it.

Read the two together and the conclusion is sharper than "it did not win": on this workload a drafting
step is net-negative or at best neutral, so tightening the knob only reduces how much of the run is
exposed to it, and the limit of that process is `--ngram` off. A knob whose optimum is its own
disablement is not a tuning problem.

The reason is the same one, seen from a third direction. Even at 82.6% acceptance a drafted step
bought only 1.08 tokens per decode: a match rare enough to be trustworthy carries one or two tokens of
evidence, while the batch it widens pays the full independent routing of every position in it. The
head's advantage was never accuracy alone — it is accuracy *sustained across every step*, which is
what turns 1 + N positions into 2.8–3.5 confirmed tokens instead of 1.1.

`--ngram` therefore ships **off**, like `--mtp`. It is kept for three reasons and no others: it is the
only speculation available on a model with no head; the per-step floor is real and now also protects
`--mtp-p-min`; and the counters it added (`drafted_steps`, the zero-cost drafting measurement) are what
make any future speculation claim on this engine falsifiable.

### On device it loses too, and one new cost shows up

Test phone (12 GB), Qwen3.6-35B-A3B-Q4_K_M streamed with the shipping recipe (`--overlap`, 3000 MiB
cache, pinned dense, `--drop-cold-experts 0.75`), 4 threads, 256 greedy tokens. Unlike the earlier MTP
device run, these cells are **thermally gated**: each waits 120 s for the SoC heat to soak into the
battery sensor and then for the reading to come back under 36.0 °C, so every cell starts between 35.3
and 36.4 °C instead of wherever the previous one left the device.

| cell | tok/s | effective | MiB/token | hit | majflt/token | acceptance | drafted steps |
|---|---|---|---|---|---|---|---|
| prose off | 5.17 | 5.17 | 82.5 | 80.9% | 299 | — | — |
| prose off (repeat) | 4.59 | 4.59 | 82.5 | 80.9% | 480 | — | — |
| prose `--ngram --draft 3` | 4.90 | **4.90** | 105.8 | 77.1% | 330 | 22.2% | 4.8% |
| copy off | 4.43 | 4.43 | 127.0 | 67.6% | 126 | — | — |
| copy `--mtp --draft 3` | 4.15 | **3.64** | 151.7 | 58.6% | 958 | 74.3% | 100% |
| copy `--ngram --draft 3` | 3.14 | **3.14** | 145.1 | 65.3% | 1427 | 50.0% | 14.2% |

**MTP loses again, now with the thermal confound removed** — 3.64 effective against 4.43, at 74.3%
acceptance and 3.20 tokens per verify decode. The earlier verdict was not an artefact of benching
without a cooldown gate.

**The flash split reproduces on device**: of 38832 MiB streamed at draft 3, 1446 (**3.7%**) was the
head's own routing and 96.3% the widened verify batch. Host said 2.9–3.0%. The claim that drafting
cost was never the constraint now holds on both platforms.

**n-gram holds the floor on prose and loses badly on the copy prompt.** Prose lands at 4.90 inside a
4.59–5.17 band, as on the host. The copy cell is −29%, worse than MTP — and the counter that explains
it is one the host never showed: **major faults per token go 126 → 1427**, an 11× fault storm on a
source that allocates no draft context at all.

That is the rollback snapshots. `cparams.n_rs_seq = draft_max` is asked for by *any* speculation,
because rejecting a draft means rewinding the KV, and on a hybrid attention/SSM model like Qwen3.6
a recurrent-state snapshot is a real allocation that scales with the context. So the n-gram source
avoids MTP's draft *context* but not the speculative loop's own memory, and on device that memory is
the expert cache's. Prose, with a 16-token prompt, barely notices (majflt 330 against 299/480); the
copy prompt, with 224 tokens of context to snapshot, storms.

Two honest caveats on the exact deltas. The copy `--ngram` cell had to be re-run on its own after a
Wi-Fi change killed it mid-generation, so it ran last, hottest (36.3 °C) and after the longest
sustained load of the session. And the two `off` prose cells did byte-identical work — same 82.5
MiB/token, same 80.9% hit — yet differ by 12.7% in tok/s, the difference tracking their major-fault
counts (299 against 480). At this thermal state the band is wide. **The mechanism counters are the
trustworthy part here, not the third decimal of a delta**: coverage, acceptance, bytes per token and
the fault counts all say the same thing, and they say it on both platforms.

## What this does not fix

The widened verify batch. It is 97.1% of what speculation costs in bytes on this engine, and it is a
property of `N` positions routing independently — the same for every source. `--ngram` avoids paying
it *when it abstains*; it pays it in full whenever it drafts, and at a lower acceptance than a trained
head. Anything that attacks the widening itself — making adjacent positions agree on their routing,
or reordering the union — is a different piece of work and is not this one.
