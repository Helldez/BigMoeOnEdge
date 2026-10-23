# The Hexagon NPU on Windows on Snapdragon

The Windows ARM64 release comes in two forms:

- **`bmoe-desktop-<version>-windows-arm64.zip`**: CPU, Vulkan, and OpenCL for the Adreno GPU. It runs as
  soon as it is unzipped. Start here.
- **`bmoe-desktop-<version>-windows-arm64-npu.zip`**: the same, plus the Hexagon NPU. Windows runs the
  NPU part only after the one-time setup on this page, and part of that setup lowers a protection of
  the machine. Read the whole page before starting.

What the NPU buys: the engine uses it for the **prompt** (prefill), as on Android
([npu-prefill.md](npu-prefill.md)). The answer is generated on the CPU either way. So the NPU
shortens the wait before the first token on long prompts; it does not change the tokens per second
of the answer. The planner gives the prompt to the NPU only if it measures faster than the other
devices and its result is identical to the CPU's; otherwise the NPU stays idle and nothing is lost.

## Why there is a setup at all

The NPU runs code of its own: one DSP library per NPU generation (`libggml-htp-v73.so` ...
`libggml-htp-v85.so`), from llama.cpp's Hexagon backend. Windows' NPU driver loads that code only if
it is listed in a catalog (`libggml-htp.cat`) signed by a certificate the machine trusts. Code that
every Windows machine trusts out of the box needs Microsoft's driver signing, which this project
does not have. So the archive is signed with a project certificate, and you tell Windows to accept
it. llama.cpp documents the same procedure for its own builds
([docs/backend/snapdragon/windows.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/backend/snapdragon/windows.md)).

How the archive is signed: the release workflow (`.github/workflows/release-host.yml`) makes a new
certificate in the build job, signs the catalog with it, and deletes the private key before the job
ends. No copy of the key exists, so nothing else can ever be signed with this certificate. Only the
public part ships, as `bigmoeonedge-npu.cer`. Each release has its own.

## What the setup changes

| Step | What it changes | Undo |
|---|---|---|
| 1. NPU driver | Installs Qualcomm's driver for the NPU | Device Manager, as for any driver |
| 2. Test-signing | Windows accepts code signed by test certificates you trust. Shows "Test Mode" on the desktop. Usually needs Secure Boot off | `bcdedit /set testsigning off`, restart, Secure Boot back on |
| 3. Trust the certificate | Adds `bigmoeonedge-npu.cer` to the machine's Trusted Root and Trusted Publishers stores | Remove it from both stores (below) |

Test-signing is the part to weigh. While it is on, Windows accepts any code signed by any test
certificate in your trusted stores, not only this one. Trust no other test certificate while it is on,
and turn it off when you stop using the NPU. Some anti-cheat software and some corporate policies
refuse to run with test-signing on. Do not do this on a machine your employer manages.

## Setup

Unzip `bmoe-desktop-<version>-windows-arm64-npu.zip` somewhere permanent, for example
`C:\BigMoeOnEdge`. Every command below runs in **PowerShell as administrator** (Start menu, type
PowerShell, right-click, Run as administrator), from that folder:

```powershell
cd C:\BigMoeOnEdge\bmoe-desktop-<version>-windows-arm64-npu
```

### 1. The NPU driver

Open Device Manager and look for **Neural processors**. If it lists a Qualcomm Hexagon NPU with no
warning sign, go to step 2. Otherwise install Qualcomm's NPU driver from
<https://softwarecenter.qualcomm.com/catalog/item/Qualcomm_HND> (or the laptop maker's support page)
and restart. The same check from PowerShell:

```powershell
Get-PnpDevice -Class ComputeAccelerator -PresentOnly
```

### 2. Test-signing

Check whether it is already on:

```powershell
bcdedit /enum '{current}' | Select-String testsigning
```

If it does not say `Yes`, turn it on and restart:

```powershell
bcdedit /set testsigning on
```

If bcdedit answers that the value is protected by Secure Boot policy, Secure Boot has to be turned
off first, in the laptop's firmware (UEFI) settings. How to get there depends on the maker; usually
Settings, System, Recovery, Advanced startup, then UEFI Firmware Settings. Then run the command
again and restart.

### 3. Trust the certificate

Look at what you are about to trust:

```powershell
Get-PfxCertificate .\bigmoeonedge-npu.cer | Format-List Subject, Thumbprint, NotAfter
```

The subject is `CN=BigMoeOnEdge NPU <version>`, the version of the archive. Then add it to the two
stores:

```powershell
Import-Certificate .\bigmoeonedge-npu.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate .\bigmoeonedge-npu.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

And check that the catalog now verifies:

```powershell
Get-AuthenticodeSignature .\libggml-htp.cat | Format-List Status, StatusMessage
```

`Status` must be `Valid`.

## Run

Start `bmoe-server.exe` from the same folder, as with the plain archive. The NPU driver looks for
the DSP libraries beside the program and, on some driver versions, on `ADSP_LIBRARY_PATH`; if the Plan
page says the NPU could not open a session, start it from a command prompt in this folder with:

```bat
set ADSP_LIBRARY_PATH=%CD%
bmoe-server.exe --open
```

Load a model and open the **Plan** page. The prefill line says which device took the prompt and why;
a device the planner did not use is listed with its reason. `bmoe-cli --auto --plan-explain` prints
the same from the command line.

A measurement from a Snapdragon laptop, with or without the NPU, is the most useful thing to send
back: a [benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml)
with the Plan page's prefill line.

## Undo

In PowerShell as administrator. Remove the certificate from both stores (the thumbprint is the one
step 3 printed):

```powershell
$t = (Get-PfxCertificate .\bigmoeonedge-npu.cer).Thumbprint
Remove-Item "Cert:\LocalMachine\Root\$t", "Cert:\LocalMachine\TrustedPublisher\$t"
```

Turn test-signing off, restart, and turn Secure Boot back on in the firmware settings if you turned
it off:

```powershell
bcdedit /set testsigning off
```

The plain archive keeps working afterwards, and the NPU archive does too, on the CPU and GPU.

## When it does not work

| What you see | Why, and what to do |
|---|---|
| The Plan page lists no NPU device | The Hexagon module did not register: the NPU driver is missing (step 1), or the folder is not the `-npu` archive |
| The NPU is listed but could not open a session | Windows refused the DSP library: test-signing is off (step 2), the certificate is not trusted (step 3), or `libggml-htp.cat` is not beside the `.so` files. Also try the `ADSP_LIBRARY_PATH` start above |
| The NPU is listed and measured, but the prompt runs elsewhere | Not an error: the planner found another device faster, or the NPU's result was not identical to the CPU's. The Plan page gives the reason |
| bcdedit: "protected by Secure Boot policy" | Turn Secure Boot off first (step 2) |

This setup has not yet been run on a Snapdragon laptop by the project; the first runs will correct
this page. Reports are welcome in the issues.
