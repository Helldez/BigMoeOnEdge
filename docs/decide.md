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
lives in. `BMOE_DECIDE` reports it as `prefix_state_mib`, and copying it after the prefix as
`store_s` (not part of `prefill_s`). A `generate()` drops it: it is worth its RAM only while
decisions follow one another, and the next decision after a chat turn builds a fresh one.

## With a prefill device

A session opened with `--prefill-device` prefills a decision by the same rule as a chat turn: the
prompt goes in pieces one ubatch wide, pieces at least `--prefill-min-tokens` wide run on the
device and a narrower tail on the CPU, and the weights are back on the host when the call returns.
`prefill_dev_tokens` in `BMOE_DECIDE` counts the tokens the device prefilled, and the other
`prefill_dev_*` keys what its expert arena read. The arena reads only the experts a decision routes
to, which on an agent's short prompts is about half of them ([npu-prefill.md](npu-prefill.md)).

No prefix state is kept with a prefill device: `auto` resolves to off, and `on` is refused at
startup. llama.cpp saves and restores a sequence through views of the KV cache it creates once,
over the buffer the cache was first allocated in. The prefill device moves the model state by
rebinding the cache tensors, which those views do not follow, so a saved state would be read from
memory the model no longer uses. The gates caught it: a restored prefix scored differently from the
same prefix computed. Every decision is prefilled whole instead, and gate G18g checks that it scores
bit for bit what the same session scores all on the CPU.

## What a decision assumes

- **Reasoning is off.** A decision reads the first token of the answer; with reasoning on, that
  token is the reasoning opener, so there is no `think` switch on the request.
- **A choice is scored by its standalone first token.** Each choice is tokenized on its own, with no
  template and no leading space. Under a chat template the answer opens a fresh assistant turn, so a
  bare label like `A` is the token the model would write. Without a template (a raw prompt ending in
  `Answer:`) the continuation is usually ` A`, and a tokenizer that prepends a space to every text
  scores `▁A`: there, end the prompt so the label follows it directly, or pass the label with the
  space the model would write.

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

## Diagnostics: `--decide-probe FILE` (experimental)

With `--decide`, appends one JSON line per decision to `FILE`: `seq`, `n_tokens`, `prefill_s`, the
`choices`, decide's own `best` and `logp`, and

- `lens`: per layer, the choice logits read from the last token's state at the end of that layer,
  put through the model's final norm and the head rows of the choice tokens (read from the gguf). For
  the last token this is exact, not an estimate: its state after layer L depends on layers 0 to L
  only, so it is what a prefill cut after layer L would answer. At the last layer it reproduces
  `logp` up to the device's fp16 rounding;
- `cnt` and `wsum`: per layer and expert, the (token, slot) routings the prefill made and the router
  weight they carried; `nu` the experts per token.

It reads the nodes through the router hook, so it works on a prefill device, where `--route-trace`
records nothing, and it changes nothing the graph computes. It needs `--moe-stream` or
`--prefill-device`. What it showed on Qwen3.6-35B-A3B, top-4, 35 decisions of an Android agent:
the answer to a screen is not readable from the lens before layer 35 of 40 (10 of 31 agree with the
final answer at layer 34, 31 of 31 at 35), and a short general question's from layer 31; both are
full-attention layers of this hybrid model, where the answer letter is looked up in the prompt. A
screen's prefill routes to about half of each layer's experts, the observation behind the routed
expert arena.

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
  decision and a refused one equals one that never saw them (e); a session without `--decide`
  refuses it harmlessly (f); and with a prefill device, three decisions (no prefix state kept) and a
  `generate()` after them equal the same session all on the CPU (g).
