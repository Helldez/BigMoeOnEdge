# Android memory reclaim, from the engine's point of view

Why a >RAM streaming engine loses its working set on an Android phone, which levers exist (almost
none), and which numbers to check before believing any theory about it. Measured on a OnePlus 15R
(Android 16, kernel 6.12, 11.4 GB RAM, 12 GB swap, **not rooted**) while running gpt-oss-120b; the
mechanisms are generic, the constants are this device's.

Everything below is either read from `/proc` on the device or read from vendor GPL source. Where a
value could not be read live, it says so — several of the load-bearing ones are root-only.

## The shape of the problem

Reclaim is **not an event to recover from, it is a standing condition**. It happens *while the engine
decodes*, not only while it idles. Through one in-app turn, sampled every 4 s:

```
t=4    RssAnon 2.02 GB   swap 463 MB     <- full cache resident
t=12   RssAnon 1.86 GB   swap 620 MB     <- the kernel is taking it
t=20   RssAnon 1.57 GB   swap 917 MB
t=44   RssAnon 1.46 GB   swap 1.02 GB
t=68   RssAnon 2.02 GB   swap 468 MB     <- the engine faults it back
t=88   RssAnon 1.88 GB   swap 596 MB     <- and loses it again
```

That oscillation, not any single reclaim, is what costs the turn.

## Free memory, and why 95 MB is not a crisis

Watermarks act on **MemFree** — physically empty page frames — not on `MemAvailable`. Linux keeps
MemFree near zero deliberately: unused RAM is wasted RAM, so everything spare becomes page cache. A
healthy phone therefore lives *at* its watermarks, and kswapd waking is normal, not pathological.

Zone Normal on this device (`/proc/zoneinfo`), against 11.4 GB of RAM:

| watermark | pages | bytes | effect |
|---|---|---|---|
| min | 5,792 | **22.6 MiB** | below: **direct reclaim** — the allocating thread reclaims, synchronously |
| low | 24,262 | **94.8 MiB** | below: kswapd wakes |
| high | 42,732 | **166.9 MiB** | kswapd reclaims up to here, then stops |

Both gaps are 18,470 pages; since `gap = max(min/4, managed × wsf / 10000)` with managed = 2,841,569,
the implied **`watermark_scale_factor` ≈ 65 against a stock default of 10** — kswapd here is 6.5×
more hysteretic than stock, chasing 167 MiB free.

The signal that something is actually wrong is **not** "kswapd is running". It is `allocstall_*` and
`pgsteal_direct` in `/proc/vmstat`: direct reclaim is billed to *your* thread as uninterruptible
sleep, inside `llama_decode`, where no tok/s counter can see it. System-wide here: `pgsteal_kswapd`
270,019,486 vs `pgsteal_direct` 80,282,940 — **23% of all steals are direct**.

## Two kinds of memory, and why ours is the expensive kind

| | reclaim cost | restore cost |
|---|---|---|
| **clean file-backed** (mmap'd weights, page cache) | **free** — dropped, no I/O | re-read from the file |
| **anonymous** (our expert cache buffers) | compress + write to zram, maybe writeback to NAND | read + decompress |

The expert cache is *a copy of bytes that already exist in the gguf*, held in the one form the kernel
must write out to reclaim. `pswpout` on this device: **36.8 M pages ≈ 140 GB written since boot**.
Worse, Q4_K_M weights are high-entropy, so zram stores them near-uncompressed: the swapout burns CPU
both ways and frees almost nothing. *(Inference — `/sys/block/zram0/mm_stat` is root-only; check
`huge_pages` vs `pages_stored` with root to settle it.)*

This is the unnoticed cost of the O_DIRECT design. O_DIRECT buys large sequential reads instead of a
4 KiB demand-fault storm — real and worth having — but it also converts free evictions into paid
ones. Any future design should weigh both halves.

## The hit rate is what decides whether the kernel keeps your cache

The LRU promotes a page from *inactive* to *active* only on a **second reference** — and a cache hit
*is* a second reference. So the cache hit rate is not merely "flash reads avoided": it is literally
the signal the kernel uses to decide whether your memory deserves to stay.

| model | file | cache | hit | tok/s | what the kernel concludes |
|---|---|---|---|---|---|
| Qwen3-30B-A3B | 17.3 GB | 4000 MiB | **77.1%** | 4.57 | touched twice → active → protected |
| Gemma-4-26B-A4B | 15.9 GB | 4000 MiB | **82.9%** | 4.76 | same |
| gpt-oss-120b | 58.5 GB | 3000 MiB | **13.4%** | 1.15 | touched once → inactive → swapped |

And the hit rate is geometry, not tuning:

```
gpt-oss-120b: 56.8 GB experts / (36 layers × 128) = 12.3 MB per expert slot
              3000 MiB cache covers  5.2% of the bank  ->  13% hit
Qwen3-30B:    16.3 GB experts / (48 layers × 128) =  2.6 MB per expert slot
              4000 MiB cache covers 24.5% of the bank  ->  77% hit
```

gpt-oss's bank is 3.5× larger with 4.6× fatter slices, so the same cache covers 5× less of it. Cover
5% of a 128-expert bank with near-uniform routing and you hit ~13%. **On that model the kernel is
right: it is being asked to hold 2 GB of pages the engine itself touches once.** A cache below some
coverage ratio does not merely fail to pay — it actively harms, by evicting the dense weights it
competes with and by driving the allocation churn that wakes kswapd.

## The vendor tilts the field against anonymous memory

`/dev/memcg/.../memory.swappiness` reads **100**, and that number is a decoy. The module
`oplus_bsp_zram_opt` is **verified loaded** (`/proc/modules`) and hooks `android_vh_set_swappiness`,
overriding the sysctl *inside* the reclaim path ([zram_opt.c](
https://github.com/OnePlusOSS/android_kernel_msm-5.4_oneplus_sm6375), GPL, © Oplus):

```c
static int g_direct_swappiness = 60;
static int g_swappiness = 160;

static void zo_set_swappiness(void *data, int *swappiness) {
    if (current_is_kswapd()) *swappiness = g_swappiness;      /* 160 */
    else                     *swappiness = g_direct_swappiness; /* 60 */
}
static void zo_set_inactive_ratio(void *data, unsigned long *inactive_ratio, bool file) {
    if (file) *inactive_ratio = min(2UL, *inactive_ratio);
    else      *inactive_ratio = 1;
}
```

Swappiness is a 0–200 ratio of the assumed relative I/O cost of swap vs filesystem paging (see
[vm.rst](https://docs.kernel.org/admin-guide/sysctl/vm.html)); 100 is parity. So **kswapd scans anon
with 4× the pressure of file cache**, which is why 2 GB of our cache goes to zram while 4.9 GB of
page cache stays. `inactive_ratio = 1` for anon additionally forces it ~50% deactivated — parked, by
construction, on the list reclaim eats first. Corroborated live: Active(anon) 459 MB vs
Inactive(anon) 3,123 MB (87% inactive).

> ⚠️ **Load-bearing and unverified.** The `160` comes from OPLUS's 5.4 source; the module is verified
> loaded on this 6.12 device, but `/sys/module/oplus_bsp_zram_opt/parameters/vm_swappiness` is
> root-only and **was not read**. High-confidence inference, not a measurement. With root, one
> command settles it.

Note the asymmetry: kswapd (160) prefers stealing our anon, while our own direct-reclaim stalls (60)
steal file cache — we evict our own dense weights when we allocate too fast.

## Nothing protects anonymous memory. Every lever, and why it is closed

| lever | verdict here |
|---|---|
| `mlock` / `mlockall` / `MAP_LOCKED` | **dead** — `RLIMIT_MEMLOCK` = **65536 bytes, soft and hard** (`/proc/<pid>/limits`; AOSP `init.rc` sets it). 64 KB of a 2 GB cache = 0.003% |
| `setrlimit` to raise it | **dead** — raising the *hard* limit needs `CAP_SYS_RESOURCE`; an app has neither that nor `CAP_IPC_LOCK` |
| cgroup `memory.min` / `memory.low` | **absent** — memcg here is **v1** at `/dev/memcg` (0700 root:system, `limit_in_bytes` unlimited); those knobs are v2-only, and cgroup2 has no memory controller |
| `memory.reclaim` | v2-only, and unused by AOSP anyway |
| `/proc/<pid>/reclaim` | **does not exist** — a Qualcomm patch, never mainline; AOSP moved to `process_madvise()` in Android 12 |
| MGLRU knobs | `CONFIG_LRU_GEN=y` and default-enabled, but `/sys/kernel/mm/lru_gen/enabled` = `0x0000` — **vendor-disabled at runtime**. Classic active/inactive LRU applies |
| `MADV_HUGEPAGE` | no-op — THP is `[never]` |
| `oom_score_adj`, foreground service | affect **kill** selection only. Kernel reclaim is LRU-based and process-agnostic; it never consults them |
| `onTrimMemory` | notification only, and largely deprecated — apps are not notified of most levels since API 34 |
| Memory Advice API | **deprecated**; only warns, never keeps anything resident |
| `MAP_POPULATE` | prefaults, does **not** pin. Load speed only |
| **`MADV_COLD` / `MADV_PAGEOUT`** (5.4+) | **the one real lever** — unprivileged. Can't stop reclaim, but *chooses its victims*: volunteer your own cold tail so kswapd finds cheap targets and you cut direct-reclaim stalls |

**lmkd never reclaims — it only kills — and here it is silent by design.** It early-returns while
`SwapFree >= swap_free_low_percentage` (10%) of total; we sit at 70–79%. **The 12 GB swap is exactly
why nothing protects us**: from lmkd's view, a machine with free swap is a healthy machine. The
vendor's Osense/"nirvana" chases `purposeFreeMB=390` but kills `type=cached` apps only and cannot
touch a top-app process; its `kill cnt=0` log lines are the sound of the system running out of *other*
victims before kswapd comes for us.

For contrast: iOS exposes `os_proc_available_memory()` (a real per-process budget) and an entitlement
to raise it. **Android has no equivalent of either.** No on-device runtime surveyed (llama.cpp, MLC,
MediaPipe/LiteRT, ONNX Runtime, ExecuTorch) pins weights on Android or publishes a "stay under X% of
RAM" rule; llama.cpp's `--mlock` fails here for the reason above, and ExecuTorch explicitly maps
Android to `NoMlock`.

## Why restoring reclaimed pages cannot win

`MADV_WILLNEED` on anon does swap them back in — but `read_swap_cache_async` puts them on the
**inactive** list (`folio_add_lru`, `mm/swap_state.c`); nothing marks them accessed. Only a real
second touch promotes to active. So a bulk restore hands the kernel a pile of pages at the *front of
the eviction queue*, and at swappiness 160 with `inactive_ratio = 1` the next pass comes almost
immediately.

Measured: restoring 1.76 GB took 6.4 s and the kernel had it back in 8 s, with the turn still at
0.3 tok/s. **That 8 s is a property of the LRU, not of the code.**

The corollary is the useful part: a restore is only worth anything if the pages get *hit* right
after — which makes its value a function of the hit rate, and predicts it helps exactly the
high-hit-rate models it was not designed for.

## Checklist before believing any theory here

- Read `MemFree` **and** `MemAvailable` **and** `Cached` — free ≈ 100 MB is normal, not a symptom.
- Split RSS: `/proc/<pid>/status` → `RssAnon` vs `RssFile` vs `VmSwap`. `VmSwap` sees only the
  anonymous half; **file-backed reclaim is invisible to it**, and it was 27× larger here
  (`workingset_refault_file` 114.8 M vs `workingset_refault_anon` 4.2 M).
- `VmHWM` tells you whether a process ever held what you think it held — it catches "this is a fresh
  process", which looks identical to "this process was stripped" in a bare RSS sample.
- Rates, not counters: `/proc/vmstat` totals are since boot. Delta them over a window.
- **Measure in the app.** The app's engine drops 3.5 GiB → 3.5 MiB within ~5 s of a reply; an adb
  session took 4 minutes to lose half that, and a second adb run barely lost anything. Every bench
  script in this repo is single-shot and never idles, so none of them can see this class of bug.
  Seeing it takes two turns with a timed idle gap between them, sampling `/proc` through the gap.
- Beware `pgrep -f <name>`: the shell running the command matches its own command line. A 3.8 MB
  "engine" is your own `sh`.
