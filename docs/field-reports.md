# Field reports

Problems people reported from their own machines, written so that a symptom can be matched to its
cause. Each case says how to recognise it, what caused it, which version fixes it, and what to do on
a version that does not. A case stays here after it is fixed: someone on an older version should
still find it. `#N` is the issue or pull request on GitHub where the case was reported or fixed.

A new case is added in the same pull request as its fix, or on its own where the fix is not yet
known. A cause is written down only as it was confirmed, in the code, on a machine or by the
reporter; where it never was, the entry says so rather than guess. Machines are described by class
(a 32 GB card, a 12 GB phone, a desktop processor with integrated graphics), never by the
reporter's name or model codes.

The cases are in three groups: [still open](#still-open), [fixed](#fixed), newest first, and
[closed without a change to the engine](#closed-without-a-change-to-the-engine).

## Still open

### The CUDA pack extracted beside `bmoe-cli`: the card is never used

**Seen on** 0.40.2, Linux, the release archives used by hand: a 12 GB card, 128 GB of RAM, a model
larger than the card. 3 tok/s; installed correctly, the same release on the same class of machine
decodes at 16-17.5 ([bench-data/2026-11-09-rented-12gb-cuda-pack](bench-data/2026-11-09-rented-12gb-cuda-pack/README.md)).

**How to recognise it.** The run lists no CUDA device, and the CUDA libraries (`libggml-cuda.so` or
`ggml-cuda.dll`, the CUDA runtime, cuBLAS) sit in the folder that contains the `bmoe-cli-...`
folder, not inside it.

**Cause.** The main archive extracts into a folder of its own; the CUDA pack has none, so its files
land wherever it is extracted. Extracting both in the same download folder leaves the pack one level
above `bmoe-cli`, which loads modules from its own folder only, and nothing says so.

**Not fixed yet.** The pack should carry the same top-level folder, and the log should say when an
NVIDIA driver is present but no CUDA module loaded.

**Workaround.** Extract the pack inside the `bmoe-cli-...` folder ([platforms.md](platforms.md)). The
desktop app is not affected: it installs the pack itself.

### Every new conversation prefills the system prompt again on a hybrid model (#583)

- **Seen on** 0.42.0, CPU-only ARM machines (a 2-core cloud VM with 12 GB, phones with 6 and 22 GB),
  hybrid models (Qwen3.6 REAP, Qwen3.8-Flash-Next, LFM2.5-24B) under an agent with a 4-6k token
  preamble: 9 to 13 minutes of prefill for every new conversation and for the first request after a
  restart, against 11-29 s with the reporter's own change.
- **How to recognise it.** A new conversation does not reuse the preamble's tokens: the prompt's
  prefill time is the whole preamble's every time.
- **Cause.** Not confirmed yet. The reporter's reading: no context checkpoint sits at the end of the
  preamble, and the oldest of the 8 kept is the first evicted.

### An image follow-up on a 12 GB iPhone ends the app (#573)

- **Seen on** 0.42.0, iOS, a 12 GB iPhone, Qwen3.6-35B-A3B Q4_K_M with its Q8_0 projector, 16384
  context: a question about a small image, then a follow-up, and the app is killed.
- **How to recognise it.** The system's memory report names `per-process-limit`, at 6144 MiB.
- **Cause.** Not confirmed yet. A 2048-token text-only run stayed under the limit (5463 MiB peak).

### The expert cache stops at the locked-memory limit (Linux, part of #321)

- **Seen on** 0.33.x, a Linux desktop with two 16 GB cards and 60 GiB of RAM, DeepSeek-V4-Flash
  UD-IQ2_M: 4.38 tok/s.
- **How to recognise it.** The plan's `cache-lock` line says the cache is bounded by what can be
  locked, and the log reads "cache pinning refused after N MiB".
- **Cause.** The account's `RLIMIT_MEMLOCK` capped the pinned cache at 7.2 GiB.
- **Workaround.** Raise the limit for the shell that starts the engine
  (`sudo prlimit --pid $$ --memlock=unlimited:unlimited`): 8.72 tok/s, 91.1 % cache hits.

## Fixed

### A graphics card beside integrated graphics: the model runs on the CPU

**Seen on** 0.42.0, the desktop app: a 32 GB card, a desktop processor with integrated graphics,
32 GB of RAM, a 26B MoE model in Q6_K small enough for the card. It decoded at 9.5 tok/s on the CPU,
where the plan expected 44.

**How to recognise it.** On the Plan screen, "Device backends" lists the integrated GPU (here
`Vulkan1`, the processor's Radeon graphics) next to the card, the notes read "Placement declined"
with a size and "Layer placement: none", the experts stream from flash, and "Tokens" says CPU.
It only appears once the card has two backends, for instance after installing the CUDA pack beside
the Vulkan module the app ships with.

**Cause.** With two backends on the card, the plan chooses one per card and hands llama.cpp's
capacity fitter the resulting list, and the integrated GPU is a card of its own in that list. The
fitter put part of the model on it. Its memory is the host's, and a placement that touches the
host's memory is set aside whole, so the card's share went with it
([hardware-planning.md](hardware-planning.md)). llama.cpp's own default list leaves an integrated GPU
out beside a discrete one, which is why the case did not appear without the pack.

**Fixed in** the release after 0.42.0 (#584): the fitter is asked only of the devices with memory of their
own, where there is one (`capacity_devices`, [backends.md](backends.md)).

**On 0.42.0 and earlier**, disable the integrated graphics (on Windows: Device Manager, Display
adapters, the integrated GPU, Disable device), restart the app and load the model again.

### A dense model runs on the CPU, not on the card

**Seen on** 0.42.0, Windows: a 6 GB laptop card, 32 GB of RAM, a 4B dense model in Q4_K_M. 12 tok/s
in the app, 65 with llama.cpp `-ngl all` on the same machine.

**How to recognise it.** The model has no experts (the Plan screen reads "Nothing to stream" and
streaming off), it fits the card, and "Tokens" says CPU. Every dense model loaded through a UI is
affected, on any machine with a graphics card.

**Cause.** The plan stopped as soon as it saw a model with no experts, before it applied the capacity
fitter's placement. No layer was placed, and with the devices left to the plan a run with no layer
placed opens on the CPU alone ([hardware-planning.md](hardware-planning.md)).

**Fixed in** the release after 0.42.0 (#585): the fitter's placement is applied to a dense model too.

**On 0.42.0 and earlier**, set Settings, advanced, "Layers on a device" to 99 and load the model
again: a value set by hand is kept, and the plan never reaches the step that would change it (read
from the code, not run on a card). This engine adds nothing to a dense model that fits the card:
llama.cpp runs it as fast.

### The expert cache stops short on a machine whose RAM holds every expert (#462)

- **Seen on** 0.39.0, Linux, a mini-PC with 96 GB of RAM and an integrated GPU, Qwen3.8-Flash-Next
  Q2_K (75 GB): 8.8 tok/s, the cache at about 92 % of an expert set that fits in RAM whole.
- **How to recognise it.** The plan's `dense-mapped` embedding table (27 GB) is also counted in the
  host's dense size, and the cache ends below the expert total.
- **Cause.** The embedding tables were charged as RAM while they stayed mapped.
- **Fixed in** 0.40.0 (#464). The integrated GPU staying unused there is by design: it measured
  slower than the cores (about 50 against 123 GiB/s).

### Answers stop in the middle of the reasoning (#460)

- **Seen on** 0.39.0, Linux, CPU only, Qwen3.8-Flash-Next Q2_K at a 32k context: answers end with no
  error.
- **How to recognise it.** Every answer ends at 1024 tokens.
- **Cause.** Answers were capped at 1024 tokens ("Max tokens") whatever the context size.
- **Fixed in** 0.40.0 (#461): the answer budget follows the context.
- **Before.** Raise Max tokens with the context.

### An agent sees only tool calls, with no text or thinking (#448)

- **Seen on** 0.38.0, Linux, the Pi agent: only tool calls showed, never text or thinking.
- **Cause.** Since 0.35.0 thinking was off unless a request turned it on, and Pi asks for it only for
  a model it marks as reasoning.
- **Fixed in** 0.39.0 (#450).
- **Before.** 0.34.1 behaves as expected, or start the server with `--think`; a "Thinking off" saved
  in Settings has to be turned back on as well.

### An agent gets empty answers after a tool result (#334)

- **Seen on** 0.33.1, Linux, two 16 GB cards, a 27B dense model in Q4_K_M under an agent loop: 12
  empty turns in 72 tasks x 3 seeds, none with llama.cpp on the same tasks.
- **How to recognise it.** `completion_tokens` above 0 with an empty `content` and no tool call, in a
  turn that follows a tool result.
- **Cause.** The chat template returns a grammar for tool calls, and the engine ignored it; a turn the
  parser could not read was dropped.
- **Fixed in** 0.33.3 (#340): the grammar is applied, and an unreadable turn comes back as text with
  a warning.

### A desktop with two cards: idle cards, crashes, wrong figures (#321, #330)

Reported on 0.32.0 to 0.33.3, Linux, a desktop with two 16 GB cards and 60 GiB of RAM (#321), and a
desktop with an 8 GB external card beside an integrated GPU (#330). Several causes, all fixed:

- **The cards stay idle, 4.3 tok/s on a 30B model.** The bench script streamed on the CPU and never
  ran `--auto`. Fixed in 0.33.0 (#322).
- **`--auto` aborts with `GGML_ASSERT(imatrix != NULL)`** on an IQ2 model: the bandwidth probe
  encoded IQ2_XXS without an importance matrix. Fixed in 0.33.0 (#323).
- **Decode reads 1777 tok/s on a card, about 241 in fact.** The timer stopped before the card
  finished. Fixed in 0.33.0 (#323).
- **Garbage output with layers on a card and the rest streamed.** The dense weights were moved back
  to host memory. Fixed in 0.33.0 (#323).
- **A 17-19 GB allocation fails on one 16 GB card.** A prefill device was armed over placed layers
  and the per-card split was lost. Fixed in 0.33.0 (#327).
- **A segmentation fault on the first prompt** (`RouterHook::on_topk` in the backtrace, or the log
  ending at `expert streaming ON` on #330's machine): the router hook read the experts' ids straight
  out of the card's memory. Fixed in 0.33.2 (#335); #330's reporter confirmed a later release works.
  That this was #330's cause is likely, not proven. On that machine, hiding the integrated GPU
  (`GGML_VK_VISIBLE_DEVICES`) and running `--auto` without tuning flags was the workaround.

### A 35B model at 1.2 tok/s on a 12 GB iPhone, then a crash under `--auto` (#304, #312)

- **Seen on** about 0.31-0.32, iOS, a 12 GB iPhone, Qwen3.6-35B-A3B Q4_K_M: 1.19 tok/s against 5.0 on
  a 12 GB Android phone; later `--auto` aborted at load or the system killed the app.
- **How to recognise it.** The plan chose `--dense-weights mmap` and the `compute:` line showed
  thousands of major faults a token; the crash printed `captured MoE layer is missing expert tensor`,
  the kill names `proc-thrashing`.
- **Cause.** The dense weights (about 2.6 GB) stayed mapped and were read back from flash on every
  token. Then the planner took the iPhone's GPU for one with memory of its own and split a layer's
  experts between the two, and no figure said how much a process may keep resident.
- **Fixed in** 0.32.0 (#307) and 0.33.0 (#312, built on a patch from the reporter, #314, #320):
  about 2.4-2.6 tok/s since. The rest of the gap to Android is iOS's per-process limit.

### "Engine not reachable: socket failed: EPERM" on Android (#298)

- **Seen on** 0.31.0, Android 17 with a firewall or a hardened system.
- **Cause.** The app's network permission was denied, and Android ties the loopback the app talks to
  its engine over to that permission.
- **Fixed in** 0.32.0 (#305), the message only: it says network access is missing and opens the
  app's settings.
- **Workaround.** Allow the app's network access.

### A long prompt slows decode on a small machine (#155)

- **Seen on** 0.19.0, a 4-core cloud VM with 15 GB and no GPU, Qwen3-30B-A3B Q4_K_M: 5.36 tok/s on a
  short prompt, 3.42 after a 3495-token one at a 4096 context.
- **How to recognise it.** Many evictions and re-reads a token (1612 and 8.7 here), the cache full.
- **Cause.** The automatic cache size did not count the context's memory, and the cache dropped below
  one token's set of experts.
- **Fixed in** 0.29.0: the plan charges the context and compute buffers before it sizes the cache.

### A minimal command runs plain llama.cpp mmap, and says nothing (#186)

- **Seen on** 0.23.0, `bmoe-cli -m model.gguf -p ...` on a MoE model: a slow run (830 to 2020 major
  faults a token) read as this engine's speed.
- **Cause.** Without `--moe-stream`, streaming and the cache were off, and the summary that names the
  mode printed only when streaming was on.
- **Fixed in** 0.24.0: a `mode:` line always says how the run ran, and the cache defaults to `auto`
  whenever streaming is on. Today `--auto` sets both.

### Low decode on a large server, and wrong figures in the report (#192)

- **Seen on** 0.23.0, a dual-socket Linux server with 503 GiB of RAM, DeepSeek-V4-Flash UD-Q8_K_XL:
  1.55 tok/s decode, and a report that gave the model 0.0 GiB and the storage 110 GB/s.
- **Cause.** The run was bound by compute on 8 threads, not by reads. The size came from `du` on a
  symbolic link, the storage figure from trusting `dd`'s exit code.
- **Fixed in** 0.24.0 (#194), the figures. For the speed, more threads pinned to one socket
  (`numactl --cpunodebind=0 --membind=0`) were suggested, not measured.

### The Windows `bmoe-cli.exe` flashes a window and closes (#181)

- **Seen on** 0.22.0, Windows, double-clicking the executable.
- **Cause.** It is a command-line program: with no model it prints its usage and exits, and Windows
  closes the console.
- **Fixed in** 0.23.0: a double-click waits for Enter and the archive's README says how to run it.
  Without a terminal, the desktop app is the way in.

### A fresh clone: scripts not executable, the build reports the previous version (#163)

- **Seen on** 0.20.0, macOS, a fresh clone: `scripts/build-host.sh` failed with "permission denied",
  and `--version` said 0.19.0.
- **Fixed in** 0.21.0 (#164, #166).

### The Android keyboard cannot be closed (#56)

- **Seen on** the Android app before 0.8.3: the keyboard stayed open and covered the answer.
- **Fixed in** 0.8.3.

## Closed without a change to the engine

### No GPU used on an Android phone (#465)

- **Seen on** 0.39.0, a 24 GB Android phone, Qwen3.8-Flash-Next: CPU only, about 1 tok/s, sometimes 3.
- **Cause.** By design for the GPU: on the phones measured, Adreno through Vulkan does not reproduce
  the CPU's result and is refused, and OpenCL does not win ([backends.md](backends.md)). The 1 tok/s
  itself was not explained. The interface problems reported with it were fixed in 0.40.0.

### A model writes "!!!!!!!!" as its reasoning (#456)

- **Seen on** 0.39.0, a Swift-1.5 Qwen3.8-Flash-Next quantization at half of a 131k context.
- **Cause.** The model and its quantization: the reporter saw the same with llama.cpp.
- **Workaround.** The model authors' sampling (`--temp 1.0 --top-p 0.95 --top-k 20`) or a larger
  quantization.

### Tool calls end the run on one Qwen3.8-Flash-Next file (#240)

- **Seen on** about 0.29, a 16 GB laptop, CPU only, Qwen3.8-Flash-Next Q2_K under an agent.
- **Cause.** Not confirmed: it did not reproduce here, and another quantization of the model worked
  for the reporter. Context checkpoints (0.36.0) later removed the full recompute after a tool call
  that was the first reading.

### Repetitive, corrupted text on Android 0.29.0 (#265)

- **Seen on** 0.29.0, an 8 GB phone, a gemma-4-26B-A4B Q4_K_M fine-tune: the answer to "hi" repeated
  one word among odd characters; 0.28.0 was fine.
- **Cause.** Not confirmed: it did not reproduce on a PC or a phone, and the tests asked for did not
  come back.

### A crash in `--session` on a phone built from source (#202)

- **Seen on** 0.23.0 and 0.27.0 built in Termux on an 8 GB phone, a gemma-4-26B-A4B Q4_K_M fine-tune:
  a segmentation fault at the first generation, in ggml's Q6_K dot product.
- **Cause.** Not confirmed; the follow-up runs did not come back.
- **Workaround.** One-shot mode without `--progress` worked for the reporter.

### Loading a model takes minutes on a phone (#63)

- **Seen on** early versions and 0.22.0 on a phone: 2 min 48 s for gemma-4-26B.
- **Cause.** Not a fault: loading copies the dense weights into pinned memory, which is what keeps
  them from being read back from flash at every token later.

### A model prints `<pad>` without end (#54)

- **Seen on** the Android app, a gemma-4-26B-A4B QAT file.
- **Cause.** A corrupted download, confirmed by the reporter: downloaded again, the model worked.
