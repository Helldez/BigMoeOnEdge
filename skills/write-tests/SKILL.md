---
name: write-tests
title: Write tests
description: Write or extend automated tests for a function, module or bug fix, in the project's own test framework and style. Use when asked to add tests, raise coverage, or prove a bug is fixed.
---

# Writing tests

1. **Find how the project tests.** Look for a test folder and configuration (`tests/`, `test/`,
   `__tests__/`, `*_test.go`, `pytest.ini`, `package.json` scripts, `CMakeLists.txt` with
   `add_test`, `Cargo.toml`). Read two or three existing tests and copy their structure, naming,
   helpers and assertion style. Never add a new test framework without asking.
2. **Read the code under test** and list its behaviours: the normal case, each branch, the edges
   (empty, zero, one, maximum, invalid input, missing file, timeouts) and the errors it promises.
   For a bug fix, the first test is one that reproduces the bug.
3. **Write small tests, one behaviour each,** named after the behaviour ("rejects a path outside
   the workspace", not "test3"). Arrange the inputs, act once, assert on the visible result. No
   sleeps for timing; no network or real clock unless the project already does that; temporary
   files in a temporary folder.
4. **Run them.** Find the project's test command (README, CI configuration, package scripts) and
   run only the new or changed tests first, then the whole suite if it is quick. A new test that
   passes before the fix it guards is not testing the fix: check it fails without it when you can.
5. **Do not change the code under test to make a test pass** unless that is the task; if a test
   reveals a bug, report it.

## Answer

Say which tests you added, where, what each one covers, the command that runs them and its
result. Mention behaviours you chose not to test and why.
