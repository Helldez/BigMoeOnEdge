# Cache-aware substitution, measured in the decode regime (2026-08-26)

Desktop host (8 threads), `Qwen3.6-35B-A3B-Q4_K_M` streamed from NVMe, engine at the
`feat/cache-aware-substitution` branch head. Common flags for every cell:

```
-t 8 -c 512 --ubatch 512 --moe-stream --cache-mb 2000 --io-threads 4 --overlap
```

`text-b-heldout.txt` and `text-c-heldout.txt` are short original passages written for this
benchmark (not excerpts of any published text) and used for nothing else. `run_step.ps1` is the
runner; `results.txt` holds the summary lines of every cell verbatim.

## Perplexity, teacher-forced, one token per decode (`--ppl FILE --ppl-step`)

| `--expert-substitute` | text B ppl | hits | substituted / reranked | text C ppl | hits | substituted / reranked |
|---|---:|---:|---:|---:|---:|---:|
| off | 4.2352 | 219/332 | 0 | 4.4063 | 209/326 | 0 |
| 0.15 | 4.4024 | 216/332 | 29669/106240 (27.9 %) | 4.4463 | 210/326 | 26930/104320 (25.8 %) |
| 0.30 | 5.3600 | 204/332 | 48502/106240 (45.7 %) | 5.3221 | 198/326 | 43734/104320 (41.9 %) |
| 0.60 | 31.2730 | 121/332 | 79810/106240 (75.1 %) | | | |

## The same cells in the one-batch form (`--ppl FILE`, no `--ppl-step`) — invalid, kept as the record

| `--expert-substitute` | text B ppl | substituted / reranked | text C ppl | substituted / reranked |
|---|---:|---:|---:|---:|
| off | 4.2734 | 0 | 4.4169 | 0 |
| 0.15 | 4.2495 | 932/109120 (0.9 %) | 4.3490 | 798/107200 (0.7 %) |
| 0.30 | 4.2391 | 2899/109120 (2.7 %) | 4.4581 | 2819/107200 (2.6 %) |

A wide batch routes every position of a layer before reading any of it, so almost nothing is
resident when the policy looks, and the numbers say nothing about the policy. This is the
measurement the feature's first write-up was based on; the stepped table above supersedes it.

## Generation, 64 tokens, one prompt

| `--expert-substitute` | tok/s | s/token | flash per token | cache hit | substituted / reranked |
|---|---:|---:|---:|---:|---:|
| off | 2.366 | 0.423 | 257.8 MiB | 48.8 % | 0 |
| 0.15 | 3.840 | 0.260 | 118.6 MiB | 69.4 % | 5542/20480 (27.1 %) |

Both cells produced the same opening (a reasoning trace analysing the prompt); the text diverges
after a few lines, as any routing change under greedy decoding does.

## tinyMMLU (100 questions, zero-shot, `--ppl-step --ppl-choices " A, B, C, D"`, `-c 2048`)

`scripts/tinymmlu-bench.py` on `tinyBenchmarks/tinyMMLU` (`data/test-00000-of-00001.parquet`).
Per-question predictions and choice log-probabilities in `tinymmlu_L0.json` and
`tinymmlu_L0.15.json`.

| `--expert-substitute` | correct | substituted / reranked |
|---|---:|---:|
| off | 88 / 100 | 0 |
| 0.15 | 84 / 100 | 27.9 % |

94 identical predictions; lost 5 (high_school_statistics, professional_law,
professional_accounting, elementary_mathematics, human_aging), won 1 (professional_law).
