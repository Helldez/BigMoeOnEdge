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

CI also runs the gates under AddressSanitizer and UndefinedBehaviorSanitizer. To reproduce a
report locally, build into a separate directory and run the same tests:

```bash
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS="$SAN" -DCMAKE_CXX_FLAGS="$SAN" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan
cd build-asan && ASAN_OPTIONS=halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=$PWD/../tests/sanitizers/ubsan.supp \
  ctest --output-on-failure
```

`tests/sanitizers/ubsan.supp` and `lsan.supp` list only findings inside `third_party/llama.cpp`,
which this project does not patch; a report in our own code is a bug to fix, never a line to add
there. Leak checking is Linux-only (macOS's ASan has none); on Linux add
`LSAN_OPTIONS=suppressions=$PWD/../tests/sanitizers/lsan.supp`.

The web UI: `cd ui && npm ci && npm test && npm run check && npm run build`.

## Good first contributions

- **A benchmark on hardware we do not have.** No code needed: `scripts/bench-report.sh MODEL.gguf`
  runs the fixed protocol and prints the tables; open a
  [benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml).
  See `docs/community-benchmarks.md` for the hardware list and what a row needs.
- A new MoE architecture recipe + its gate (`docs/adding-a-model.md`).
- Read coalescing / expert-contiguous layout experiments (see `docs/roadmap.md`).

## Reporting problems

Bugs: the [bug report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=bug-report.yml)
form. Security problems: privately, as [SECURITY.md](SECURITY.md) says, never in a public issue.

## PRs

Keep them focused. Describe what you measured if the change is performance-related, and on
what hardware. Update the relevant doc in the same PR as the code.
