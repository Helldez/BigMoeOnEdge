# Prefill on the NPU (`--prefill-device`)

Prefill and decode want opposite hardware. A prefill graph is hundreds or thousands of tokens wide,
so a matrix engine runs it many times faster than the CPU cores. A decode graph is one token wide,
and on a phone's unified memory the cost of crossing to an accelerator and back eats what it saves:
measured on a Hexagon NPU, the decode token got slower (see "Why not decode" below).

`--prefill-device D` gives each phase its hardware. Wide prefill graphs run on ggml device `D` (the
Hexagon NPU is `HTP0`); decode stays on the CPU, exactly as without the flag. With `--moe-stream` it
works on a model larger than RAM: the NPU never holds the model, only two layers of it at a time.

## Measured

Qwen3.6-35B-A3B, Q4_0 gguf (20.8 GB) on a 12 GB phone with a Hexagon v81 NPU and UFS 4 storage,
streamed (`--moe-stream --overlap --dense-weights ahwb --cache-mb 1500 -t 4`), ubatch 2048 on the
NPU, 512 on the CPU. The Q4_K_M row is the same model in the quantisation the app's catalog ships,
on the 2026-09-26 llama.cpp base, with 8 loaders:

| prompt | CPU prefill | NPU prefill | |
|---|---:|---:|---:|
| 121 tokens | 9.95 s (12.2 tok/s) | 9.5 s (12.7 tok/s) | parity: flash bound |
| 1418 tokens, prose | 63.8 s (22.2 tok/s) | 8.2 s (172 tok/s) | 7.8x |
| 1921 tokens | 106.5 s (18.0 tok/s) | 11.2 s (171 tok/s) | 9.5x |
| 1418 tokens, prose, **Q4_K_M** | 80.6 s (17.6 tok/s) | 10.0 s (141 tok/s) | 8.1x |

Decode after the 1418-token prompt: 3.41 tok/s on the CPU-only run, 3.25 tok/s with the NPU
prefill (both decode on the CPU; the difference is the memory the NPU's slots hold).

Gemma 4 26B-A4B-it Q4_K_M (17 GB), same phone and settings but `--cache-mb 2000`, a 238-token
prompt, a 2048-token context:

| | prefill | decode |
|---|---:|---:|
| CPU | 16.2 s (14.7 tok/s) | 3.60 tok/s (8192-token context) |
| NPU | 5.85 s (40.7 tok/s) | 3.30 tok/s |

**Memory is the limit on a model with a large KV cache.** The device path holds the two expert slots
(1.1 GB on this model), the device's compute buffers (0.8 GB at ubatch 2048) and the model state in
the device's host buffer. On a 12 GB phone Gemma 4 at an 8192-token context and a 2000 MiB expert
cache already leaves about 330 MB free on the CPU alone; with the device path on top it thrashes. At
2048 it fits. A smaller `--cache-mb` or context makes the room.

**Short prompts do not gain.** A prefill graph this wide routes to nearly every expert of every
layer, so the NPU path reads the whole expert set from flash once per graph (17.4 GB here),
whatever the prompt length. Past roughly a thousand tokens that read hides behind the NPU's compute;
under a few hundred it is the whole cost, and the CPU path, which reads only the experts the prompt
routes to and hits its cache, is as fast.

## How it works

### Moving the weights per graph, not per model

llama.cpp's scheduler runs each op on the backend that holds its weight, and it decides that again
for every graph it builds. Where a weight lives is three public fields of its `ggml_tensor`:
`buffer`, `data`, `extra`. So before a wide prefill graph the engine rebinds the layer weights onto
the NPU's buffers, and afterwards back onto the CPU's. Nothing in llama.cpp is patched. The device
joins the scheduler at load as the model's only non-CPU device, with no layer assigned to it, and
`op_offload` is off, so the device runs nothing it was not handed.

The one hazard is graph reuse: llama.cpp skips re-scheduling a graph shaped like the previous one.
The rule that makes the rebind safe is about widths. The prompt goes in one ubatch per decode;
pieces at least `--prefill-min-tokens` wide (default 32) run on the device and a shorter tail runs
on the CPU, so a device graph and a CPU graph never share a shape. Speculative decoding widens CPU
graphs and runs a second context over the same weights, so the two are refused together for now.

### The arena: two layers of the model at a time

A model larger than RAM cannot give the NPU a copy of itself. The NPU gets two layer-sized slots
instead, and every layer's weights are bound to one of them, alternating. While the NPU computes
layer k out of one slot, loader threads fill the other with layer k+1:

- **experts** are read from the gguf with `O_DIRECT`, one expert at a time, and handed to the backend
  with `ggml_backend_tensor_set` on a per-expert view. That call is where the Hexagon backend repacks
  them into its matrix-engine tiles, so the repack runs in parallel across the loaders;
- **the other layer weights** (attention, norms, shared experts) are already resident on the host,
  so filling them is a copy, not a read.

Pacing uses points the graph already offers. The experts wait at the layer's routing node, which the
streamer knows how to isolate; the routing itself is not read, since at this width it selects nearly
every expert and it lives in NPU memory anyway. The other weights are needed before the routing, so
they wait at the last node of the previous layer, which the capture pass learns per layer because no
node name is common to every architecture. A graph that skips a pacing point fails the decode rather
than compute on a slot that never filled. Measured: the slots cost about 900 MB for the model above,
instead of the 21 GB the model is.

### What else had to move

- **The KV cache and recurrent state** move once, at load, into the NPU's host buffer type: memory the
  CPU reads directly and the NPU addresses too, so decode and prefill share one cache. Left in plain
  CPU memory, every attention of a device graph would run on the CPU. The Hexagon backend exposes that
  buffer type only with `GGML_HEXAGON_HOSTBUF=1`, which the app sets. The buffers llama.cpp first
  allocated the state in stay allocated, since only llama.cpp can free them, but nothing reads them
  after the move, so their pages are handed back to the kernel (and again after each
  `llama_memory_clear`, which rewrites them). Kept resident they would double the KV cache: 1760 MiB
  on Gemma 4 26B-A4B at an 8192-token context, where Qwen3.6, mostly linear attention, has 143 MiB.
- **Weights that are not a matmul's matrix.** A backend may hold a `WEIGHTS` buffer in a form only its
  matmul kernels address: with DMA64 on (the default above Hexagon v79), the Hexagon backend maps
  such a buffer for DMA only, and most of its other kernels refuse it at run time, which aborts the
  graph. Gemma 4 met it first: it broadcasts a per-expert scale with `REPEAT`. The capture pass records every layer weight some op reads other than as the matrix of a
  `MUL_MAT`/`MUL_MAT_ID` (through views too), and those go to a second pair of slots in plain device
  memory, in their own type. The matrices keep the `WEIGHTS` slots. No op moves: the scheduler still
  runs each on the device, now on memory every kernel can read.
- **Weight types the device refuses.** A "Q4_0" gguf is a mix: the one above keeps its shared experts
  in Q5_0 and four attention projections in Q6_K, which the Hexagon matmul does not take. Left alone
  they ran on the CPU inside every device graph (65 matmuls, 111 splits, 17.6 s instead of 11.2 s).
  Such a weight is carried to the device in the nearest type it takes (Q8_0 for a quantised grid,
  F16 or F32 for a float one), converted once at load. The file and the CPU decode are untouched.
  Which type is asked of the device with a probe matmul, not read off a list.
- **Compute buffers.** llama.cpp reserves compute memory at load for the widest graph with every
  weight on the CPU, a graph this session never runs there, and it reserves a logit row per token of
  the ubatch, each the width of the vocabulary: 2.2 GB at ubatch 2048, which pushed decode into
  thrashing (0.5 tok/s). The reservation is redone with the weights on the device, and the logit rows
  are capped at 128, which brought the CPU's buffer to 154 MB.

## Requirements

- A model whose expert tensors the NPU's `MUL_MAT_ID` takes: Q4_0, Q4_1, Q8_0, IQ4_NL, MXFP4, and since
  the 2026-09-26 llama.cpp base the K-quants Q4_K, Q5_K and Q6_K, which is what a Q4_K_M is made of.
  A dense weight in any other type is converted for the device (see above); Q3_K and below are not
  taken for experts. gpt-oss is natively MXFP4.
- The Hexagon backend in the build: `scripts/build-hexagon-android.sh` builds the CLI, the backend and
  one DSP-side skel per NPU generation (v73 to v81) inside upstream's Snapdragon toolchain container,
  and `scripts/stage-hexagon-jnilibs.ps1` stages them into the app. The release APK is built the same
  way by CI, so it carries all of them.
- `--prefill-loaders N` (default 8) sets the threads that fill the slots, apart from `--io-threads`,
  which stays the decode's read lanes. Each loader reads and repacks, and a K-quant repack is
  CPU-heavy: on a Q4_K_M, 4 loaders left the NPU waiting 10.3 s of a 15.1 s prefill, 8 left it 5.0 of
  10.0.
- On device: `ADSP_LIBRARY_PATH` pointing at the directory with the `libggml-htp-v*.so` skels (fastrpc
  resolves the one for the phone's NPU through it), and `GGML_HEXAGON_HOSTBUF=1`.
  `GGML_HEXAGON_OPPOLL=1` makes the host poll for the DSP instead of waiting on an interrupt, which
  halves the cost of each crossing.

When the device is not there the run does not fail: a name the registry does not know (no backend in
the build, or a phone without the fastrpc driver, where Hexagon registers nothing) and a device that
does not open (a Snapdragon older than v73, which registers and then refuses a session) both leave the
whole run on the CPU, with a `bmoe:` line on stderr saying which, and `prefill_dev_tokens` stays 0.
The device is opened once before the load to find out, and the context reuses that session.

Without `--prefill-device` a Hexagon build keeps the NPU out of the run altogether. llama.cpp, given
no devices, lists every GPU-type device and opens a backend on each, and Hexagon reports itself as
one; the engine drops any such device that can reach neither a host buffer type nor host pointers,
since with no layer assigned it could do nothing but open a DSP session. See `docs/seam.md`.

## Correctness

Gates G16 and G17 run the whole path against a loopback `rpc-server` fronting the CPU: the same
kernels, so placement is the only difference, and output, perplexity and the bytes the arena reads
must all match an all-CPU run bit for bit, with and without a cache, with a slowed loader, and across
several generates in one session. Removing either wait in the arena fails them (checked). The RPC
backend is only that test fixture: it is built with the tests alone, on 127.0.0.1, and neither the
CLI nor the app accepts an RPC endpoint.

On the NPU itself the matrix engine computes in fp16, so the output is not bit-identical to the CPU's.
Price it with `--ppl` on the same Q4_0 model with and without the flag before relying on it.

## Telemetry

`BMOE_DONE` and the CSV trailer carry `prefill_dev_tokens`, `prefill_dev_nodes` (nodes the device
actually computed), `prefill_dev_read_mib` and `prefill_dev_stall_s`. See [telemetry.md](telemetry.md).

## Why not decode

Measured before this feature: the NPU runs the isolated q4_0 matmul 2.4x faster than the CPU at
batch 1 and 20x at 512, and still made the decode token slower, 11% over adb and 31% in the app. With
experts streamed on the CPU, a token crosses to the device and back about twice per layer, 91 times
on a 40-layer model, at 0.3 to 0.5 ms each. Putting whole layers on the NPU removes the crossings,
but on unified memory those layers take RAM from the expert cache one for one, and it lands at
parity. A prefill graph crosses the same boundaries once per thousands of tokens instead.
