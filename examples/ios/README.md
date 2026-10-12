# BigMoeOnEdge — iOS harness

`bmoe-cli` inside an app, so the engine and its hardware planner can be run on an iPhone or iPad.
One text field for the command line, one button, the output. It is a measuring instrument, not a
chat app.

An app on iOS may not start a process, so the Android example's pattern (run the CLI as a binary
and speak to it over pipes) does not exist here. The harness compiles `cli/main.cpp` into the app
with its entry point renamed and calls it on a thread: every flag, the plan's rationale and the
metrics CSV are the CLI's own.

**Status: builds and runs in the simulator (CI compiles it there); not yet run on a device.** A
streamed generation in the simulator gives the host's text on the gates' tiny model, which says the
app works and nothing about a phone. What the planner reads on
real hardware (the process's memory share, whether memory can be locked, whether a model larger
than memory can be mapped at all) is unmeasured.

## Build

Needs Xcode (the command line tools alone carry no iOS SDK) and CMake.

```bash
scripts/build-ios.sh            # the simulator, unsigned
BMOE_IOS_TEAM=<team id> BMOE_IOS_BUNDLE_ID=<your.own.id> scripts/build-ios.sh device
```

A device build is signed by your own Apple development team, and the bundle identifier has to be
unique to it. `BMOE_IOS_MEMORY_ENTITLEMENTS=ON` asks for the increased memory limit and the
extended address space; both must be granted to the team, or the build does not sign.

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
kernel with the Mac's memory, storage and GPU, while the build still describes the machine as one
that ends a process for holding too much. Do not read a plan or a throughput figure from it.

## Run on a device

Install the signed build from Xcode or with `xcrun devicectl device install app`. Copy a model into
the app's folder with the Files app, or with the Finder while the device is attached. Type the
command line in the app, or script it:

```bash
xcrun devicectl device process launch --console --device <id> <your.own.id> --bmoe --auto --plan -m model.gguf
```

## Limits

- `--session` reads requests from a standard input an app does not have.
- A command line the CLI rejects by exiting (a flag with its value missing) ends the app.
- Devices are probed in-process: there is no second process to isolate a failing driver in.
- Which regions of the model's mapping are resident is not reported here.
