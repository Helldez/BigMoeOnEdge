# Adding a MoE architecture

Most MoE models in llama.cpp are built by the same `build_moe_ffn` helper and expose the
identical routing node (`ffn_moe_topk`) and expert tensors
(`ffn_{gate,up,down}_exps`). For those, adding support is one row.

## 1. Add a recipe

In `core/src/moe/arch_registry.cpp`:

```cpp
static const MoeRecipe k_recipes[] = {
    { "qwen3moe", "ffn_gate_exps", "ffn_up_exps", "ffn_down_exps" },
    { "your_arch", "ffn_gate_exps", "ffn_up_exps", "ffn_down_exps" },  // <-- new
};
```

`arch` is the gguf `general.architecture` string. The three suffixes are the tensor names
without the `blk.<il>.` prefix and `.weight` suffix.

## 2. Run the gates

Generate a tiny model for your architecture (adapt `scripts/make-tiny-moe.py`) and run:

```bash
cd build && ctest -R moe_gates --output-on-failure
```

If G1 (streamed == resident) passes, the architecture streams losslessly.

## When one row is not enough

Some models **pack gate and up into a single tensor** (`ffn_gate_up_exps`) instead of two.
The current recipe shape assumes three separate projections, and the engine will refuse
such a model rather than stream it incorrectly. Supporting the merged layout needs a
distinct recipe variant and a small change in `expert_stream_source` to treat the packed
tensor as two logical projections — contributions welcome.

Models with **shared/always-on experts** (a dense expert applied to every token, as in
DeepSeek and some Qwen variants) work, but the shared expert stays resident and reduces
the streaming saving proportionally. Note it in the model's entry when you add one.
