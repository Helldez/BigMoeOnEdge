# Plan records

One file per run of `bmoe-planlab record` ([../../plan-records.md](../../plan-records.md)): what
`--auto` measured on a machine for a model, what the cost model predicted in parts, and what the
run then measured. Unlike the dated sessions beside this directory, this one is a single growing
table: every record is one row of the data the planner's predictions are checked and improved
against.

```bash
bmoe-planlab errors docs/bench-data/plan-records        # predicted against measured, every record
bmoe-planlab table  docs/bench-data/plan-records -o all.csv
```

The `plan_records` test reads every record here on every build: each must still parse and replay.
It does not require them to make the plan they recorded (the few that gate rule changes live in
`tests/plan-records`), so adding a record never blocks a change.

## Adding one

- Name it `<machine class>-<model>-<quant>[-<what differs>].planrec`, with its `.csv` beside it
  when there was a run: `phone-12gb-qwen3.6-35b-q4km.planrec`, `rtx-24gb-glm-4.5-air-q4km-ctx16k.planrec`.
  The machine class, never the product: this repository names no device model.
- Make it on a first load, with as few flags as the run needs.
- Read it before committing. A record carries no weights, no prompt and no local path, and the tool
  leaves out the machine's model, host name and user. What it does carry is whatever the backends
  and the system report about themselves: check `hw.devices.*.name`, `.description` and
  `.physical_id`, `env.cpu`, `env.kernel` and `hw.label`, and `--note`. A GPU or processor model is
  what a benchmark row carries anyway; a serial, an address or a vendor build string that names one
  device is not, and is edited out (the rules read none of those keys).
- Records made by the project's own test runs come here by default. A record from someone else's
  machine comes through a benchmark report, where they chose to attach it.

These files are data about the build they were made on. A probe that later changes what a fact
means makes the older records' facts stale; they are dropped then, not corrected.
