# Working on BigMoeOnEdge (agent guide)

Read this before making changes. It captures the invariants that keep this project clean.

## What this is

A ports-and-adapters engine that streams MoE experts from flash so >RAM models run on
device, built **on top of** llama.cpp's public API. The whole value proposition is that
we do not fork llama.cpp. See `docs/architecture.md` and `docs/seam.md`.

## Project map

- `core/include/bmoe/` — ports (interfaces) + config. Pure policy, no llama.cpp include.
- `core/src/io/` — `platform_io` (cross-platform O_DIRECT reads + reserve/commit/evict VM);
  `file_reader` (pooled positioned reader, per-consumer O_DIRECT — used by both the expert stream
  and the dense loader).
- `core/src/moe/` — `gguf_offsets`, `arch_registry`, `expert_stream_source`, `router_hook`;
  `dense_weights` (the non-expert weight policy: mmap / warm / anon, plus the residency sensor).
- `core/src/engine/runtime.cpp` — composition + greedy generation loop.
- `cli/main.cpp` — `bmoe-cli`; the ONLY place environment variables are read.
- `third_party/llama.cpp` — stock upstream submodule.
- `tests/` — byte-identity gates. `examples/android/` — the demo APK.

## Hard rules

1. **Never patch llama.cpp in-tree.** Everything goes through the public eval-callback and
   public gguf/model APIs. If a change seems to need a llama.cpp edit, stop and discuss —
   the fallback is a *separate* 1-commit fork branch on `Helldez/llama.cpp`, never an
   in-tree diff, and only after agreement. Upgrading llama.cpp must stay a submodule bump.
2. **Repack stays off.** The engine loads with `use_mmap=true, use_extra_bufts=false`.
   The streamer rebinds `tensor->data` to the native gguf layout; repacking breaks it.
   This is load-bearing, not a tunable.
3. **No env vars in the library.** `core/` never calls `getenv`. Config flows through
   `RunConfig`; the CLI resolves any env overrides before building it.
4. **No hardcoding.** New architectures are recipe rows in `arch_registry.cpp`; expert
   counts, strides and offsets are discovered at runtime. No model-specific constants in
   the streaming path.
5. **Gates must pass before merge.** `bmoe_moe_gates` proves streamed == resident. If you
   touch the streamer, the seam, or bump the submodule, run them.

## Build and test

```bash
git submodule update --init --recursive
scripts/build-host.sh
cd build && ctest --output-on-failure         # byte-identity gates (needs python3 + gguf)
```

Android CLI: `pwsh scripts/build-android.ps1` (needs the NDK), then build the APK in
`examples/android`.

## Conventions

- **Commits:** Conventional Commits (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`,
  `build:`, `ci:`, `chore:`). Author is **Helldez only** — do NOT add AI co-author or
  session trailers to commits.
- **Language:** all code, comments, docs, and commit messages in English.
- **Style:** `.clang-format` (LLVM base, 4-space, 120 col). Comments explain *why* /
  invariants, not *what*.
- **No milestone codenames** (M0…Mn) in docs — describe capabilities thematically.

## Where numbers come from

Benchmark figures in the docs are measured (OnePlus 15R, Qwen3-30B-A3B-Q4_K_M). Don't
invent or round them silently; if you re-measure, update `docs/benchmark-method.md` and the
README table together.
