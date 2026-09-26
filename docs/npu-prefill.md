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
NPU, 512 on the CPU:

| prompt | CPU prefill | NPU prefill | |
|---|---:|---:|---:|
| 121 tokens | 9.95 s (12.2 tok/s) | 9.5 s (12.7 tok/s) | parity: flash bound |
| 1418 tokens, prose | 63.8 s (22.2 tok/s) | 8.2 s (172 tok/s) | 7.8x |
| 1921 tokens | 106.5 s (18.0 tok/s) | 11.2 s (171 tok/s) | 9.5x |

Decode after the 1418-token prompt: 3.41 tok/s on the CPU-only run, 3.25 tok/s with the NPU
prefill (both decode on the CPU; the difference is the memory the NPU's slots hold).

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
  buffer type only with `GGML_HEXAGON_HOSTBUF=1`, which the app sets.
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

- A model whose expert tensors the NPU's `MUL_MAT_ID` takes: Q4_0, Q4_1, Q8_0, IQ4_NL or MXFP4. K-quants
  (Q4_K_M and friends) are not supported by the Hexagon kernels. gpt-oss is natively MXFP4.
- The Hexagon backend in the build: `scripts/build-hexagon-android.sh` builds the CLI, the backend and
  its DSP-side skel inside upstream's Snapdragon toolchain container, and
  `scripts/stage-hexagon-jnilibs.ps1` stages them into the app.
- On device: `ADSP_LIBRARY_PATH` pointing at the directory with `libggml-htp-v81.so` (fastrpc resolves
  the skel through it), and `GGML_HEXAGON_HOSTBUF=1`. `GGML_HEXAGON_OPPOLL=1` makes the host poll for
  the DSP instead of waiting on an interrupt, which halves the cost of each crossing.

## Correctness

Gates G16 and G17 run the whole path against a loopback `rpc-server` fronting the CPU: the same
kernels, so placement is the only difference, and output, perplexity and the bytes the arena reads
must all match an all-CPU run bit for bit, with and without a cache, with a slowed loader, and across
several generates in one session. Removing either wait in the arena fails them (checked).

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
