---
name: explain-codebase
title: Explain a codebase
description: Explain how an unfamiliar codebase is organised and how a feature works end to end, with file references. Use when asked what a project does, where something is implemented, how the parts fit together, or how to get started in a repository.
---

# Explaining a codebase

1. **Orient.** List the root (`file_glob_search` with `type` "all" and `max_depth` 1); read the
   README, the build or package files (`package.json`, `CMakeLists.txt`, `pyproject.toml`,
   `Cargo.toml`, `go.mod`), and any architecture or contributing document. Note the languages, the entry points (main functions,
   server start-up, CLI commands) and the test layout.
2. **Map the layers.** List the top-level folders and say in one line what each holds. Find the
   boundaries: what talks to the outside world (HTTP, files, the UI), what holds the logic, what
   is generated or vendored (skip those).
3. **Trace one path end to end** when the question is about a feature: start at the entry point
   (a route, a command, a button) and follow the calls with `grep_search` for each function name
   until the work is done and the result returns. Read the actual code at each step; do not guess
   from names.
4. **Answer at the level asked.** For "what is this", a short overview and the map. For "how does
   X work", the traced path as numbered steps, each with `path/to/file:line` and what happens
   there. For "where do I start", the two or three files to read first and the command to build and
   run the tests.
5. **Be honest about gaps.** If part of the path goes through code you did not read (a library, a
   generated file), say so.

Keep the explanation in plain words; quote code only where a line is the point.
