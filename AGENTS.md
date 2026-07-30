# Working on BigMoeOnEdge (agent guide)

Read this before making changes. It captures the invariants that keep this project clean.
It follows the [AGENTS.md](https://agents.md) convention, so any coding agent picks it up;
`CLAUDE.md` just points here.

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

## Build and test

```bash
git submodule update --init --recursive
scripts/build-host.sh
cd build && ctest --output-on-failure         # byte-identity gates (needs python3 + gguf)
```

Android CLI: `pwsh scripts/build-android.ps1` (needs the NDK), then build the APK in
`examples/android`.

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
6. **Docs and changelog ship with the change.** Every PR updates `CHANGELOG.md` and the docs
   it invalidates, in the same PR — never as a later sweep. A release gets its own dated
   `## [X.Y.Z] - YYYY-MM-DD` section; nothing accumulates under `[Unreleased]`. Check in
   particular: the README benchmark tables and model list, the `docs/architecture.md` layer
   map, `docs/seam.md` when the llama.cpp boundary moves, `docs/telemetry.md` when CSV
   columns or the `BMOE_*` protocol change, `docs/roadmap.md` when a listed future item
   ships, and `examples/android/README.md` when the catalog, settings or build flow change.
   Docs that name a file the code no longer has are worse than no docs.
7. **Review exactly what is being published, every time.** This repo is public and every push
   is permanent record. Before any commit, push, PR or release: run `git status --short` and
   stage by explicit path only — never `git add -A` / `git add .`; untracked files in the
   working tree are not yours to publish. Logs, CSVs and bench evidence get a scan for
   identifying data (device model codes, local paths, addresses) before landing in `docs/`;
   phrase the test device generically. After a squash-merge, verify the landed tree
   (`git ls-tree`) before pushing anything else.

## Conventions

- **Commits:** Conventional Commits (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`,
  `build:`, `ci:`, `chore:`). Author is **Helldez only** — do NOT add AI co-author or
  session trailers to commits.
- **Group commits.** One commit = one coherent change. Never a commit for a trivial
  tweak on its own — fold small fixes, doc touches and follow-ups into the change they
  belong to. If several small things accumulate, batch them into one commit.
- **Delete the branch when its PR closes** — merged or rejected, local and remote
  (`git branch -d`, `git push origin --delete`). A branch list should only show work in
  flight. Nothing is lost on a rejected PR: GitHub keeps its commits reachable from the
  closed PR itself.
- **Language:** all code, comments, docs, and commit messages in English.
- **Style:** `.clang-format` (LLVM base, 4-space, 120 col). CI checks with
  **clang-format 18**; match that version locally (`pip install clang-format==18.*`) or
  formatting that looks clean can still fail the check.
- Comments explain *why* / invariants, not *what*.
- **No milestone codenames** (M0…Mn) in docs — describe capabilities thematically.

## Releases

- **Release APKs come from CI, never from a local build.** The `release-apk` workflow runs
  when a release is published: clean checkout of the tag, NDK build, signed with the stable
  key from repository secrets, assets attached to the release. Do not hand-upload an APK.
- **Every released feature bumps the app version**: `versionCode` + `versionName` in the
  Android app's Gradle config, in the same PR as the change being released, matching the tag.
- **Release title is the bare version** — `vX.Y.Z`, no description after it.
- **Validate on device before releasing.** The host gates prove correctness, not speed or
  app behaviour; a release that changes the engine or the app gets a run on a real phone
  first.

## Where numbers come from

Benchmark figures in the docs are measured (12 GB / UFS 4.x test phone,
Qwen3-30B-A3B-Q4_K_M and friends). Don't invent or round them silently; if you re-measure,
update `docs/benchmark-method.md` and the README table together. Phrase the test device
generically in anything public.
