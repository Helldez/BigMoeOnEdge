# Does a routing-frequency profile beat llama.cpp's ClassLRU in the device expert cache? (2026-11-18, #648)

No. On held-out prompts, a profile never gives the device cache more hits than the ClassLRU it
already runs, on either model and at any size from 5% to 50% of the expert set. A static profile,
the way Strata fills its GPU cache, loses 2-37 points; starting ClassLRU from the profile changes
nothing (-0.8 to +0.5 points). The kill criterion set in #648 (+3 points at the sizes an 8-12 GB card
gets) is not met, so nothing goes into the engine.

## Method

- `scripts/device-cache-replay.py` replays the decode of a `--route-trace` through:
  - `classlru`: llama.cpp's `moe_cache` policy (`src/llama-moe-cache.cpp`): fixed slots over every
    layer's experts, one LRU list per frequency class over a use count decayed with a 128-token
    half-life, the victim the least recently used slot of each list with the largest age/2^class.
    A batch wider than 32 tokens never reaches the cache, so a prompt's prefill leaves it as it was.
  - `lru`: plain recency, the policy before the llama.cpp bump.
  - `static`: the profile's hottest experts in every slot, never replaced.
  - `warm`: ClassLRU with its slots filled from the profile and its use counts started from it.
  - `pin@0.5`, `pin@0.8`: that share of the slots held by the profile's hottest, ClassLRU on the rest.
  - `belady`: the offline optimum, the ceiling.
- Traces: the 2026-11-08 profile-cache corpus, 36 prompts in 6 domains (code, math, Italian chat,
  English knowledge, creative writing, document QA), each prompt independent, 192 tokens decoded,
  recorded with `bmoe-cli --session --route-trace` on a 16 GB Apple-silicon laptop. Models:
  Qwen3.6-35B-A3B Q4_K_M (40 layers, 256 experts, top-8) and OLMoE-1B-7B (16 layers, 64 experts,
  top-8).
- The profile is the decode routing of 3 prompts per domain (18 turns); every policy is scored on the
  other 18. A timeline is one user's run of held-out prompts with the cache kept across them: one per
  domain, one per domain mixed with 3 others, and all of them shuffled (13 timelines).

## Results

Decode hits over every held-out timeline, as a share of lookups.

| Model | Cache (share of experts) | belady | **classlru** | lru | static | warm | pin@0.5 | pin@0.8 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B | 5% | 63.7 | **48.1** | 44.0 | 15.1 | 48.1 | 37.3 | 25.2 |
| | 10% | 76.4 | **62.9** | 60.8 | 25.7 | 63.0 | 55.6 | 44.2 |
| | 20% | 87.6 | **77.2** | 76.3 | 42.6 | 77.3 | 72.6 | 66.1 |
| | 35% | 93.8 | **87.8** | 88.1 | 61.9 | 88.1 | 85.4 | 82.3 |
| | 50% | 96.1 | **92.9** | 93.4 | 75.6 | 93.4 | 92.2 | 90.8 |
| OLMoE-1B-7B | 5% | 31.7 | **14.6** | 0.0 | 12.2 | 13.8 | 12.5 | 10.5 |
| | 10% | 49.2 | **28.7** | 0.0 | 20.4 | 28.4 | 23.7 | 20.9 |
| | 20% | 68.3 | **50.9** | 45.4 | 34.1 | 50.8 | 42.6 | 36.6 |
| | 35% | 83.4 | **69.2** | 65.8 | 51.1 | 69.1 | 64.0 | 57.7 |
| | 50% | 91.5 | **81.8** | 79.9 | 65.9 | 81.8 | 78.1 | 74.3 |

## Reading

- The hot experts move with the prompt: a profile built on some domains misses on the others, and a
  static cache cannot follow. That matches the RAM cache result of 2026-11-08, where every profile
  read more than LRU.
- ClassLRU is the better online policy where the cache is small (plain LRU is 0.3-0.5 points ahead
  on Qwen3.6 at 35-50%, inside the noise): on OLMoE at 5-10% plain LRU drops
  to 0 (the layer cycle evicts what the next layer needs), ClassLRU keeps 15-29%.
- The room left is between ClassLRU and belady, 4-17 points on Qwen3.6 and 10-20 on OLMoE. A profile
  does not reach into it; a predictor of the next token's routing would have to.
- The October gap to Strata on an 8 GB card (81% device-cache hits with 1.4 GB against our 61% with
  2.2 GB) was measured on plain LRU. What the device cache lacked since then was size, which #647
  addresses.

Not covered: Qwen3.8-Flash-Next (no trace recorded), long multi-turn chats, and a profile per user
kept across sessions.
