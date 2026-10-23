# jniLibs/arm64-v8a

The engine binaries are **not** committed. Build and stage them with:

```powershell
pwsh scripts/build-android.ps1
```

That cross-compiles `bmoe-server` and `bmoe-cli` and copies them here as `libbmoe-server.so`
and `libbmoe-cli.so`, alongside the `libllama.so` / `libggml*.so` they link. Android only
extracts and lets an app execute files named `lib*.so` from its `nativeLibraryDir`, which is
why the executables ship under those names. The app starts `libbmoe-server.so` via
`ProcessBuilder` (no JNI) and talks to it over HTTP on loopback; it does not start
`libbmoe-cli.so`.

Expected files after staging (the CPU backend comes as one module per instruction-set tier):

```
libbmoe-server.so
libbmoe-cli.so
libllama.so
libggml.so
libggml-base.so
libggml-cpu-*.so
libc++_shared.so
```
