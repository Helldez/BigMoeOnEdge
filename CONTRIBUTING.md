# Contributing

Thanks for your interest. BigMoeOnEdge stays deliberately small and disciplined.

## Ground rules

- **Never patch `third_party/llama.cpp` in-tree.** Work through its public API. Keeping
  the submodule stock is the point of the project (see `docs/seam.md`).
- **The byte-identity gates must pass.** Streaming must stay lossless. Run
  `ctest --output-on-failure` before opening a PR; if you change the streamer or the seam,
  say so in the PR and note the gate result.
- **No hardcoding.** There is no table of architectures: which tensors are the routed experts is
  read from the file (`docs/which-models-stream.md`). A new expert layout gets a byte-identity gate.
- English everywhere. Conventional Commits. Match `.clang-format`.

## Getting set up

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
scripts/build-host.sh
cd build && ctest --output-on-failure
```

Our own code builds with `-Wall -Wextra` (`/W4` on MSVC); llama.cpp keeps its own flags. A new
warning is fixed, not silenced: configure with `-DBMOE_WARNINGS_AS_ERRORS=ON` to make it an error,
as CI's macOS job does.

CI also runs the gates under AddressSanitizer and UndefinedBehaviorSanitizer, and the threaded tests
(the engine host, the server's turn queue, the gates) under ThreadSanitizer. `BMOE_SANITIZE` builds
everything, llama.cpp included, with the sanitizers it lists (`address`, `undefined`, `thread`,
comma-separated; `thread` alone). To reproduce a report locally, build into a separate directory and
run the same tests:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBMOE_SANITIZE=address,undefined
cmake --build build-asan
cd build-asan && ASAN_OPTIONS=halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=$PWD/../tests/sanitizers/ubsan.supp \
  ctest --output-on-failure
```

ThreadSanitizer: `-DBMOE_SANITIZE=thread -DGGML_OPENMP=OFF` into its own build directory, then
`ctest -R '^(engine_host|server_logic|moe_gates_.*)$'` with `TSAN_OPTIONS=halt_on_error=1`. On a
Linux kernel with wide address-space randomisation, `sudo sysctl vm.mmap_rnd_bits=28` first.

`tests/sanitizers/ubsan.supp` and `lsan.supp` list only findings inside `third_party/llama.cpp`,
which this project does not patch; a report in our own code is a bug to fix, never a line to add
there. Leak checking is Linux-only (macOS's ASan has none); on Linux add
`LSAN_OPTIONS=suppressions=$PWD/../tests/sanitizers/lsan.supp`.

The web UI: `cd ui && npm ci && npm test && npm run check && npm run build`.

## Good first contributions

- **A benchmark on hardware we do not have.** No code needed: `scripts/bench-report.sh MODEL.gguf`
  runs the model with `--auto` and prints the tables and the plan; open a
  [benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml).
  See `docs/community-benchmarks.md` for the hardware list and what a row needs.
- A byte-identity gate for an expert layout the gates do not cover yet (`docs/which-models-stream.md`).
- Read coalescing / expert-contiguous layout experiments (see `docs/roadmap.md`).

## Reporting problems

Bugs: the [bug report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=bug-report.yml)
form. Security problems: privately, as [SECURITY.md](SECURITY.md) says, never in a public issue.

## PRs

Keep them focused. Describe what you measured if the change is performance-related, and on
what hardware. Update the relevant doc in the same PR as the code.
