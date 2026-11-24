## What and why

<!-- One coherent change. What it does, and the reason. -->

## Decision tree

<!-- Which branches of docs/decision-tree.md this change touches. Paste the output of
     `python3 scripts/decision-tree.py touched origin/main --mermaid`; for a new, changed or removed
     branch, say what the node now says. Code reported "outside every node" either decides nothing
     (say so) or is a branch the tree is missing (add it). -->

## Checklist

- [ ] `ctest --output-on-failure` passes (and the gates, if the streamer, the seam or the submodule moved)
- [ ] `CHANGELOG.md` and every doc this change invalidates are updated in this PR
- [ ] `python3 scripts/decision-tree.py check` passes: every branch this change adds, removes or alters has its node and `// dt:` marker updated
- [ ] No new hardcoded tunable: a new setting is a `RunConfig` field plus a row in `core/src/params.cpp`
- [ ] Benchmark numbers, if any, are measured, and the hardware is named as a class
