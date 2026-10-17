## What and why

<!-- One coherent change. What it does, and the reason. -->

## Checklist

- [ ] `ctest --output-on-failure` passes (and the gates, if the streamer, the seam or the submodule moved)
- [ ] `CHANGELOG.md` and every doc this change invalidates are updated in this PR
- [ ] No new hardcoded tunable: a new setting is a `RunConfig` field plus a row in `core/src/params.cpp`
- [ ] Benchmark numbers, if any, are measured, and the hardware is named as a class
