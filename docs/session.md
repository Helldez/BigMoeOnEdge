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

## Independent prompts vs chat

`GenerateRequest::clear_kv` (default `true`) clears the KV cache before each prompt, so prompts
are independent while the expert cache stays warm. Setting it `false` continues the KV cache for
multi-turn chat (the caller renders the running conversation into the prompt). The default keeps
the two concerns separate: the KV cache is per-conversation, the expert cache is per-model.

## Cancel

`Session::cancel()` is thread-safe and interrupts an in-flight `generate()` at the next decode
boundary via llama's abort callback (installed unconditionally at open, so it works in serial and
overlap alike). It leaves the model and cache intact; the returned `RunResult` has `cancelled =
true`. Cancel is distinct from a fatal streaming error, which is sticky and ends the session.

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
