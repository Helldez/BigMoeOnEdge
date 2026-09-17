# Serving the engine to agent tooling

The engine has no HTTP server. It serves prompt requests over the `--session` stdin protocol
([telemetry.md](telemetry.md#session-mode)), which is what the Android app drives. For desktop
agent tooling — opencode, or anything that speaks the OpenAI Chat Completions API — a small
bridge wraps that protocol in an OpenAI-compatible HTTP endpoint, without adding an HTTP stack
or any dependency to the engine itself.

- `scripts/bmoe-serve.py` — stdlib-only Python bridge and the single entry point: spawns
  `bmoe-cli --session`, translates `POST /v1/chat/completions` (streaming and not) and
  `GET /v1/models` onto the line protocol, and keeps the model loaded between requests so the
  expert cache stays warm. It resolves engine and model itself (`--engine`/`--model`, else
  `$BMOE_ENGINE`/`$BMOE_MODEL`, else the host build of a repo checkout, else a `bmoe-cli`
  sitting beside the script in a staged bundle), with friendly errors instead of a raw
  subprocess failure.

```bash
python3 scripts/bmoe-serve.py -m ~/llm/models/LFM2.5-8B-A1B-UD-Q4_K_M.gguf \
    --model-id lfm2.5-8b-a1b --port 8017 \
    --engine-args "--chatml --moe-stream --ctx-size 16384 --ubatch 512"
```

`BMOE_ENGINE` overrides the engine binary, e.g. a cross-built ARM64 bundle. Point `--host
0.0.0.0` to serve other machines on the LAN; the default is loopback only.

What the bridge does with client fields:

- `messages` are flattened to a single user prompt (system messages kept inline); the engine
  renders its own chat template over its own conversation history (`--chatml` required).
- `max_tokens` (default 2048 — thinking models spend completion tokens on reasoning) becomes
  `n_predict`, and is **clamped to the bridge's `--max-tokens` ceiling**: a client asking for
  more output than fits beside its prompt would otherwise sit in the n_ctx window for tens of
  minutes. OpenAI clients treat `max_tokens` as a ceiling, so clamping is behavior-preserving.
  If a request still overflows `n_ctx`, the bridge retries once with the budget halved
  (floor 256) rather than failing — and if the prompt alone overflows, the error surfaces,
  because that genuinely needs a shorter conversation.
- Requests are serialised: the engine is one session, one generation at a time.
- Non-streaming responses carry a `bmoe` object with the `BMOE_DONE` perf block (tok/s, cache
  hit, stall) — the same numbers the CSV sink records. The SSE stream emits only OpenAI-shaped
  chunks: strict client SDKs validate every event, so no vendor-specific events ride the stream.

## Memory budget on the host

Compute buffers are reserved for the widest graph, and the dominant term scales with
`ubatch × vocabulary` — at ctx 8192 on LFM2.5-8B that measured **4.1 GiB** on a desktop host,
enough to OOM-kill the session the first time a generation ran with ~4 GiB free. `--ubatch 512`
caps the reservation at ~258 MiB and leaves decode speed untouched (only prefill splits into
more graphs). On RAM-constrained hosts, keep `--ubatch 512` and let `--cache-mb auto` size the
expert cache to what is left.

## Prefill is the wall on small hardware

Prompt processing (prefill) is compute-bound, and on modest CPUs it dwarfs everything else:
a 2015 dual-core laptop measured **~20 tok/s**, so an agent client that re-sends its multi-
thousand-token system prompt pays minutes before the first token. The engine-side knobs do
not move it — decode threads (2 vs 4), `--ubatch` 256/512/1024, and the CPU governor all
land within run-to-run noise; `--n-expert-used` does, but it drops the model's real routing
and answer quality with it (a benchmark knob, not a service one). The lever that works is
shrinking what the client sends, which is a client configuration problem:

- Set the model's real limits so agent clients compact before overflowing
  (`context` deliberately below the engine's `--ctx-size` to leave output headroom):

```jsonc
"lfm2.5-8b-a1b": {
  "name": "LFM2.5-8B-A1B (bmoe streamed)",
  "limit": { "context": 12288, "output": 2048 }
}
```

- Prefer a minimal agent profile: a ~100-token custom prompt instead of the client's full
  agent stack, and fewer enabled tools — every enabled tool's schema rides in the prompt on
  every request. In opencode that is a `"mode": "primary"` agent with a short `prompt`, the
  mutating tools disabled, and the bmoe model pinned.
- Keep conversations short: clients re-send the whole history each turn, and `/new` is
  cheaper than any cache.

## ARM64 Linux bundles

`scripts/build-arm64.sh` cross-compiles the engine for ARM64 **GNU/Linux** (needs the
`aarch64-linux-gnu` GCC toolchain) and stages a self-contained `bmoe-arm64/` bundle — the CLI
plus its shared libraries, RUNPATH `$ORIGIN/lib`, no `LD_LIBRARY_PATH` required:

```bash
scripts/build-arm64.sh            # stage bmoe-arm64/ (and bmoe-arm64.tar.gz with --tar)
scp -r bmoe-arm64/ target:/opt/   # then on the target:
/opt/bmoe-arm64/bmoe-cli -m model.gguf --moe-stream -p "hello"
```

Baseline is `armv8.2-a+dotprod+fp16` (any 2018+ ARM64 SoC; no i8mm, so older SoCs do not
SIGILL). The expert-ready hook is compiled in, so `--overlap` works. This is **not** the
Android build — Android binaries link bionic and only run on Android. See the bundle's own
`bmoe-arm64/README.md` for running the server from it (`BMOE_ENGINE=bmoe-arm64/bmoe-cli`).

Configuring the client (opencode) is a user-level concern, not a repo one: add an
`@ai-sdk/openai-compatible` provider pointing at `http://127.0.0.1:8017/v1`, either per project
or in `~/.config/opencode/opencode.json` for every session, and give the model the `limit`
from the prefill section above.
