# Draft models (`--drafter`)

`--drafter --drafter-file FILE` is the third draft source for the speculative loop [`--mtp`](mtp.md)
and [`--ngram`](ngram.md) drive. The draft comes from a **draft model**: a small network trained for
one target model, shipped in a GGUF of its own, that reads the target's hidden states and proposes
the next tokens. A block-diffusion drafter (DFlash, and DSpark, which adds a small autoregressive
head to it) proposes a whole block in **one** forward pass, where a head drafts a token per pass.
Off by default.

The verify half is the one every source shares: the target decodes the confirmed token and the
drafts in one wider batch, the longest prefix that equals its own argmax is kept, and the rest is
rolled back. So the same two statements hold as for the other sources, and
[mtp.md](mtp.md#it-is-exact-but-it-is-not-bit-reproducible) is where they are argued: nothing is
approximated (a draft is only ever accepted when it is what the model itself would have produced),
and the run is **not** bit-reproducible against an unspeculated one, because a batched verify is not
the same arithmetic as single-token decodes.

## Nothing here names a kind of drafter

The engine does not know what DFlash is. What a draft file is comes from its header, read by
llama.cpp's own speculative driver (`common_speculative_types_from_gguf`), and the same driver
moves the hidden states, runs the draft graph and keeps the draft state. The engine supplies the
loop around it and three things the driver's answer decides, all in one module
(`core/src/engine/draft_driver`):

- how the file is loaded (whole and resident, on the host or on the target's devices);
- the context it drafts in (an ordinary one over the draft model, reading the target through
  `ctx_other`; a head is the one kind that needs a context of the MTP type);
- the driver built over the two contexts.

A kind of drafter the driver learns with a submodule bump therefore needs no change in the engine,
no row in a table and no flag. Two consequences worth stating:

- **A file that turns out to hold an MTP head is driven as one.** `--drafter-file head.gguf` and
  `--mtp --mtp-head head.gguf` are then the same run. The converse is refused: `--mtp-head` takes a
  head and nothing else, because `--mtp` says the draft is the model's own head.
- **A file that is not a draft is refused at load**, by the parameter that named it, and so is a
  drafter whose vocabulary is not the model's: a drafter is trained for one model and converted with
  that model's tokenizer.

## Getting one

A drafter is trained for a specific model, and published as a checkpoint of its own next to it. Use
a GGUF conversion of it, or convert the checkpoint with llama.cpp's converter, which needs the
target's directory so the drafter inherits its tokenizer:

```bash
python third_party/llama.cpp/convert_hf_to_gguf.py <drafter checkpoint> \
    --target-model-dir <the model it was trained for> --outtype bf16 --outfile drafter.gguf
```

Then:

```bash
bmoe-cli -m model.gguf --drafter --drafter-file drafter.gguf --draft 8 -p "..."
```

A full-vocabulary drafter carries no token embeddings and no output head: it uses the target's,
through the context it drafts for. That is why the file is small beside the model, and it is the
reason the draft context is created with the target context as its `ctx_other`.

## The settings it shares, and the one it does not

| Setting | With a drafter |
|---|---|
| `--draft N` | Tokens drafted per verify batch, `1` to `8`. The driver clamps it to the block the drafter was trained on. |
| `--mtp-p-min F` | Stops a draft at the first token the drafter gives less than this probability (a DSpark drafter with a confidence head uses that head). `0`, the default, always drafts the full width. |
| `--drafter-on-devices` | The draft model's weights on the devices the model's layers are on, instead of the host. With `--auto` the plan sets it. |
| greedy decoding | Required, as for every source: `validate()` rejects speculation with `--temp` above 0. |

`--drafter-on-devices` is the same decision as `mtp-head-on-devices`, taken by the same planner
rule under its own name: where the plan arms a device expert cache, a draft file follows the experts
to that device and takes its bytes from the cache, never from the layers the fitter placed, and
where it does not fit above one token cycle it drafts on the host and the plan says so. The rule
knows the file's size and nothing about what is in it.

The draft width stops at `8` for every source. It is an evidence boundary, not a physical one
(`SpecConfig::draft_max_limit`): a block drafter is trained on wider blocks than that, and raising
the bound for it is a measurement this engine has not made yet (see [What is measured](#what-is-measured)).

## Why it can lose here

For the same reason every source can, and it is the reason this is a setting and not a default.
The verify positions route **independently**, so a layer's read set widens from `k` experts toward
`N × k` wherever routing differs across adjacent tokens. Where the experts are resident and the
compute is on a device, a wide verify costs little more than a narrow one and a block of accepted
tokens is nearly free. Where the experts are streamed, the widened read set is flash reads, and a
rejected draft pays for them and buys nothing. A drafter's own cost is the opposite of a head's on
that axis: it routes no experts of the model (it is a small dense network unless its header says
otherwise), so drafting itself reads nothing from the model file.

Acceptance is a property of the drafter and of the text. A drafter trained on answers without
reasoning text drafts reasoning text badly; one trained for another revision of the model is a
different model's drafter.

## How it is wired

- **Load** (`Session::open`): the draft kind is read from the header; the file is loaded as a model
  of its own with `detail::draft_file_model_params`.
- **Contexts**: the target gets `n_rs_seq = --draft`, as for every source, so a rejected tail is a
  restore and not a replay. The draft context comes from `detail::draft_context_params`: its own
  narrow graph and KV, no rollback snapshots, no device expert cache, no eval callback (its weights
  are not the streamer's), and the per-sequence output cap the driver's own sizing rule gives a
  sequence drafting `--draft` tokens, since a block drafter emits its whole draft in one decode and
  has it sampled on the backend.
- **Prefill** goes through the driver, as with the head: the drafter's KV is filled from the
  target's hidden states for every prompt chunk, or the first draft is conditioned on a state that
  never saw the prompt.
- **Per step**: draft, rewind the draft context to where the draft started, verify on the target,
  let the draft context catch up on the accepted prefix from the target's own hidden states, roll
  the target back over the rejected tail. The loop is the one in
  [mtp.md](mtp.md#how-it-is-wired); only the producer changed.
- **Checkpoints and the device cache**: the draft context's state is saved beside the target's
  (`context_checkpoints`), and when the device expert cache makes the contexts again the driver is
  made again over the new pair.

As with the head, speculation is off for a conversation holding an image: the draft reads tokens,
and an image's are embeddings.

## Reading the numbers

The CLI's summary lines are the ones [mtp.md](mtp.md#reading-the-numbers) explains, prefixed
`drafter:`; the counters keep their historical `mtp_` names in the summary and the CSV whatever the
source (`mtp_drafted`, `mtp_accepted`, `mtp_decodes`, `drafted_steps`), and the CSV header says
`spec=drafter`. The split of streamed bytes into "the head's own routing" and "the widened verify
batch" is printed for the head only: a drafter reads no expert of the model.

## What is measured

`tests/drafter_session_test.cpp` (the `drafter_session` test) runs a real session over the tiny
MoE fixture, streamed, with a tiny block-diffusion drafter generated for it
(`scripts/make-tiny-moe.py --arch dflash`). Its weights are random, so nearly every draft is
rejected, which is the path a mistake corrupts silently: a second model loaded, hidden states moved,
a block drafted, a wide verify, the tail rolled back, and the same again on a second turn. It also
proves the two refusals above. It proves the machinery, not a speed.

No speed is claimed here. Whether a drafter pays on a given machine is the measurement the
[benchmark method](benchmark-method.md) describes, against the same model without it, and the
verify-width caveat above says which way to expect it to go where the experts are streamed.
