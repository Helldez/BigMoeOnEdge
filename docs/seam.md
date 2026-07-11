# The seam: how we hook llama.cpp without forking it

Everything that connects BigMoeOnEdge to llama.cpp goes through two public mechanisms.
This file documents the exact contract so it can be re-verified when the submodule is
updated.

## 1. The eval-callback

`llama_context_params.cb_eval` / `cb_eval_user_data` (public) install a function called by
`ggml_backend_sched` for every graph node:

- `callback(node, ask=true, ud)` is called for each node. Returning true isolates that
  node: the scheduler computes it alone, `ggml_backend_synchronize`s, then calls
  `callback(node, ask=false, ud)`.
- Returning false groups the node with its neighbours for normal computation (no non-ask
  callback).

We use both phases:

**Capture phase** (one warm-up decode). `ask` is called for every node, so we scan each
node's `src[]` for expert weight tensors (`blk.<il>.<suffix>.weight`, where the suffixes
come from the arch's recipe — `ffn_{gate,up,down}_exps` for the split layout, a fused
`ffn_gate_up_exps` for others) and record the live `ggml_tensor*`. We return false
throughout — capture observes, it does not isolate. `ggml_tensor` is a public struct, so
reading `->name`, `->ne`, `->nb` and writing `->data` is public API surface.

**Stream phase** (real generation). We return true only for `ffn_moe_topk-<il>`. The
non-ask callback then hands us that node with the selected expert ids materialized; we
gather them (stride-aware) and trigger the slice reads.

## 2. gguf offsets

`gguf_init_from_file(..., no_alloc=true)` + `gguf_get_data_offset` +
`gguf_get_tensor_offset` (all public) give each tensor's absolute byte offset in the file,
without loading any tensor data. We match these to the captured tensors by name.

## The one ggml behaviour we depend on

That a node marked "needed" is computed and synchronized **before** the non-ask callback,
and that the batch containing the dependent expert matmul runs **after** the callback
returns. This is how `ggml_backend_sched` implements the eval-callback today
(`ggml/src/ggml-backend.cpp`). It is not a stability-guaranteed contract, so:

- the [byte-identity gates](../tests/moe_gates.cpp) assert lossless output, which fails
  loudly if the ordering ever changes;
- CI runs the gates on every submodule bump.

## Upgrading llama.cpp

```bash
cd third_party/llama.cpp && git fetch && git checkout <newer-tag>
cd ../.. && git add third_party/llama.cpp && scripts/build-host.sh
cd build && ctest --output-on-failure     # gates must stay green
```

Most bumps are exactly this: update, rebuild, gates green. The fragility is not the public
API but the *internal naming conventions* the seam attaches to — the tensor suffixes and
the `ffn_moe_topk` node name are how llama.cpp happens to build MoE graphs today, not a
guaranteed contract, so upstream can rename or restructure them (Gemma 4's fused
`ffn_gate_up_exps` is one such evolution we absorbed with a recipe row). The gates are the
enforcement: a rename breaks byte-identity before merge instead of silently corrupting
output. Each supported architecture adds one more gate to keep green across a bump.

If a future release moves the two hooks (a stable expert-residency API, say) upstream,
this seam shrinks further or disappears — `core/` does not change.

Pinned submodule at the time of writing: upstream `ggml-org/llama.cpp` master
`22b69b6` (see `.gitmodules` / `git submodule status` for the current pin).
