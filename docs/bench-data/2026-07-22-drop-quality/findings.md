# Cache-aware expert dropping — quality check on GSM8K (2026-07-22)

**Verdict: no detectable quality loss at any threshold, up to and including 100% of the uniform
share, where 28% of routings are discarded. This closes the open item left by the throughput
measurement — but at 15 questions it can only rule out a large regression, not a small one.**

## Setup

`bmoe-cli --session` over adb, `Qwen3.6-35B-A3B-Q4_K_M` (`qwen35moe`, 40 layers, 256 experts,
top-k 8) — the same model as
[the throughput A/B](../2026-07-22-drop-cold-experts/findings.md). Config mirrors it too:
`--moe-stream --cache-mb 3000 --io-threads 4 --overlap --dense-weights ahwb --no-think`,
`n_predict 400`, greedy, KV cleared between questions. Four cells, one variable changed, run
sequentially with a cooldown between them (`run.sh`).

15 questions from the **GSM8K test split** ([openai/grade-school-math](https://github.com/openai/grade-school-math),
`grade_school_math/data/test.jsonl`), verbatim. The prompt asks for at most three short lines of
plain-text working and a final `#### <number>` line.

**Grading rule, fixed before any output was seen:** take the number after the last `####`; if the
model never emitted the marker, fall back to the last number in the reply. The fallback is a
formatting allowance, applied identically to every cell. `grade.py` reproduces the table from
`answers.csv`, which carries every reply in full so the grading is auditable.

## Result

| `drop_cold_frac` | correct | accuracy | routings dropped | mean reply |
|---|---|---|---|---|
| off | 12/15 | 80.0% | 0 | 148 chars |
| 0.50 | 13/15 | 86.7% | ~3% | 152 chars |
| 0.75 | 13/15 | 86.7% | ~14% | 154 chars |
| 1.00 | 13/15 | 86.7% | ~28% | 154 chars |

**Decoding is greedy**, so there is no sampling noise in this table: the baseline is a
deterministic function of the model, and every difference between cells is *caused* by the drop
policy. That makes the twelve identical answers below a real statement — the policy left those
trajectories untouched all the way to the argmax — and it also means the one-question gap is a real
effect of dropping rather than a coin flip.

What it does **not** make it is an improvement. Perturbing a problem the model was already getting
wrong can land either side of the right answer; one flip in the lucky direction is not evidence the
policy reasons better, and at 6.7 points per question this sample cannot even establish the sign of
the effect. The finding is the absence of a collapse: discarding 28% of what the router selected
leaves the score flat.

Reply length is also flat (148-154 characters). Dropping did not make the model ramble or truncate;
no cell had a missing `####` marker or an empty reply.

## Where the answers actually differ

Twelve of the fifteen questions produce the **same final answer in all four cells**. All variation
is confined to two questions, and it does not worsen with the threshold:

| qid | expected | off | 0.50 | 0.75 | 1.00 |
|---|---|---|---|---|---|
| 2 (house flip) | 70000 | 195000 | **70000** | 65000 | 195000 |
| 12 (lemon tree) | 13 | 12 | 12 | 12 | 12 |
| 13 (vacuum cleaners) | 18 | 36 | 36 | **18** | **18** |

Question 12 is wrong in every cell, including the baseline: a model limitation, not a policy effect.
Questions 2 and 13 flip in **both** directions as the threshold rises. That is the signature of a
perturbation on problems already at the edge of the model's competence, not of accumulating damage —
which is what a systematic quality cost would look like.

## What this does not establish

- **15 questions resolve ~7 points per answer.** A regression smaller than roughly 13 points cannot
  be excluded. This is a sanity check, not an evaluation.
- **One task type.** GSM8K is arithmetic word problems with a checkable numeric answer, which is why
  it was chosen. Nothing here speaks to long-form writing, code, or instruction following.
- **One model, top-k 8 of 256.** A model routing 2 or 4 experts has a far larger uniform share, so
  the same threshold bites much harder. gpt-oss remains unmeasured.
- **Single run per cell.** Greedy removes sampling noise, but the policy is cache-state dependent,
  so a repeat run need not reproduce these replies exactly — a different cache history can drop a
  different set. The stability of 12/15 identical answers is itself evidence that the perturbation
  is small, but it is one sample.

## Reproducing

```
adb push prompts.jsonl run.sh /data/local/tmp/bmoe-q/
adb shell 'cd /data/local/tmp/bmoe-q && sh run.sh'      # writes out-<cell>.txt
python grade.py out-off.txt out-0.5.txt out-0.75.txt out-1.0.txt
```
