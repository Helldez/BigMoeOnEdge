# Reclaim between turns — on-device capture (2026-07-15)

Measured on the OnePlus 15R (Snapdragon, 11.4 GB RAM), UFS4 storage, gpt-oss-120b-Q4_K_M pushed to
`/data/local/tmp`. Reproduced with `scripts/rewarm-ab.sh`, which runs two turns of one session with
a timed idle gap between them and samples `/proc/<pid>/status` through the gap.

The recipe is deliberately **the Android example's own session argv**, not the benchmark recipe:
`--session --chatml --no-think -c 4096 --moe-stream --cache-mb 2000 --io-threads 4 --overlap
--n-expert-used 2`. The question here is what a *chat* does between messages, so the app's
configuration is the one that matters.

Device state: thermal status 1, battery 36 °C, `policy0` uncapped at 2.19 GHz, no other engine
resident. `turns-*.jsonl` are the `BMOE_DONE` summaries (answer text stripped); `idle-*.mem` are the
`VmSwap`/`VmRSS` samples, 10 s apart, timed from turn 1's completion.

## What a four-minute idle costs

`turns-off.jsonl` — `--no-rewarm`, i.e. today's behaviour:

| | turn 1 | turn 2, after 240 s idle |
|---|---|---|
| decode | 1.454 tok/s | **0.746 tok/s** |
| major faults / token | 10.1 | **412.1** |
| CPU-seconds / token | 1.35 | **4.07** |
| flash I/O / token | 1.21 s | 0.91 s |
| compute / token | 0.36 s | 1.00 s |
| cache hit (cumulative) | 8.3% | 9.0% |
| expert bytes read | 6599.6 MiB | 7091.6 MiB |

The two turns ask for near-identical work (78 vs 76 prompt tokens, 10 vs 11 generated, the same
cache hit rate and roughly the same expert bytes), yet the second is half the speed. Flash I/O per
token *fell*; the time reappeared as "compute" and CPU per token tripled. That is fault handling, not
arithmetic — `majflt_tok` says so directly, 41× turn 1's rate.

`idle-off.mem` shows what the gap did: `VmSwap` 337 MiB → 1.46 GiB, `VmRSS` 3.80 → 1.69 GiB. The
process lost 2.1 GB of resident memory while doing nothing.

## The result that complicates the story

`turns-on.jsonl` / `idle-on.mem` — same script, same binary, rewarm left at its default. The pass
never fired (`rewarm_s: 0.000`), because this run was reclaimed *far* more gently: `VmSwap` stayed
flat at 346 MiB and RSS lost only 170 MiB, never crossing the 256 MiB threshold by enough to matter.

And yet turn 2 was **just as slow**: 0.754 tok/s, 387 major faults/token — statistically the same as
the run that lost 2.1 GB to swap.

Two runs an order of magnitude apart in swap, one identical slowdown. Anonymous swap cannot be the
whole cause. The prime suspect for the rest is the mmap'd dense weights (1768 MiB on this model):
they are file-backed, so reclaim drops them without `VmSwap` moving at all, and refaulting them reads
flash from inside the decode — which is exactly what the load-time dense warm-up exists to prevent,
undone by four minutes of sitting still.

The reclaim itself is not in doubt, and the app has it far worse than adb does: its engine goes from
3.5 GiB resident to 3.5 MiB within five seconds of a reply finishing, where an adb session took four
minutes to lose half that.

## The app test that settled it — `app-turn-after-rewarm.csv`

Sampled every 4 s through one in-app turn (gpt-oss-120b, `--cache-mb 2000`, rewarm on) that started
with 1.76 GiB swapped out. The pass fired, reported 6.4 s, and worked: by t=4 s `RssAnon` was back to
2.02 GiB — the full 2000 MiB budget — `RssFile` to 1.29 GiB, and `VmSwap` down to 463 MiB.

Then, without the turn doing anything unusual:

| t (s) | RssAnon | VmSwap | |
|---|---|---|---|
| 4 | 2.02 GB | 463 MB | restored by the pass |
| 12 | 1.86 GB | 620 MB | the kernel is already taking it back |
| 20 | 1.57 GB | 917 MB | |
| 44 | 1.46 GB | 1.02 GB | |
| 68 | 2.02 GB | 468 MB | the engine faults it back in |
| 88 | 1.88 GB | 596 MB | and loses it again |

It oscillates like this for the whole generation, `MemFree` pinned between 35 and 300 MiB. **The turn
still ran at 0.3 tok/s** — indistinguishable from no rewarm, after a 6.4 s pause.

That is the answer, and it is a negative one. This is not an idle session that can be made whole; it
is a process asking for ~3.8 GiB (2.0 expert cache + 1.77 dense) where the device concedes ~3.0, with
the kernel settling the difference continuously *while it decodes*. Restoring residency buys seconds
of truce for a several-second pause. The rewarm is now off by default.

The lever that remains is the size of the ask. On this model the 2 GiB expert cache returns an 8–9%
hit rate — 128 experts at top-2 puts the working set orders of magnitude beyond any budget that fits
— so it buys one expert in eleven at the price of the war. Measuring `--cache-mb 0` against `2000`
in-app is the obvious next step. (`auto` is not a fix here either: it sizes from `MemAvailable`, which
counts the page cache holding this model's own dense weights as free.)

## Note on thermal

Both arms started from a cool device (36 °C, uncapped clocks) and each run heats it, so turn 2 is
mildly disadvantaged by heat-soak in *both* arms — which is fine for comparing the arms to each other
but means the absolute `tok/s` carry the usual thermal noise. `majflt_tok` and `cpu_s_tok` do not
depend on temperature, and they tell the same story.
