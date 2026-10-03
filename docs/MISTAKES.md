# Mistakes — what failed, why, and the checkable rule that prevents it

Format per project-lifecycle: what failed + root cause (not the symptom) + the cost +
a **checkable** prevention rule ("assert X before Y", never "be careful").

---

## 2026-09-20 — `pkill -f <pattern>` kills the tool call that runs it (swallowed command tails)

**What failed:** three compound cleanup commands (`pkill -f cellc.sh; rm -rf …; echo …`)
ended with their tails never executing — no output, no error, stale state left behind
(`/tmp/bench-q30` surviving an "rm -rf", a tmux relaunch silently not happening). The
misleading part: `pkill` itself always "worked" (the target processes did die), so each
command *looked* successful up to the point it vanished.

**Root cause:** `pkill -f` matches the pattern against **every** process's full command
line — including the bash process running the compound command itself, whose cmdline
contains the literal pattern string (`pkill -f cellc.sh; …`). pkill kills the caller;
everything after the pkill in the same command never runs.

**Cost:** ~30 min of confused re-polling across the Qwen3-30B c-suite session; one false
"relaunched" belief; stale bench state briefly misread as "something recreated the dir."

**Prevention rule (checkable):** when killing by pattern inside a compound command, use
a bracketed pattern that cannot match its own command line — `pkill -f "[c]ellc.sh"` —
or split the kill into its own single command and assert afterwards
(`pgrep -f "[c]ellc.sh" || echo dead; ls <path-i-expect-gone> 2>&1`). Never chain
`pkill -f X` with any follow-up statement in the same shell command.

---

## 2026-09-21 — `setsid nohup … &` engines survive their tmux session's teardown ("DOWN" while UP)

**What failed:** session-15's wrap-up recorded the LFM2.5 daily driver as DOWN after
killing the serve tmux session — but the engine had been launched `setsid nohup … &`,
so it detached from the session and survived. The stale engine (started Sun 15:04)
squatted RAM through the night and into session-19's RAM-constrained quality gate;
the resume asserted a false environment state that the process table contradicted.
Found only by accident, reading a `ps` output for an unrelated question.

**Root cause:** `setsid nohup cmd &` re-parents the process to init — the tmux
session is no longer its ancestor, so `tmux kill-session` cannot reach it. The
belt-and-suspenders launch pattern (session wrapper + setsid detach) creates a
process no teardown step in the workflow owns or tracks.

**Cost:** ~8 h of unknown contention on a 4-core host; ~2 GB anon + page cache
missing during a gate that needs it; one false "DOWN" resume entry trusted until
session 19.

**Prevention rule (checkable):** any engine started detached gets its PID recorded
in PROGRESS before wrap-up (`pgrep -f "[b]moe-serve.py"`), and every "server
UP/DOWN" claim in the resume is backed by a process assertion, not by teardown
intent: `pgrep -f "[b]moe-serve.py" && echo UP || echo DOWN`. Never claim teardown
without the assert — `tmux kill-session` does not kill setsid-reparented children;
kill the recorded PID (`kill <pid>`), then assert the process is gone.

## 2026-09-21 — bench driver `rc=1` was post-run cleanup noise, not a cell failure

**What failed:** the Cyber-Tiel gate driver exited rc=1 after the HumanEval λ=0 cell
(first chain — the λ=0.15 cell never started) and again after the relaunched λ=0.15
cell completed. The rc was read as "the run died / the cell may never have run": a
relaunch was ordered, a failure investigation started, and the record briefly
described a completed gate as dead. The collected artifacts proved every cell had
finished with full, valid output — the nonzero exit came from the driver's post-run
cleanup (the only hypothesis consistent with complete cell output + rc=1 + no OOM
trace), which runs after the measured work and whose failure says nothing about it.

**Root cause:** exit code was treated as the verdict before reading the artifacts.
A driver's rc summarizes its *last command*, not the experiment; the measured cells
write their own evidence independently of the driver's exit path.

**Cost:** one unnecessary relaunch decision, a misrecorded incident ("gate died,
cause unknown"), and hours of needless doubt about valid evidence.

**Prevention rule (checkable):** a bench "failure" is asserted from the captured
per-cell artifacts, never from exit code alone — before declaring a run dead, list
the output dir and parse the per-cell JSON/JSONL (count rows), declaring failure
only when a cell's artifacts are absent or truncated. The driver log captured to a
file is the durable error surface (per-session rule); its tail is read before any
relaunch. Exit codes triage *how* a run failed only after the artifacts prove
*whether* it did.

## 2026-09-21 — the publish flow's diff-only capture misses untracked files (recipes.md nearly shipped dangling links)

**What failed:** publishing the recipes doc via the patch flow, the new file
`docs/recipes.md` was written into a personal worktree but the patch was generated
with `git diff` — which is **untracked-blind**. The published commit contained only
the two modified files (TOC + README entry, both linking to `recipes.md`); the file
itself never made it. Caught within one step by asserting the landed file set
(`git show fork/main:docs/recipes.md` → missing), fixed by committing the file in
the worktree and rebasing onto the real tip.

**Root cause:** `git diff` shows tracked modifications only. A new file with no
`git add` is invisible to it, and a successful push was being read as "everything
published". Contributing factor: the publish script applies patches in its own
throwaway worktree, so the agent's personal worktree stayed at the pre-publish ref
and its untracked file was never part of any commit.

**Cost:** one rejected push (non-fast-forward from the sibling commit), a rebase
detour, and a two-commit published chain where one was meant.

**Prevention rule (checkable):** a publish is only done when every expected file is
*inside* the published diff — after pushing, assert the landed set explicitly:
`git show fork/main:<new-file>` must succeed for each new file, and
`git diff --stat <pre-publish-ref> fork/main` must list every file the change
touched. Either assert failing ⇒ the publish is not done, whatever the push said.
New files get `git add`ed in the worktree *before* the patch is captured, or the
capture command is `git add -N <new-file>` first (intent-to-add makes `git diff`
see them).
