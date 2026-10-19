# BigMoeOnEdge — iOS harness

`bmoe-cli` inside an app, so the engine and its hardware planner can be run on an iPhone or iPad.
One text field for the command line, one button, the output. It is a measuring instrument, not a
chat app.

An app on iOS may not start a process, so the Android example's pattern (run the CLI as a binary
and speak to it over pipes) does not exist here. The harness compiles `cli/main.cpp` into the app
with its entry point renamed and calls it on a thread: every flag, the plan's rationale and the
metrics CSV are the CLI's own.

**Status: builds and runs in the simulator (CI compiles it there), and has run on a device.** A
contributor streamed a 35B-A3B on a 12 GB iPhone signed by a free Personal Team with the memory
limit entitlement alone (#235), and the plan it read there is what #304 corrected: the dense set
was left mapped and refaulted from flash on every token. A streamed generation in the simulator
says the app works and nothing about a phone.

## From a release

Every release attaches `bmoe-ios-harness-sim-<tag>.zip`: the harness built for the simulator,
unsigned, with a README that gives the commands below. It runs on a Mac with Xcode and needs no
Apple team; a device build does, and is yours to make from the same tag.

## Build

Needs Xcode (the command line tools alone carry no iOS SDK) and CMake.

```bash
scripts/build-ios.sh            # the simulator, unsigned
BMOE_IOS_TEAM=<team id> BMOE_IOS_BUNDLE_ID=<your.own.id> scripts/build-ios.sh device
```

A device build is signed by your own Apple development team, and the bundle identifier has to be
unique to it. Entitlements must be granted to the team, or the build does not sign:

- `BMOE_IOS_MEMORY_ENTITLEMENTS=ON` asks for the increased memory limit. A free Personal Team is
  granted it (with a seven-day profile), and it is what a streamed 35B needed on a 12 GB phone.
- `BMOE_IOS_EXTENDED_ADDRESSING=ON` also asks for the extended address space, which a Personal Team
  is refused. Nothing measured so far has needed it.

## Run in the simulator

```bash
xcrun simctl boot <device>                       # one of `xcrun simctl list devices`
xcrun simctl install booted <bmoe-harness.app>   # the path the build script printed
# Copy a model into the app's documents folder:
cp model.gguf "$(xcrun simctl get_app_container booted io.bigmoeonedge.harness data)/Documents/"
# Run a command line and read it in this terminal; everything after --bmoe goes to bmoe-cli:
xcrun simctl launch --console booted io.bigmoeonedge.harness --bmoe --auto --plan -m model.gguf
```

Relative paths resolve against the documents folder, so `--csv run.csv` lands where it can be
copied back.

**The simulator proves that it builds and runs, not what a device would do.** It runs on the Mac's
kernel with the Mac's memory, storage and GPU, and the planner reads that kernel's facts (no
per-process share, a compressor): it plans for the Mac. Do not read a plan or a throughput figure
from it.

## Run on a device

Install the signed build from Xcode or with `xcrun devicectl device install app`. Copy a model into
the app's folder with the Files app, or with the Finder while the device is attached. Type the
command line in the app, or script it:

```bash
xcrun devicectl device process launch --console --device <id> <your.own.id> --bmoe --auto --plan -m model.gguf
```

## The runs a report needs, in one command

`scripts/ios-runs.sh` runs the plan, a run of it, and the same run with the dense set left mapped,
each in a fresh process, and brings the console output and per-token CSV of each back to the Mac
with a `summary.txt` on top (tok/s, the decode split, major faults, dense residency):

```bash
BMOE_IOS_DEVICE=<id> BMOE_IOS_BUNDLE_ID=<your.own.id> scripts/ios-runs.sh device model.gguf
RUNS="plan auto mmap t2 t4" ...   # add the same run at 2 and 4 threads
```

`scripts/ios-runs.sh sim model.gguf` does the same in the booted simulator, which checks the
harness and nothing else.

## Limits

- `--session` reads requests from a standard input an app does not have.
- A command line the CLI rejects by exiting (a flag with its value missing) ends the app.
- Devices are probed in-process: there is no second process to isolate a failing driver in.
- The process's region list is not offered to an app, so the dense residency sensor reads the
  mapping at each tensor's own address instead, which works here as everywhere else.
