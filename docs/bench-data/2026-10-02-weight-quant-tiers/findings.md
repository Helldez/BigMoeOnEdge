# Weight-quantization tiers on a 35B-A3B MoE, measured (2026-10-02)

Question: for a 35B-A3B hybrid MoE (Cyber-Tiel-Coder-35B-A3B-MTP), how much *quality* does
buying the smaller GGUF actually cost? Speed and RAM were already understood; this session asks
only about answer quality, because RAM is worthless if the model stops being right.

Desktop host (i7-5500U, 4 threads, 16 GB), streamed from a SATA SSD. One cell per tier,
`scripts/tinymmlu-bench.py`, 100 tinyMMLU questions, λ = 0.15, ~110 min per cell:

```
/tmp/evalvenv/bin/python -u scripts/tinymmlu-bench.py \
    --parquet ~/llm/data/tinyMMLU-test.parquet --cli build/cli/bmoe-cli \
    --model ~/llm/models/Cyber-Tiel-Coder-35B-A3B-MTP-UD-<TIER>.gguf \
    --out <OUT> --lambda 0.15 --limit 100 --threads 4 --cache-mb 2000 --ctx 2048
```

Per-question records (prediction, answer, and the four choice log-probabilities) are the whole
result: `bench-data/qgate-cyber-q3-2026-10-02/tinymmlu/cell_L0.15.json` and
`bench-data/qgate-cyber-q2-2026-10-02/tinymmlu/cell_L0.15.json`, against the published Q4 baseline
`bench-data/qgate-cyber-2026-09-21/tinymmlu/cell_L0.15.json`.

## Provenance: the three tiers are the same model

Not assumed — read out of the GGUF headers (any GGUF metadata dumper will do; the keys below are
also in the first lines of each cell log the harness writes, under `llama_model_loader: - kv`).
All three report 753 tensors, 55 KV pairs, an identical geometry block (41 blocks, 256 experts,
8 used, `full_attention_interval` 4, `nextn_predict_layers` 1, ssm inner 4096 / conv 4 /
16 groups), and the same base:

| tier | `general.file_type` | size | `general.name` / base repo |
|---|---:|---:|---|
| `Q4_K_M` | 15 | 22.52 GB | Huihui Ornith 1.5 35B A3B Abliterated — `ornith-ai/Ornith-1.5-35B-A3B` |
| `Q3_K_XL` | 12 | 17.23 GB | identical |
| `Q2_K_XL` | 10 | 12.68 GB | identical |

Only the quantization type differs, so the comparison isolates weight quantization. (The
"Cyber-Tiel" name is the fine-tune's GGUF packaging; the weights are Huihui's abliterated
Ornith 1.5. Read the header before quoting a vendor's per-model numbers.)

## Accuracy — the tier ladder is inside the noise

| tier | size | tinyMMLU | slots substituted |
|---|---:|---:|---:|
| `Q4_K_M` | 22.52 GB | 67/100 (67.0 %) | λ=0.15 |
| `Q3_K_XL` | 17.23 GB | 64/100 (64.0 %) | 30.2 % |
| `Q2_K_XL` | 12.68 GB | 63/100 (63.0 %) | 28.0 % |

At n=100 and p≈0.65 the standard error is **±4.7 points**, and the whole Q4→Q2 span is 4
questions. The ladder is monotone but not resolvable: it is exactly what noise produces.

Paired on the same 100 questions (McNemar, exact two-sided) — the right test here, because the
tiers answer the *same* items and the unpaired SE overstates the uncertainty:

| comparison | first tier only right | second tier only right | discordant | exact p |
|---|---:|---:|---:|---:|
| Q4 vs Q3 | 11 | 8 | 19 | 0.648 |
| Q4 vs Q2 | 12 | 8 | 20 | 0.503 |
| Q3 vs Q2 | 10 | 9 | 19 | 1.000 |

Unanimous on 71 of 100 (49 all right, 22 all wrong); 29 items are contested. **Verdict: on this
benchmark no tier is distinguishable from Q4.** This is the second weight-quantization comparison
in the tree and the first that is paired; it is also the only one on a model this size.

## The accuracy score is a lossy summary — this is the real finding

Every one of the 100 items has a *changed* answer distribution at every tier, yet the score moves
4 questions:

| tier | mean symmetric KL vs Q4 (nats) | argmax agrees with Q4 | mean logit spread |
|---|---:|---:|---:|
| `Q4_K_M` | — | 100/100 | 4.817 |
| `Q3_K_XL` | 0.463 | 75/100 | 4.903 |
| `Q2_K_XL` | 0.582 | 70/100 | 4.217 |

So quantization damage is **diffuse and non-monotonic, not cumulative**: each tier perturbs
almost every item, the perturbations mostly cancel in the mean, and accuracy is blind to it. A
pass-rate table cannot see this. If you need a *specific* answer to be right — a code edit, a
cited fact, a value you will act on — expect roughly a quarter to a third of items to differ
from the Q4 model regardless of how good the aggregate score looks.

Note this contradicts the vendor ratio rather than confirming it. Unsloth's published table for
a sibling 35B-A3B reports KLD 0.548 (Q4_K_M) / 0.954 (Q3_K_XL) / 2.909 (Q2_K_XL) — Q2 five
times worse than Q3. Measured here, Q2 is 1.26× Q3. Their KLD is a calibration-corpus next-token
measure; this is 4-way multiple-choice. Neither number transfers to the other, so treat "Q2 is
5× worse" as a property of their probe, not of Q2.

## Methodology note: never compare raw log-probabilities across tiers

The first pass at a distributional metric said Q2 was *better* than Q4: mean gold-answer
log-probability −7.27 vs −8.70 nats, a paired improvement of +1.43 ± 0.38 (t = +3.76). That is an
artifact. Q2's logits are compressed — mean spread 4.217 vs 4.817, a paired −0.601 — so every
log-probability rises toward zero regardless of whether the model is more or less right.
Normalize over the four choices first and the effect vanishes: gold probability −0.0065 ± 0.0320
(t = −0.20), i.e. identical. Q3 shows the same shape, milder (−0.042 ± 0.031, t = −1.38).

Compare **normalized** choice distributions, or accuracy. Raw logits across quantization tiers
are not on a common scale, and the direction of the bias favours the *more* quantized model.

## Cross-model tinyMMLU reference

Provenance is mixed and marked, because only some of these are committed artifacts. Rows 1-2 and
4-8 carry per-question log-probabilities in the tree; rows 3-4 do not and were scored by a
one-off script with a different protocol, so they are context, not comparison.

| model | tier | λ | tinyMMLU | harness | source |
|---|---|---:|---:|---|---|
| Qwen3.6-35B-A3B | Q4_K_M | 0 | 88/100 | stepped, `-c 2048` | `docs/bench-data/2026-08-26-cache-aware-substitution/tinymmlu_L0.json` |
| Qwen3.6-35B-A3B | Q4_K_M | 0.15 | 84/100 | stepped, `-c 2048` | same dir, `tinymmlu_L0.15.json` |
| Qwen3.5-9B | Q4_K_M | — | 74/100 | **wide batch**, one per question | `PROGRESS.md` only; logs were `/tmp`-only and are gone |
| MiMo-V2.6-Distill-Qwen-9B | Q4_K_M | — | 68/100 | **wide batch**, one per question | as above |
| Cyber-Tiel-Coder-35B-A3B-MTP | Q4_K_M | 0 | 66/100 | stepped, `--ctx` unrecorded | `bench-data/qgate-cyber-2026-09-21/tinymmlu/cell_L0.json` |
| Cyber-Tiel-Coder-35B-A3B-MTP | Q4_K_M | 0.15 | 67/100 | stepped, `--ctx` unrecorded | `.../cell_L0.15.json` |
| Cyber-Tiel-Coder-35B-A3B-MTP | Q3_K_XL | 0.15 | 64/100 | stepped, `--ctx 2048` | `bench-data/qgate-cyber-q3-2026-10-02/` |
| Cyber-Tiel-Coder-35B-A3B-MTP | Q2_K_XL | 0.15 | 63/100 | stepped, `--ctx 2048` | `bench-data/qgate-cyber-q2-2026-10-02/` |

Absolute scores are not comparable across models — different fine-tunes, different prompt sets,
different hosts, and (rows 3-4) a different scoring protocol. Only the within-model tier
comparison above is a controlled experiment.

## What this does not establish

- **100 questions cannot resolve a 4-point difference.** The verdict is "no measurable
  degradation", which is not "no degradation". Resolving it needs ~5× the questions for a paired
  design, or a probe with more headroom than a 22-item unanimous-wrong floor.
- **tinyMMLU is multiple choice, single token, teacher-forced.** It says nothing about
  long-form generation, instruction following, or code correctness — the things a coder model is
  actually for. A HumanEval arm on the Q2 tier was scoped and not run (~3.5 h on this host).
- **One λ.** Every cell is λ=0.15. Tiers could interact differently with substitution.
- **The Q4 baseline's `--ctx` was not recorded.** Unresolvable from the artifact. It can only
  matter for the three long prompts — q017 is 997 tokens, q018 and q094 sit just over 512 — since
  at 512 the CLI refuses them and stops. Unlikely to be material, unproven.
- **Speed and RAM at the lower tiers are unmeasured here.** This session bought 9.84 GB of disk
  and headroom for an unmeasurable quality change; whether Q2 decodes faster on this host, and
  whether 12.68 GB moves it out of the streamed regime, is a separate question with its own
  numbers.