# Session mode

A fresh `bmoe-cli` process per prompt re-pays two fixed costs every time: the model load (tens
of seconds for a >RAM model) and the expert-cache warm-up ramp (the LRU cache starts empty, so
the first tokens miss often and read far more from flash than the steady state — see
[benchmarks.md](benchmarks.md)). Streaming was meant to avoid exactly this kind of repeated work.

Session mode amortises both. `Session` (`core/include/bmoe/session.h`) loads the model, discovers
the MoE expert tensors, and initialises the expert source **once**; each `generate()` then runs a
prompt against that resident state, and the expert LRU cache **survives between calls**, so a
second prompt starts warm.

```cpp
std::string err;
auto s = Session::open(cfg, err);      // model load + capture + source.init — once
s->generate({.prompt = "..."});        // prefill + decode; cache filled
s->generate({.prompt = "..."});        // starts warm — no reload, no cold ramp
```

`run()` (`runtime.h`) is a thin one-shot wrapper over `Session` — open, one generate, close — so
the byte-identity gates exercise the same machinery an interactive session uses. Gates **S1/S2**
assert that a second, warm generate produces output identical to the cold one-shot reference:
warming the cache changes latency, never the bytes.

## Independent prompts vs multi-turn chat

`GenerateRequest::clear_kv` selects between two modes:

- **`true` (new chat / independent prompt):** the KV cache and the engine-held conversation are
  dropped before the prompt runs. The expert cache stays warm. This is the one-shot path — `run()`
  always uses it, and the byte-identity gates exercise it.
- **`false` (continue):** the prompt continues the current conversation. In chat mode the Session
  owns the conversation (`chat_history`) and re-renders the model's chat template over the *whole*
  history each turn, so the caller sends only the new user message — not the running transcript.

**KV prefix reuse.** Re-rendering the full history would re-tokenize the entire conversation, but
most of it is already decoded into the KV. Each turn the engine diffs the freshly rendered tokens
against `kv_tokens` (the tokens currently in the KV, in order), keeps the common prefix, removes the
divergent tail with `llama_memory_seq_rm`, and prefills only the suffix. So a follow-up turn pays
for its own tokens, not a full re-prefill — which matters because prefill is the slow phase on
device. `BMOE_DONE.n_prompt` reports the tokens actually prefilled this turn; `n_past` is the total
context length after it.

**Fallbacks and costs.** SWA-style memory (e.g. Gemma) can refuse a partial `seq_rm`; the engine
then clears the KV and re-prefills the whole prompt for that turn (correct, just slower). With
thinking **on**, the template strips the previous turn's reasoning on re-render, so the rendered
prefix diverges at the last answer and up to one answer's worth of tokens is re-prefilled per turn;
with thinking **off** (the app default) the re-fed suffix is just the new user turn plus a few
wrapper tokens.

**Reasoning is returned, not discarded.** On a thinking model the Session parses the reasoning span
out of the raw stream and carries it in its own field (`TokenMetrics`/`RunResult::reasoning`, and
`reasoning` on `BMOE_PROGRESS`/`BMOE_DONE`) rather than dropping it. The answer text stays free of
it either way, so the byte-identity gates are unaffected; a caller that wants to show the thinking
reads the separate field. The parser wiring lives in `core/src/engine/chat_parse.cpp`
(see [seam.md](seam.md)).

## Cancel

`Session::cancel()` is thread-safe and interrupts an in-flight `generate()` at the next decode
boundary via llama's abort callback (installed unconditionally at open, so it works in serial and
overlap alike). It leaves the model and cache intact; the returned `RunResult` has `cancelled =
true`. In chat mode a cancel **rolls the turn back** to the reused prefix (dropping this turn's KV
and un-appending the user message) so prior turns stay usable and the conversation can continue.
Cancel is distinct from a fatal streaming error, which is sticky and ends the session.

## Fixed context

`n_ctx` and `n_batch` are baked into the llama context at `open()`, before any prompt is known, so
size them for the longest prompt + generation the session will serve. A request that would overflow
`n_ctx` is rejected without tearing the session down.

## CLI and app

`bmoe-cli --session` exposes this over a line protocol (requests on stdin, `BMOE_*` responses on
stdout — see [telemetry.md](telemetry.md)). The Android example runs one such process per model:
the first prompt loads the model, later prompts reuse the warm process, and the session is freed on
an explicit **Unload** or after an idle timeout. Changing the model or any streaming setting
reopens the session; changing only the prompt, `n_predict`, or the thinking toggle does not.
