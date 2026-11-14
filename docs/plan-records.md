# Plan records: a machine as a file

The hardest part of this project is not writing a rule, it is trying it on hardware nobody has at
hand. A plan record is the part of that problem that can be solved on a laptop.

The planner is a pure function: it reads a `HardwareProfile`, a `ModelProfile` and the capacity
fitter's `Placement`, and nothing else ([hardware-planning.md](hardware-planning.md)). So to the
rules, a phone or a rented card *is* the facts its probes reported. A plan record is those facts as
text. Made once on a real machine, it lets any build, on any machine, make that machine's plan
again, with no model file, no device and no drive.

## What it answers, and what it does not

| Question | Answered by a record? |
| --- | --- |
| Does this rule change move the plan on a machine I do not have? | Yes: the `plan_records` test replays every record on every build. |
| What would the rules do there with a bigger cache, another width, a pinned thread count? | Yes: replay with the knob typed. |
| What would the same model get with less memory or a slower drive? | Yes, as far as the changed fact goes: replay with `--set`. |
| How far is the cost model's prediction from what the machine measured? | Yes, when the record was made with a run. |
| What would the plan be on a machine nobody recorded (two 16 GB cards, an 8 GB card with 32 GB of RAM)? | Yes, from measured parts: `bmoe-planlab simulate` ([hardware-simulation.md](hardware-simulation.md)). |
| Is the plan actually fast there? Does the driver crash? Does the kernel kill the process? | **No.** That is the machine's to answer. |

This is a developer's instrument, not a feature of the product: it lives in `tools/planlab`, is
built only with `-DBMOE_BUILD_TOOLS=ON`, and no release carries it. `bmoe-cli`, the server and the
apps know nothing about it.

A record is not an emulator and nothing here pretends to be one: there is no emulator whose flash,
memory bandwidth, thermal behaviour and drivers match a real device. What a record removes is every
trip to a machine that was only made to learn what the *planner* would decide.

## Making one

On the machine, with the model:

```bash
bmoe-planlab record -m MODEL -o my-machine.planrec --note "12 GB phone, UFS 4"
```

This probes and plans exactly as `bmoe-cli --plan` does, and exits without loading the model. With
`--run` it then runs the plan, and the record also carries what the run then measured (seconds per token, compute and read time, cache hit
rate), which sets the prediction beside the truth:

```bash
bmoe-planlab record -m MODEL -o my-machine.planrec --run "..." -n 200
```

`--routing-dir DIR` reads the routing and the measured knobs `bmoe-cli` kept there for the model, so
the record is of the plan a second load gets rather than a first one. Every other argument is an
engine parameter, spelled as `bmoe-cli` spells it.

Make it on the first load after the machine was left alone. A second load back to back does not
see the same machine: in a Linux container the model file's page cache is charged to the cgroup and
the memory the plan reads as available falls with every load, and the record then describes a
starved machine that does not exist.

Make it with as few flags as the run needs: every parameter typed is recorded as pinned, and a
record of a hand-tuned run only says what the rules do around those pins.

The record is replayed once, in memory, before it is written. If it does not make the plan it was
just taken from, the tool says so: a fact the rules read is missing from the record, and the machine
that can show which one is the one in front of you.

### What is in the file

One `key=value` per line:

- `hw.*`: the hardware profile: memory figures, core classes, the thread sweep, every compute
  device with its probed capabilities and rates, the storage rate curve.
- `model.*`: the model profile: shapes, byte counts per weight group, the hit curve from the model's
  recorded routing and the measured knobs, when the front-end kept them.
- `placement.N.*`: each answer of the capacity fitter, at the graph width it was asked at.
- `config.*` and `request.*`: the parameters that were not at their default, and which were pinned.
- `plan.resolved`: the plan as one line, which a replay is compared with.

and three tables no rule reads, which is why they are free to grow (a figure worth keeping is a key
added where it is measured, and an older record simply lacks the column):

- `env.*`: where it ran, as a person would say it: the operating system and its version, the
  kernel, the architecture, the processor's name, how much swap there is and of what kind (zram, a
  disk, a page file, files made on demand), the swappiness, a container's memory limit, the model
  file's name and content fingerprint, the day. The rules stay free of platform names because they
  read the probed facts in `hw.*`; a table of runs is grouped and explained by these.
- `predicted.*`: what the cost model predicted, in parts: the total, each weight group's lane, bytes
  held and seconds per token, the cache and the hit rate it credited, every row of the memory
  ledger. So the error of each part can be read, not only the total's.
- `observed.*`: what the run measured, when there was one:
  - the summary: seconds per token and its parts (compute, expert reads, cache management, stalls,
    flash waits, loop overhead, CPU time), the prefill and the load, megabytes read;
  - `token.*`: the same per decoded token as a distribution (mean, p10, p50, p90, max, and the
    median of the second half, the decode once the cache has settled);
  - the cache as it really was: budget (first, lowest, last), bytes resident, evictions, re-reads,
    experts the system dropped; the hit rate over the decode and with the prompt;
  - `memory.*`: the process through the run: resident memory at its peak, its own memory the system
    compressed or swapped, memory available and swap free at the start and at their lowest, the
    share of the dense weights still resident, major faults;
  - `system.*.before` / `.after`: the system's counters around the run: swap in use, pages swapped
    in and out, compressions, major faults, the page cache, a container's charged memory, pressure.

With `--run` the per-token metrics are also written as `FILE.csv` beside the record, always: the
record keeps their distribution, the CSV keeps every token.

No weights, no prompt, no file contents. Paths are kept as file names only. The device names are
the ones the backends report (a GPU model, a bus id), so read the file before publishing it, as with
any bench evidence, and phrase `--note` generically.

## What the records are for

One record is a test and a what-if. Many records are a table: for each machine and model, the facts
the plan was made from, what it predicted in parts, and what happened. That table is what a better
predictor is built on, whether it stays the cost model with its terms corrected or becomes a fitted
one, and it is why a record keeps more than today's rules read: what a predictor needs is found by
using the table, and a fact not kept on the day of the run cannot be measured afterwards.

```bash
bmoe-planlab errors DIR            # this build's prediction against each recorded run
bmoe-planlab table DIR -o all.csv  # every record a row, every key a column
```

`errors` replays each record that carries a run with the rules of the build at hand and prints the
predicted and measured seconds per token (the settled half of the decode), the hit rate credited
against the one measured, the cache priced against the cache the run had, and the swap and major
faults the run saw. Run it before and after a change to the cost model: the change is good when
the error falls across the machines, not on the one it was tuned on.

### Where they are kept

`record` writes where `-o` says, on the machine it ran on: bring the `.planrec` and its `.csv` back
from a phone or a rented box before it is gone. Then:

- **`docs/bench-data/plan-records/`** is the data set. Every record from the project's own test
  runs goes there, after the read for identifying data its README describes. The `plan_records`
  test checks each still reads and plans; it does not hold them to the plan they recorded.
- **`tests/plan-records/`** is the few that gate rule changes (see The corpus below).

Recording is something the project's own runs do by default. For anyone else it is off: the
community benchmark script makes a record only with `BENCH_PLAN_RECORD=1`
([community-benchmarks.md](community-benchmarks.md)), and the file is theirs to read and attach.

## Replaying one

```bash
bmoe-planlab replay my-machine.planrec
```

prints the plan with its reasons, exactly as `bmoe-cli --plan` would there, and says whether it is still the
plan the record carries (exit code 4 when it is not).

**A knob.** Engine parameters typed after the file are laid over the recorded configuration and
pinned, as they would be on the machine:

```bash
bmoe-planlab replay my-machine.planrec --cache-mb 3000
```

**Several values of one knob.** One row per value: the cache, the credited hit rate, the memory held
against the ceiling, and the predicted speed. The first row is what the plan chose on its own.

```bash
bmoe-planlab replay my-machine.planrec --sweep cache-mb=1500,2500,4000,6000
```

**A fact.** `--set` changes one recorded fact before planning, by its key in the file; it can
be repeated. This is how one record stands in for the machines next to it:

```bash
# the same phone with 3 GiB less to hold
bmoe-planlab replay my-machine.planrec --set hw.residency_budget=4000000000 --set hw.holdable_bytes=0
```

Facts are not independent, and the record does not know how they move together: less memory on a
real machine also changes what can be locked and what the fitter places. Change the facts the
question is about, and read the result as "what the rules do with these facts".

### Caveats a replay states

The capacity fitter runs llama.cpp on the real model and cannot run in a replay. Its answers are
recorded at every width the plan asked for, so a replay as recorded stands on recorded facts alone.
A replay that moves away from them says so:

- a graph width the fitter was never asked at: the recorded placement is moved along the slope it
  measured (`Placement::at_width`), and the layers it placed do not change;
- a context the fitter was never asked at (the context giving way to the expert cache asks it at
  shorter ones): the first placement stands in for it;
- another context length, KV type or device list: the placement was made for the recorded ones.

Such a plan is still the rules' answer to the facts given; it is not what the fitter would have
placed. For those questions, make a second record on the machine with that context or device list.

## The corpus

Records kept in `tests/plan-records/*.planrec` are replayed by the `plan_records` test on every
build. Each must still make the plan it recorded. When a rule change moves one:

- the move was not intended: the change reaches a machine it was not written for;
- the move is intended: replay the record and write it back, in the same change,

  ```bash
  bmoe-planlab replay tests/plan-records/X.planrec -o tests/plan-records/X.planrec
  ```

  and the diff of its `plan.resolved` line is what a reviewer reads.

The test also prints, for every record made with a run, the prediction beside the measurement. It
does not fail on that gap: it is the standing list of where the cost model is wrong.

A record is tied to the facts the probes report, not to an engine version: a key added later is
simply absent from an older record and reads as its default. A probe that changes what an existing
fact *means* invalidates the records made before it; re-make them on the machine or drop them.

## Where the code is

- `tools/planlab/plan_record.h`, `plan_record.cpp`: the record, its text, and `replay_plan`. Pure;
  every field is named once, in the walk the writer and the reader share. A field added to a
  profile gets a line there.
- `tools/planlab/main.cpp`: `bmoe-planlab record`, `replay`, `simulate`, `speed`, `errors`, `table`.
- `tools/planlab/simulate.cpp`, `sim_device.cpp`, `sim_speed.cpp`, `gguf_shell.py`: a machine
  composed from records ([hardware-simulation.md](hardware-simulation.md)).
- `replan()` in `core/src/plan/auto_plan.cpp`: the rules on facts already gathered, the same a
  calibration plans with. A record holds every answer the fitter gave (`AutoPlan::placement` and
  `placement_at_width`), and a replay hands them back through `AutoPlan::refit`, which a live plan
  leaves unset so the fitter itself is asked.

The engine's side of it is that hook; nothing in `core/`, the CLI or the server reads a record.

The `plan_records` test builds with the tests and needs no tool. To build the tool itself:

```bash
scripts/build-host.sh -DBMOE_BUILD_TOOLS=ON     # build/bin/bmoe-planlab
```
