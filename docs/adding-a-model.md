# Adding a MoE architecture

Most MoE models in llama.cpp are built by the same `build_moe_ffn` helper and expose the
identical routing node (`ffn_moe_topk`) and expert tensors
(`ffn_{gate,up,down}_exps`). For those, adding support is one row.

## 1. Add a recipe

In `core/src/moe/arch_registry.cpp`:

```cpp
static const MoeRecipe k_recipes[] = {
    { "qwen3moe", { "ffn_gate_exps", "ffn_up_exps", "ffn_down_exps" } },
    { "your_arch", { "ffn_gate_exps", "ffn_up_exps", "ffn_down_exps" } },  // <-- new
};
```

`arch` is the gguf `general.architecture` string. `exps_suffix` lists the layer's expert
weight tensors — the names without the `blk.<il>.` prefix and `.weight` suffix. The common
split layout names three ({gate, up, down}); leave a trailing slot `nullptr` for layouts
with fewer (see the fused case below). The engine discovers the expert count and per-expert
stride at runtime, so a recipe is only these names.

## 2. Run the gates

Generate a tiny model for your architecture and run its gate:

```bash
# scripts/make-tiny-moe.py --arch <arch> emits a synthetic model for that layout;
# tests/CMakeLists.txt wires one generate-fixture + gate per arch it knows.
cd build && ctest -R moe_gates --output-on-failure
```

If G1 (streamed == resident) passes, the architecture streams losslessly. For a layout
`make-tiny-moe.py` does not yet emit, either teach it that arch (preferred — permanent CI
coverage) or validate the recipe against a real model of that architecture.

## When one row is not enough

Some models **pack gate and up into a single tensor** (`ffn_gate_up_exps`) instead of two —
`gemma4` (Gemma 4 MoE) is one. This is still a single row: put the fused suffix first and
leave the tail `nullptr`, because to the streamer a fused `gate_up` is just an expert tensor
with a larger per-expert stride, discovered at runtime like any other.

```cpp
    { "gemma4", { "ffn_gate_up_exps", "ffn_down_exps", nullptr } },
```

Models with **shared/always-on experts** (a dense expert applied to every token, as in
`gemma4`, DeepSeek and some Qwen variants) work, but the shared expert stays resident and
reduces the streaming saving proportionally. Note it in the model's entry when you add one.
