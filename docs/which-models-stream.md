# Which models stream

Any MoE model llama.cpp loads whose routed experts are laid out the way llama.cpp lays them out.
There is no list of architectures in the engine to add to: which tensors of a file are its routed
experts is read from the file.

## The rule

A tensor is a routed expert weight when all of the following hold, and the file is checked for them
before anything is streamed (`bmoe/expert_layout.h`):

- it belongs to a block: `blk.<il>.<suffix>.weight`;
- it is a `.weight`, not a companion of one (`ffn_down_exps.scale` shares the suffix and stays a
  resident, mapped tensor);
- it has three dimensions, and the third is the expert count the file declares
  (`<arch>.expert_count`).

That is how llama.cpp builds every weight it hands to the expert matmul (`mul_mat_id`), for every
architecture it converts. Everything else a MoE block holds has another shape: the shared expert and
the router are 2-D, a per-expert scale and the router's bias are 1-D. The expert count, the
per-expert stride and the byte offsets were always read from the file; the names are now too.

A file's MoE layers must all name the same set of expert tensors, and the set is at most three. The
set, sorted, is the file's *layout*, and a slot is a suffix's position in it, so the same slot holds
the same tensor in every layer. The engine streams whatever a slot holds and never needs to know
which projection it is. The layouts llama.cpp builds today:

| layout | expert tensors per layer | a model that has it |
|---|---|---|
| split | `ffn_gate_exps`, `ffn_up_exps`, `ffn_down_exps` | Qwen3 MoE, gpt-oss, DeepSeek V4, Ling 3.0, LFM2 MoE |
| fused | `ffn_gate_up_exps`, `ffn_down_exps` | Gemma 4 MoE |
| gate-less | `ffn_up_exps`, `ffn_down_exps` | Nemotron 3 / 3.5 MoE |

Each has a byte-identity gate (`tests/moe_gates.cpp`, on a tiny model `scripts/make-tiny-moe.py`
emits for that layout), run in CI on every change. A layout the gates have not seen is a reason to
add one, not a row anywhere.

## What is refused, and how it says so

The engine refuses to stream rather than guess. Each refusal names its reason, in the load error on
the CLI and in the server's response:

- **A dense model.** `expert_count` is absent or 1: "a dense model, which this engine runs as plain
  mmap and does not stream". With streaming off the model runs as llama.cpp would run it; the
  CLI's `mode:` line says so, and the UIs label the file as one this build does not stream.
- **Experts declared, none found.** The count is declared but no block holds a 3-D weight indexed
  by it. Nothing is streamed.
- **Layers that disagree.** One layer names `{gate, up, down}` and another `{up, down}`: "the
  layers disagree on what an expert is, and this engine streams one layout per file". The reason
  names the two layers.
- **More than three.** A layer with four or more expert tensors is more than the streamer's
  per-slot buffers hold.

A tensor routed over a *different* count than the file's expert count (a second, smaller expert set
in the same layer) is not claimed and stays resident; the layer's main experts stream.

## What stays resident, by layout

The rule above also says what does not stream, and it is worth knowing before measuring a model,
because it sets the streamed fraction ([limitations.md](limitations.md)):

- **Shared, always-on experts** (`ffn_*_shexp`, or a block's dense `ffn_*` beside its routed set, as
  in Gemma 4) are 2-D and stay mapped. Qwen3.5/3.6, DeepSeek V4, Ling 3.0, Nemotron and Gemma 4
  all carry one.
- **Leading dense blocks** (`<arch>.leading_dense_block_count`, as in LFM2 MoE and Ling 3.0) and the
  attention, Mamba or convolution blocks of a hybrid stack name no expert tensor and never bind.
- **A trailing multi-token-prediction block** names expert tensors like any other layer; llama.cpp
  does not load it unless asked (`--mtp`), and then it streams like the trunk ([mtp.md](mtp.md)).
- **Companions of an expert tensor** (`ffn_down_exps.scale`, the router's `exp_probs_b`) stay
  resident by their rank.
- **Anything on the dense side of the seam** - compressed attention, an n-gram embedding table, the
  hyper-connection tensors of a Qwen4 preview - is llama.cpp's and invisible to the streamer.

## The frontier that is not a layout

Everything above assumes the routing node is the one `build_moe_ffn` emits (`ffn_moe_topk`), which
the router hook reads to learn which experts a token takes ([seam.md](seam.md)). An architecture
with custom gating that bypasses that node would stream its experts and never be told which ones
were routed; the hook would have to learn its node. None of the architectures llama.cpp loads today
does that.
