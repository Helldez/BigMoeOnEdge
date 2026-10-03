# ADR-002: Reasoning echo for thinking hybrids

**Status:** Accepted (2026-09-17, commit `d83d153`). *2026-09-18 addendum: the client
cooperation cost is now optional — the bridge can perform the echo itself, see
[ADR-003](003-bridge-auto-echo.md).*

## Context

For thinking models, the client echoes back only the visible answer — the reasoning
tokens live in the cache but never in the next render, so the resident prefix and the
rendered history diverge at the first reasoning token and append reuse cannot fire
(ADR-001). KV surgery between reasoning and answer is impossible: entries are
position-baked and causally entangled, so the only legal cache operation is a suffix
chop — which drops the answer along with the reasoning.

Ground truth from the LFM2.5-8B template: it exposes a `preserve_thinking` chat-template
variable (default false) that renders history reasoning verbatim. That turns the problem
inside out — instead of rewinding state to before the thinking stage, **keep the
reasoning resident and make the next render reproduce it**.

## Decision

1. Add a `preserve_reasoning` request flag (opt-in, off by default) that sets
   `chat_template_kwargs["preserve_thinking"]=true` at render time. Implemented
   end-to-end: `GenerateRequest` (`core/include/bmoe/session.h`) → session JSON protocol →
   render path (`core/src/engine/session.cpp`) → OpenAI-compatible bridge
   (`scripts/bmoe-serve.py`).
2. Clients that use it echo the model's reasoning back inside the assistant `content`
   (e.g. `<think>…</think>answer`), so the next turn's render is a strict extension of
   the token mirror and the append path prefills only the new turn.

Measured (LFM2.5-8B-A1B, thinking hybrid, 3-turn echo chain, all answers correct):

| Turn | prefilled | reused |
|---|---|---|
| T1 | 28 | 0 |
| T2 | 17 | 101 |
| T3 | 17 | 235 |

Control without echo: full clear every turn, as designed.

## Consequences

- Easier: thinking hybrids get append reuse with zero engine risk — nothing is ever
  rewound; the resident prefix only grows. Decode on LFM2.5 measured ~9.2 tok/s in the
  resident regime.
- Harder: clients must cooperate (echo reasoning into `content`). No mainstream client
  does this today; the bridge can do it for them ([ADR-003](003-bridge-auto-echo.md)).
- Accepted as-is: reasoning tokens stay in context, consuming window and slightly
  conditioning later turns — deliberately chosen over prefill cost. Template-dependent:
  other families need the same `preserve_thinking`-style hook verified before the flag
  does anything.
