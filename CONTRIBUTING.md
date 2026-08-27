# Contributing

Thanks for your interest. BigMoeOnEdge stays deliberately small and disciplined.

## Ground rules

- **Never patch `third_party/llama.cpp` in-tree.** Work through its public API. Keeping
  the submodule stock is the point of the project (see `docs/seam.md`).
- **The byte-identity gates must pass.** Streaming must stay lossless. Run
  `ctest --output-on-failure` before opening a PR; if you change the streamer or the seam,
  say so in the PR and note the gate result.
- **No hardcoding.** New MoE architectures are recipe rows (`docs/adding-a-model.md`).
- English everywhere. Conventional Commits. Match `.clang-format`.

## Getting set up

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
scripts/build-host.sh
cd build && ctest --output-on-failure
```

## Good first contributions

- **A benchmark on hardware we do not have.** No code needed: `scripts/bench-report.sh MODEL.gguf`
  runs the fixed protocol and prints the tables; open a
  [benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml).
  See `docs/community-benchmarks.md` for the hardware list and what a row needs.
- A new MoE architecture recipe + its gate (`docs/adding-a-model.md`).
- Read coalescing / expert-contiguous layout experiments (see `docs/roadmap.md`).

## PRs

Keep them focused. Describe what you measured if the change is performance-related, and on
what hardware. Update the relevant doc in the same PR as the code.
