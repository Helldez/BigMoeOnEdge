# ARCHIVED — kept for the provenance of docs/bench-data/2026-07-13, not for reuse.
#
# This drove the 2026-07-13 matrix: auto-sized cache (--cache-mb auto, ± ceiling) and the reworked
# speculative gating, against a fixed-cache overlap reference. Both of those features are gone from
# the engine as shipped — spec-gating was removed, and the adaptive governor that made `auto` a live
# control loop was retired (`auto` now sizes once at init). Its `--spec-gate` cells will not run.
#
# It stays because the tables it produced are published; deleting it would leave those numbers with
# no visible derivation. For a live matrix use bench-matrix.ps1, which shares bench-lib.ps1.
#
# Mirrors bench-matrix.ps1's outputs (same bench-run.sh, same .csv/.metrics/.log, same tag naming so
# bench-analyze.py picks them up). Qwen used a 4000 MiB reference/ceiling, Gemma a 2000 MiB one
# (4000 OOMs on this device).
param(
  [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) ".bench"),
  [int]$NPred = 256,
  [int]$CooldownSec = 45
)
$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$DEV = "/data/local/tmp"
$QWEN  = "/sdcard/Download/Qwen3-30B-A3B-Q4_K_M.gguf"
$GEMMA = "/sdcard/Download/google_gemma-4-26B-A4B-it-Q4_K_M.gguf"

function Run-Cfg($tag, $model, $flags) {
  Write-Host "==================== $tag ===================="
  Start-Sleep -Seconds $CooldownSec
  $t0 = Get-Date
  $log = & adb shell "sh $DEV/bench-run.sh $NPred $model $DEV/$tag.csv $DEV/$tag.metrics $flags" 2>&1
  $dt = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
  $log | Where-Object { $_ -match "generation:|prefill:|moe-stream:|moe-cache:|moe-overlap:|moe-prefetch:|peak_rss|mem_avail_floor|batt_temp_max|cpu_temp_max" } | ForEach-Object { Write-Host "  $_" }
  Write-Host "  wall(incl load)=${dt}s"
  $log | Out-File -FilePath "$OutDir\$tag.log" -Encoding utf8
  & adb pull "$DEV/$tag.csv" "$OutDir\$tag.csv" 2>&1 | Out-Null
  & adb pull "$DEV/$tag.metrics" "$OutDir\$tag.metrics" 2>&1 | Out-Null
  if (Test-Path "$OutDir\$tag.csv") { Write-Host "  -> $tag.csv + .metrics + .log saved" } else { Write-Host "  !! no CSV for $tag" }
}

# Per-model config: fixed reference, adaptive (uncapped), adaptive (capped at the reference).
$matrix = @(
  @{ n = "qwen";  m = $QWEN;  cap = 4000 },
  @{ n = "gemma"; m = $GEMMA; cap = 2000 }
)
foreach ($e in $matrix) {
  $n = $e.n; $m = $e.m; $cap = $e.cap
  Run-Cfg "${n}_base_ov"    $m "--moe-stream --cache-mb $cap --io-threads 4 --overlap"
  Run-Cfg "${n}_auto_ov"    $m "--moe-stream --cache-mb auto --io-threads 4 --overlap"
  Run-Cfg "${n}_autocap_ov" $m "--moe-stream --cache-mb auto --cache-ceil-mb $cap --io-threads 4 --overlap"
}
Write-Host "ALL DONE"
