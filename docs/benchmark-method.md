# Benchmark method

## On-device (the headline number)

Hardware for the reference numbers: OnePlus 15R, Snapdragon-class SoC, 11.3 GB RAM,
UFS 4.x storage. Model: Qwen3-30B-A3B-Q4_K_M (18.5 GB), ~1.7× device RAM.

```bash
# push the model
adb push Qwen3-30B-A3B-Q4_K_M.gguf /sdcard/Android/data/io.bigmoeonedge.example/files/

# or, running the CLI directly over adb shell (binary staged by build-android.ps1):
bmoe-cli -m Qwen3-30B-A3B-Q4_K_M.gguf --moe-stream \
  --cache-mb 4000 --io-threads 4 -t 4 -n 48 --progress \
  --csv /sdcard/.../sweep-4000.csv -p "Explain mixture-of-experts routing."
```

Report `s/token` and the `compute + flash I/O` split from the `moe-stream:` line, and the
`moe-cache:` hit rate.

### Sweep

Vary one axis at a time:

| Axis | Values | Expectation |
|------|--------|-------------|
| cache-mb | 0, 2000, 4000, 6000 | monotone improvement; 0-or-≥2000 only |
| io-threads | 1, 4 | 4 ≈ 3× the serial read bandwidth |
| threads (-t) | 2, 4, 8 | U-shape, 4 optimal, 8 regresses |

### Caveats

- **Thermal.** Sustained decode throttles. Warm up, then measure a steady window; discard
  the first few tokens. Ignore `cpu-hw-trip-*` sensors — those are static 95 °C trip
  points, not live temperatures.
- Expect **0.55–0.73 s/token** across the good part of the sweep.

## Host (correctness + a sanity number)

- **Gates** (mandatory before release): `cd build && ctest --output-on-failure`. These
  prove streamed == resident on the tiny synthetic model.
- **Real small MoE** (release checklist): run Qwen1.5-MoE-A2.7B-Q4_K_M streamed vs
  resident on the dev host and confirm identical output. It is too large for CI.

The host streamer works on Linux/macOS/Windows, but throughput targets apply to
Android/Linux on UFS storage; Windows `VirtualAlloc` commit-per-slice is heavier (see
[limitations.md](limitations.md)).
