# Sizing the expert cache by its own stalls (proposal)

**Status: a design on hold.** Nothing here is implemented. It records why the memory figure every
operating system reports is the wrong input for the expert cache, what was measured on two
machines, and one procedure that would size the cache the same way on every machine. It is written
against [pressure.md](pressure.md), which explains why the earlier runtime governor was retired, and
it has to answer that document before any code is written.

## The question the planner asks, and why no OS answers it

`--auto` sizes the expert cache from what the process may hold. Every platform reports a figure for
that, and each figure means something different:

| reported figure | what it leaves out |
|---|---|
| available physical memory on Windows | what trimming other processes' working sets would free |
| `MemAvailable` on Linux and Android | what compressing idle memory, or ending background apps, would free |
| free plus file-backed plus purgeable pages on macOS | what the compressor would free |
| the process's remaining share on iOS | nothing: it is a hard limit |

The planner today corrects the figure where a correction can be read (the compressor estimate) or
measured (the headroom probe), and `--headroom idle` (proposed separately) adds what other processes
hold idle. Each of these is a different reading per platform, and the measurements below show that
the same reading is right on one machine and wrong on another.

The question that matters is not how much memory is available. It is how much the process can keep
resident while it runs, without the system taking it back. No platform publishes that. It can only
be observed.

## What was measured

Qwen3.6-35B-A3B, 64 generated tokens, the same 30-token prompt in every cell, the same generated
text in every cell. One cell per configuration unless stated.

**A 15 GB Windows laptop, Q4_K_M (22 GB, so 1.5 times RAM).**

| configuration | expert cache | hit rate | decode |
|---|---:|---:|---:|
| `--auto`, sized from the reported figure (4 GB) | 1557 MiB | 55% | 3.22 tok/s |
| plain llama.cpp mmap, no streaming | (page cache) | | 4.37 tok/s |
| `--moe-stream --cache-mb 5000`, set by hand | 5000 MiB | 76% | 5.43 tok/s |
| `--auto --headroom idle` | 6107 MiB | 78% | 4.92 tok/s |

The planner lost to plain mmap because it believed the reported figure. Counting what other
processes held idle was right on this machine: the system trimmed them and gave the room to the
engine. The first tokens paid for that trim (923 ms of compute on the first token, against about
110 ms at steady state).

**A 12 GB phone in ordinary use, Q4_0 (21 GB), two runs.** A video app in the foreground, other
apps in the background, about 5 GB already in compressed swap.

| run | configuration | expert cache | hit rate | decode | tokens over 1 s | mean of the other tokens |
|---|---|---:|---:|---:|---:|---:|
| 1 | `--auto` | 828 MiB | 40% | 2.89 tok/s | 0 | 331 ms |
| 1 | `--auto --headroom idle` | 2728 MiB | 67% | 1.99 tok/s | 5, 15.1 s in all | 288 ms |
| 2 | `--auto` | 1417 MiB | 53% | 2.83 tok/s | 0 | 353 ms |
| 2 | `--auto --headroom idle` | 3547 MiB | 73% | 2.26 tok/s | 9, 14.9 s in all | 244 ms |

On the phone the larger cache paid at steady state (244 against 353 ms a token in the second run)
and then lost it all: the system compressed about 2 GB of the engine's own memory, the process's
swapped share rising from 0.2 to 2.1 GB, and a handful of tokens took 1 to 7 seconds each while the
cache was faulted back. The system did not take other processes' idle memory first. It took ours.

So a larger cache is worth 40 to 70 percent on both machines, and the size at which the system
starts taking it back is a property of the machine at that moment that no reported figure predicts.

## The proposal: grow until the work stalls

The idea is the one behind Meta's TMO and its Senpai agent: do not compute the memory a workload
needs ahead of time; change it gradually and watch how much work is lost to waiting. Senpai reads
that loss from a Linux kernel counter. The engine can read it from its own clock, because it times
every token already, and a clock means the same thing on every machine.

The signal is in the CSVs above. The compute of one token is steady (110 to 150 ms on both
machines). When the system takes the cache back, the same compute takes 1 to 7 seconds, for several
tokens in a row. A jump of that size needs no counter to see.

One procedure, with no branch on the platform:

1. **Start at the floor.** The cache starts at one token cycle of the model, a fact of the model
   alone. That start is safe everywhere. The reported figure is not read.
2. **Take a reference.** After warm-up, the median compute of a token on this machine with this
   model.
3. **Grow while it pays.** Every few tokens the cache grows by a step, as long as two things hold:
   no token stalled, and the flash bytes the last step saved per MiB it took are above a threshold.
   On a model where the cache does not pay, growth stops at the floor by itself.
4. **Brake on a stall.** A token whose compute exceeds the reference by a large factor halves the
   cache at once. The coldest entries are released at no cost, because they are copies of bytes
   already on flash. The size reached becomes a ceiling that is tried again only later.
5. **Tell memory from CPU.** A slow token can also mean the cores are busy or throttled. Before
   shrinking, time a one-byte read from a handful of random cache pages: a resident page answers in
   nanoseconds, a page the system took back in microseconds or more. Shrink only when that sentinel
   is slow. When only the compute is slow, wait.

The dense set is never touched, and every other choice stays with the planner. The only change is
that the cache size stops being a number fixed at load and becomes a quantity measured while the
run goes.

Replayed on the second phone run, the brake would have fired at the seventh token, where the first
stall appears, and the rest of the run would have been near the 244 ms steady state instead of
losing 15 seconds. On the laptop no stall appeared, so the cache would have grown towards the 5 to
6 GB that pays there. Both are estimates from the CSVs, not runs.

## How this differs from the retired governor

[pressure.md](pressure.md) records a TCP-shaped loop (a `mincore` and major-fault sensor, a cut on
reclaim, a grow when calm, once per token) that converged on what cache-off gives for free, because
on a model several times larger than RAM no budget earns its hits and avoids the reclaim war.

- **Growth is gated on what it buys.** The governor grew whenever it was calm. This grows only while
  a step saves flash reads, which is why it stays at the floor on the models where the governor
  thrashed.
- **The sensor is a clock.** The governor read per-OS counters, one of them unavailable to an app on
  Android. Timing the engine's own work and a few of its own pages needs no permission on any
  platform.
- **The regime is different.** The verdict above was reached on gpt-oss-120b at about five times
  RAM. At one and a half times RAM the measurements here put the cache's value at 40 to 70 percent,
  not "a few points of hit rate".

## What has to be measured before any code

- **The sentinel.** That a compressed or swapped-out page is told apart from a resident one by
  timing alone, on the phone and on the laptop, with idle apps around and under pressure. A probe of
  a few dozen lines, outside the engine.
- **The cost of starting at the floor.** How many tokens the growth takes to reach the size a fixed
  budget would have, against the prize.
- **The model where the cache does not pay.** gpt-oss-120b must stay at the floor and match
  cache-off.

The acceptance test would be a memory-constrained sweep on one machine (a process holding 2, 4, 6
and 8 GB) plus a phone in ordinary use: in every cell no token over a second after warm-up, and a
decode at least as fast as the best of `--auto` and plain mmap.

## References

- Weiner et al., "TMO: Transparent Memory Offloading in Datacenters", ASPLOS 2022, and the Senpai
  agent: memory sized by the stall it causes rather than computed up front.
- mzCache, MobiCom 2026 (arXiv 2609.01338): an on-device LLM runtime that releases and restores its
  memory in response to the platform's pressure callback, on Android only.
