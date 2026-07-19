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

## 3. The expert-ready hook (fork extension)

Sections 1 and 2 are enough for the *serial* streamer: block on the expert reads, then let
the layer compute. Overlapping the two — reading a token's experts while the same token's
expert matmuls are running — needs a wait point that no public API exposes. That is the one
place where BigMoeOnEdge carries a llama.cpp extension.

**What it is.** A single optional hook, ~25 lines, living on the fork branch
`bmoe/expert-ready-hook` of `Helldez/llama.cpp` as a single commit on top of the upstream
pin. It adds nothing to the model files and changes no data layout; it is a callback the
CPU MoE kernel invokes.

**Exact API and call site.**

```c
// ggml-cpu.h
void ggml_cpu_set_expert_ready_hook(ggml_expert_ready_hook_t hook, void * user_data);
```

`ggml_compute_forward_mul_mat_id` calls the hook at the top of its per-expert loop, right
after the "expert not routed this token" skip, before it consumes that expert's weight
slice. Every compute thread calls it for every routed expert; the hook may block. There is
**no barrier inside the expert loop**, so a thread blocking on one expert cannot deadlock
the threadpool — other threads proceed to the experts whose slices are already resident.
The streamer's hook blocks until the requested expert slice has been read in, then returns.
When no hook is registered (stock upstream, or `--overlap` off) the call is a single null
check — zero cost.

**Why it exists.** The topk eval-callback (section 1) is the only public hook near routing,
and it can only fire *before* the expert matmuls of a layer run — it cannot pause partway
through them. Overlapping expert reads with expert compute requires a per-expert wait point
*inside* the kernel, which the public API does not provide. Hence the extension.

**Graceful degradation.** CMake probes `ggml-cpu.h` for the hook symbol and, when present,
defines `BMOE_HAVE_EXPERT_READY_HOOK`. Built against stock upstream (symbol absent) the
whole project still compiles and runs — the serial streaming path is unchanged; only
`--overlap` is affected, and it fails with a clear runtime error instead of silently
falling back.

**Sunset condition.** This fork exists *solely* for this one hook. The moment upstream
ships an equivalent per-expert readiness/residency callback, the branch is dropped and the
submodule bumps straight back to `ggml-org/llama.cpp`. It is a tide-me-over until the wait
point is public, not a divergence we intend to maintain.

## The chat glue: llama.cpp `common` (not the streaming seam)

Separate from the two streaming hooks above, `session.cpp` links llama.cpp's `common`
library for one thing: rendering the model's own chat template and parsing reasoning output.
`common_chat_templates_init` / `common_chat_templates_apply` run the real Jinja template the
gguf ships (so Gemma's channel format, Qwen ChatML, etc. all format correctly, driven by the
model rather than hardcoded), and `common_chat_parse` extracts a reasoning model's thinking so
it can be reported apart from the answer. The parser-params wiring lives in its own translation
unit, `chat_parse.cpp` — the PEG parser arena has to be loaded explicitly or `common_chat_parse`
throws on the first token, which is how issue #49 stayed invisible; keeping it separate makes
that seam unit-testable without a model.

A second translation unit, `thinking_control.cpp`, crosses the same boundary for "thinking off".
`enable_thinking` is only a *request* to the template, and many templates never read it, so the
engine renders the template to find out (three renders at open, no model names involved) and, where
the flag is inert *and* reasoning is a structural section of the format, asks for a **continuation**
instead: the `continue_final_message` field of `common_chat_templates_inputs`, plus a synthetic
trailing assistant message, makes llama.cpp's own per-template handler emit that family's "reasoning
is over" span into the prompt. This is why no `<think>` or harmony channel marker appears anywhere in
`core/` — the markers stay upstream, where a submodule bump keeps them current.

Whether the continuation is *binding* is read off `common_chat_params::thinking_start_tag`/
`thinking_end_tag`: a model that declares a reasoning span owns it, so a pre-closed empty one is a
suggestion it can decline (LFM2.5 does), while a model that declares none separates reasoning
structurally and cannot. Both facts come from the loaded model, never from its name.
`tests/think_control_test.cpp` pins all of it against the vendored templates, again with no model.

Unlike the public-C-API streaming seam, `common` is **not a stable API** — it can change
between upstream versions. So a submodule bump may require updating this chat glue in
`session.cpp` / `chat_parse.cpp` / `thinking_control.cpp`; the build and gates catch a break at
compile time rather than at runtime (`tests/chat_parse_test.cpp` and
`tests/think_control_test.cpp` cover these seams directly). This
trade-off is deliberate and is also noted at the link site in the root `CMakeLists.txt`. The
gates themselves run with the template off (raw prompt), so they stay deterministic and are
unaffected by this dependency.

## The one ggml behaviour we depend on

That a node marked "needed" is computed and synchronized **before** the non-ask callback,
and that the batch containing the dependent expert matmul runs **after** the callback
returns. This is how `ggml_backend_sched` implements the eval-callback today
(`ggml/src/ggml-backend.cpp`). It is not a stability-guaranteed contract, so:

- the [byte-identity gates](../tests/moe_gates.cpp) assert lossless output, which fails
  loudly if the ordering ever changes;
- CI runs the gates on every submodule bump.

## Upgrading llama.cpp

Because the submodule pins the `bmoe/expert-ready-hook` fork branch (section 3), a bump
rebases that 1-commit branch onto the new upstream tag, re-pushes it, and re-pins:

```bash
# in a Helldez/llama.cpp checkout: rebase the single hook commit onto the new tag
git fetch upstream && git checkout bmoe/expert-ready-hook
git rebase <newer-upstream-tag> && git push --force-with-lease origin bmoe/expert-ready-hook

# in this repo: move the submodule to the rebased commit, rebuild, run the gates
cd third_party/llama.cpp && git fetch origin && git checkout <rebased-commit>
cd ../.. && git add third_party/llama.cpp && scripts/build-host.sh
cd build && ctest --output-on-failure     # gates must stay green
```

When the sunset condition lands (upstream ships the readiness callback) this collapses back
to a plain `git checkout <newer-tag>` against `ggml-org/llama.cpp` with no branch to carry.
Either way the gates are the enforcement. The fragility is not the public
API but the *internal naming conventions* the seam attaches to — the tensor suffixes and
the `ffn_moe_topk` node name are how llama.cpp happens to build MoE graphs today, not a
guaranteed contract, so upstream can rename or restructure them (Gemma 4's fused
`ffn_gate_up_exps` is one such evolution we absorbed with a recipe row). The gates are the
enforcement: a rename breaks byte-identity before merge instead of silently corrupting
output. Each supported architecture adds one more gate to keep green across a bump.

If a future release moves the two hooks (a stable expert-residency API, say) upstream,
this seam shrinks further or disappears — `core/` does not change.

Pinned submodule at the time of writing: `Helldez/llama.cpp` branch
`bmoe/expert-ready-hook`, commit `5236140` — the single expert-ready-hook commit (section 3)
on top of upstream `ggml-org/llama.cpp` master `22b69b6` (see `.gitmodules` /
`git submodule status` for the current pin).
