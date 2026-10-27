---
name: review-change
title: Review a change
description: Review uncommitted or recent changes in a repository for bugs, missing tests and unclear code. Use when asked to review, check or look over a change, a diff, a commit or a pull request.
---

# Reviewing a change

Goal: find what is wrong with the change, in order of how much it matters, with evidence. Praise
and restating the diff are not the job.

1. **Get the change.** `git status` and `git diff` (add `--staged` for staged work, or
   `git show <commit>` / `git diff <base>...HEAD` for a commit or a branch). If the person named
   files instead, read those.
2. **Understand the intent.** Read the commit message or ask what the change is for if it is not
   clear. A change can only be wrong relative to what it is meant to do.
3. **Read around every hunk.** Open the changed files (`read_file` with a line range) and the
   callers and callees of each changed function (`search_files` for its name). Most real bugs sit
   where the change meets code it did not touch.
4. **Check, in this order:**
   - Correctness: wrong conditions, off-by-one, null or empty inputs, error paths that are
     swallowed, resources not released, concurrency (shared state without a lock, a lock held
     across a slow call).
   - Contracts: a changed signature, return value or file format whose other users were not
     updated.
   - Security: input from outside used in a path, a shell command, a query or HTML without
     validation or escaping; secrets in code or logs.
   - Tests: is the new behaviour covered? Would the tests fail if the change were reverted?
   - Clarity: names that mislead, comments that no longer match the code, dead code left behind.
5. **Verify before you claim.** If a test suite exists and running it is allowed, run the tests
   that cover the change. Do not report a bug you have not traced through the code.

## Answer

A list of findings, most important first. Each one: the file and line, what is wrong, why it
matters (a concrete input or sequence that breaks it), and a suggested fix. End with one line on
whether the change is ready. If you found nothing, say so plainly and say what you checked.
Do not change files unless the person asks you to fix what you found.
