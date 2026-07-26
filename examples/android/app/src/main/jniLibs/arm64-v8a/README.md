# jniLibs/arm64-v8a

The engine binaries are **not** committed. Build and stage them with:

```powershell
pwsh scripts/build-android.ps1
```

That cross-compiles `bmoe-cli` and copies it here as `libbmoe-cli.so` alongside the
`libllama.so` / `libggml*.so` it links. Android only extracts and lets an app execute
files named `lib*.so` from its `nativeLibraryDir`, which is why the CLI is shipped
under that name and launched via `ProcessBuilder` (no JNI).

Expected files after staging:

```
libbmoe-cli.so
libllama.so
libggml.so
libggml-base.so
libggml-cpu.so
```

## Optional: the GPU build

`pwsh scripts/build-android.ps1 -OpenCL` (after `pwsh scripts/fetch-opencl-android.ps1`) adds
`libggml-opencl.so` to the set, which is what the app's **Dense weights on GPU** setting needs.

Note what is deliberately *not* staged: `libOpenCL.so`. The GPU build leaves it as an unresolved
`DT_NEEDED` so the device's own driver in `/vendor/lib64` satisfies it — the app already puts that
directory on `LD_LIBRARY_PATH`. Shipping the Khronos loader here instead would take priority over
the vendor driver and then find no ICD to dispatch to: everything would link, nothing would crash,
and no GPU would ever be found.

The trade-off is that a GPU-built CLI **will not start at all** on a device with no OpenCL driver,
where the default build runs fine. See `../../../../../../docs/gpu-offload.md`.
