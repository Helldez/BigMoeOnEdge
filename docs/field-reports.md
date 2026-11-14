# Field reports

Problems people reported from their own machines, written so that a symptom can be matched to its
cause. Each case says how to recognise it, what caused it, which version fixes it, and what to do on
a version that does not. A case stays here after it is fixed: someone on an older version should
still find it.

A new case is added in the same pull request as its fix, or on its own where the fix is not yet
known. Only what was confirmed goes in: the cause found in the code or reproduced on a machine, not
guessed from the report. Machines are described by class (a 32 GB card, a 12 GB card, a desktop
processor with integrated graphics), never by the reporter's name or model codes.

## A graphics card beside integrated graphics: the model runs on the CPU

**Seen on** 0.42.0, the desktop app: a 32 GB card, a desktop processor with integrated graphics,
32 GB of RAM, a 26B MoE model in Q6_K small enough for the card. It decoded at 9.5 tok/s on the CPU,
where the plan expected 44.

**How to recognise it.** On the Plan screen, "Device backends" lists the integrated GPU (here
`Vulkan1`, the processor's Radeon graphics) next to the card, the notes read "Placement declined"
with a size and "Layer placement: none", the experts stream from flash, and "Tokens" says CPU.
It only appears once the card has two backends, for instance after installing the CUDA pack beside
the Vulkan module the app ships with.

**Cause.** With two backends on the card, the plan chooses one per card and hands llama.cpp's
capacity fitter the resulting list, and the integrated GPU is a card of its own in that list. The
fitter put part of the model on it. Its memory is the host's, and a placement that touches the
host's memory is set aside whole, so the card's share went with it
([hardware-planning.md](hardware-planning.md)). llama.cpp's own default list leaves an integrated GPU
out beside a discrete one, which is why the case did not appear without the pack.

**Fixed in** the release after 0.42.0 (#584): the fitter is asked only of the devices with memory of their
own, where there is one (`capacity_devices`, [backends.md](backends.md)).

**On 0.42.0 and earlier**, disable the integrated graphics (on Windows: Device Manager, Display
adapters, the integrated GPU, Disable device), restart the app and load the model again.

## The CUDA pack extracted beside `bmoe-cli`: the card is never used

**Seen on** 0.40.2, Linux, the release archives used by hand: a 12 GB card, 128 GB of RAM, a model
larger than the card. 3 tok/s; installed correctly, the same release on the same class of machine
decodes at 16-17.5 ([bench-data/2026-11-09-rented-12gb-cuda-pack](bench-data/2026-11-09-rented-12gb-cuda-pack/README.md)).

**How to recognise it.** The run lists no CUDA device, and the CUDA libraries (`libggml-cuda.so` or
`ggml-cuda.dll`, the CUDA runtime, cuBLAS) sit in the folder that contains the `bmoe-cli-...`
folder, not inside it.

**Cause.** The main archive extracts into a folder of its own; the CUDA pack has none, so its files
land wherever it is extracted. Extracting both in the same download folder leaves the pack one level
above `bmoe-cli`, which loads modules from its own folder only, and nothing says so.

**Not fixed yet.** The pack should carry the same top-level folder, and the log should say when an
NVIDIA driver is present but no CUDA module loaded.

**Workaround.** Extract the pack inside the `bmoe-cli-...` folder ([platforms.md](platforms.md)). The
desktop app is not affected: it installs the pack itself.
