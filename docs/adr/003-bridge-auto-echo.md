# ADR-003: Bridge-side reasoning auto-echo

**Status:** Accepted (2026-09-18). *2026-09-18 addendum (aider): with aider the message
array needs a request-independent canonical form before any of this can fire — aider
appends its edit-format boilerplate to the newest user turn only (the same turn shrinks
in the next request) and re-adds edited files with fresh content at a new position. The
bridge therefore strips the boilerplate from all user turns and relocates it into the
system prompt once. Measured: edit turns still full-clear (file content changed — a
hybrid cannot rewind, and the model must see the new file), but pure-question follow-ups
reuse 1683 tokens and prefill 26 in ~1.6 s instead of re-prefilling ~950.*

## Context

`preserve_reasoning` (ADR-002) makes thinking hybrids hit append reuse — but only if the
client re-embeds each reply's reasoning into its history as `<think>…</think>answer`.
No mainstream OpenAI-compatible client does this, so real agent tooling (aider, opencode)
kept paying full re-prefill on every thinking-hybrid turn.

The bridge is the one component that sees both sides: the exact `(reasoning, answer)`
span the model generated, and the history the client sends back. It can rewrite the
history server-side, leaving clients untouched.

## Decision

1. `scripts/bmoe-serve.py --auto-echo` (off by default): after each successful
   non-cancelled reply, record its exact `(reasoning, answer)` span in a bounded
   in-memory registry (16 entries, newest-wins on duplicate answers).
2. On every request, assistant history turns whose `content` matches a recorded answer
   verbatim are rewritten to the recorded span. Precedence: registry record > client
   `reasoning_content` field > no rewrite. Turns already containing `<think>`, edited or
   regenerated answers, and non-assistant roles are never rewritten — an unmatched turn
   simply falls back to the engine's safe full re-prefill.
3. Rewriting happens before the engine sees the messages and is not persisted; the
   client's own conversation state is never modified (the response history stays the
   client's).

## Consequences

- Easier: any unmodified OpenAI client gets delta-only prefill on thinking hybrids —
  the mechanism ADR-002 measured, without client cooperation.
- Harder: the registry duplicates reasoning text in bridge memory (bounded, process
  lifetime — lost on restart, after which turns re-prefill once); the bridge now
  mutates request semantics, so debugging must distinguish what the client sent from
  what the engine saw (the bridge log's `msgs` count reflects the rewritten array).
- Accepted as-is: exact-string matching only. A regenerated or client-edited answer
  never matches (correct behavior — the reasoning for a different answer is the model's
  guess, not a record); a smarter fuzzy match is deliberately not attempted.
