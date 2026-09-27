# Decisions: choose, do not generate (`--decide`)

A decision asks the model which of a fixed list of choices it would answer next, and reads the
answer from the next-token distribution after **one prefill**. Nothing is decoded.

On this engine that matters more than usual. Decode streams experts from flash token by token, and
on a model larger than RAM it is the slow part; a decision skips it entirely and costs what reading
the prompt costs. The shape fits any caller that picks from a list: an agent choosing the next UI
action among lettered candidates, a router, a classifier, a multiple-choice benchmark.

The feature is **off by default**. A session opened without it refuses decide requests (not fatally)
and allocates nothing for them.

## Using it

From the CLI, in session mode:

```
bmoe-cli -m model.gguf --chatml --moe-stream --session --decide
```

then one request per line on stdin:

```
{"cmd":"decide","id":1,"prefix":"Task: turn on Wi-Fi. History: none. ",
 "suffix":"Screen: Settings. Options: A) Network B) Display C) Battery. Answer with the letter.",
 "choices":["A","B","C"]}
```

and one `BMOE_DECIDE` line back per request (the full schema is in
[telemetry.md](telemetry.md#session-mode)):

```
BMOE_DECIDE {"id":1,"best":0,"choice_logp":[-0.014,-6.76,-11.0],"n_tokens":69,"n_reused":31,...}
```

From C++, `Session::decide(const DecideRequest &)` with `SessionConfig::decide.enabled = true`
(`bmoe/decide.h`). The Android app exposes it as a **Choose from options** switch on the chat
screen: the question is the prefix, the lettered options are the suffix, and each option is shown
with the probability the model put on it.

## What it returns

- `choice_logp[i]`: log p of the **first token** of `choices[i]`, normalised over the whole
  vocabulary, not over the choices. The mass the model puts outside the choices is how unsure it
  is, and a caller fitting an abstention threshold needs exactly that.
- `best`: the index of the highest.
- The prompt phase's cost, with the same keys and rules as `BMOE_DONE`'s `prefill_*` fields.

Choices are scored by their first token, so two choices whose first tokens coincide cannot be told
apart. Such a request is **refused before anything is prefilled** rather than answered with a tie.
Single-token labels (`A`, `B`, `C`, ...) avoid the collision by construction. The same up-front
refusal applies to an empty choice list and to a prompt longer than the context.

## The prefix and the kept state

A caller making a sequence of decisions repeats most of its prompt: the instructions, the task, the
history so far. So the prompt comes in two parts. The chat template is rendered over
`prefix + suffix` as **one** user message, so the split changes nothing about what the model sees;
it only says which part is worth remembering.

With the prefix cache on, the session keeps the model state after the prefix and restores it when
the next request's prefix renders to the same tokens or **extends** them (an agent's history only
grows, so each step's prefix extends the last). Only the rest is prefilled, and the longer prefix is
kept for the next call. The state is the whole sequence state, recurrent layers included, so it
works on hybrid models whose memory cannot be trimmed back to a prefix.

Where the prefix ends is found on tokens, never on text: the prefix is also rendered alone, and the
two renderings agree up to the join. One matching token is given back, because a tokenizer merge
across the join can make the last shared-looking token a different token in context.

`--decide-prefix-cache auto|on|off` (default `auto`):

- `auto` keeps the state where prefilling a prompt in two pieces costs about what the pieces cost
  together, which is the CPU prefill this engine runs. A backend on which every prefill graph costs
  the same at any width (an accelerator that streams the whole expert set per graph) would answer
  the other way, and there a second graph for the few tokens after the prefix costs a whole pass.
- `on` / `off` force it. A request can also opt out on its own with `"reuse_prefix":false`: it is
  then prefilled whole and the kept state is left as it was.

The kept state costs memory, and on a phone that memory comes out of the same RAM the expert cache
lives in. `BMOE_DECIDE` reports it as `prefix_state_mib`.

## Measured

A correctness run on a PC, Qwen3.6-35B-A3B Q4_K_M streamed with the expert cache off and little free
RAM (so the times are not a benchmark), four decisions of an agent turning on Wi-Fi:

| decision | answer | p(answer) | reused / prefilled tokens | prefill |
|---|---|---:|---:|---:|
| home screen | A, open Settings | 0.989 | 0 / 70 | 15.8 s |
| settings, same prefix | A, Network | 0.986 | 31 / 38 | 8.1 s |
| network, grown prefix | B, Internet | 0.998 | 31 / 53 | 12.4 s |
| same, `reuse_prefix:false` | B, Internet | 0.999 | 0 / 84 | 13.1 s |

The prefix state of this hybrid model is 63 MiB at 31 tokens: its recurrent state has a fixed size.
Restoring it took 7 to 8 ms.

Two things this run shows that the design has to be honest about:

- **A restored prefix is exact; a differently chunked prefill is not.** The last two rows answer
  the same, but the log-probs of the unlikely choices differ by about 0.3 nats. That is not the
  restore: llama.cpp does not round the same when a prompt is prefilled in different pieces, and a
  grown prefix is by construction prefilled in different pieces than a fresh prompt. The gates
  below check what must hold exactly.
- **What reuse saves depends on the regime.** With the expert cache off, a prefill graph's cost is
  dominated by reading the experts its tokens route to, which grows slower than the token count.
  The second decision prefilled 38 tokens in 8.1 s, against 15.8 s for the first one's 70; the
  grown prefix, prefilled in two pieces, took 12.4 s against 13.1 s for the same prompt whole.

## How it is built

`core/src/engine/decide/` is policy over token ids and a logit row, with no llama.cpp include:

| File | Role |
|---|---|
| `backend.h` | `IDecideBackend`, the port: render, tokenize, prefill, logits, save/load state |
| `prompt_split` | where the prefix ends, on tokens |
| `choice_scorer` | the up-front choice checks and the log-softmax scoring |
| `prefix_cache` | `IPrefixCache`, `LastPrefixCache` (one kept state, the latest prefix), the `auto` policy |
| `decider` | `run_decide`, one decision start to finish |
| `llama_backend` | the only adapter over a live llama.cpp context |

`Session::decide()` hands the adapter the pieces of the session it touches and nothing else. The
log-softmax is shared with `perplexity()`, and the chat turn is rendered by the same function
`generate()` uses, so a decision and a first chat turn over the same text see the same prompt.

## Tests

- `decide_policy` (no model): the split rule, the choice checks and scoring, when the kept state is
  restored, stored or left alone, and which failures leave the session usable, over a scripted
  backend whose logits are a pure function of its state.
- Gate **G18** on the tiny models: a restored prefix scores bit for bit what a fresh session
  computing it scores (a); a state stored after a partial restore answers bit for bit as it did when
  computed (b); a whole-prefill decision equals `perplexity()`'s choices after the same text (c);
  resident == streaming, with and without an evicting cache (d); a `generate()` continuing after a
  decision and a refused one equals one that never saw them (e); and a session without `--decide`
  refuses it harmlessly (f).
