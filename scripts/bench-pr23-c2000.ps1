# Max-throughput follow-up for Qwen: push the cache lever (5000 MiB — fewer misses, closer to the
# ~7 tok/s compute ceiling) and test the one new feature with a real shot here — shallow prefetch
# (depth 1, low speculative volume, unlike spec-gate). Baseline vs +prefetch 1, at lane 4 overlap.
param(
  [string]$OutDir = "C:\Users\raffa\Documents\BigMoeOnEdge\.bench-pr23",
  [int]$NPred = 256,
  [int]$CooldownSec = 45
)
$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$DEV = "/data/local/tmp"
$QWEN = "/sdcard/Download/Qwen3-30B-A3B-Q4_K_M.gguf"

function Run-Cfg($tag, $model, $flags) {
  Write-Host "==================== $tag ===================="
  Start-Sleep -Seconds $CooldownSec
  $t0 = Get-Date
  $log = & adb shell "sh $DEV/bench-run.sh $NPred $model $DEV/$tag.csv $DEV/$tag.metrics $flags" 2>&1
  $dt = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
  $log | Where-Object { $_ -match "generation:|prefill:|moe-stream:|moe-cache:|moe-overlap:|moe-prefetch:|moe-spec-gate:|peak_rss|mem_avail_floor|batt_temp_max|cpu_temp_max" } | ForEach-Object { Write-Host "  $_" }
  Write-Host "  wall(incl load)=${dt}s"
  $log | Out-File -FilePath "$OutDir\$tag.log" -Encoding utf8
  & adb pull "$DEV/$tag.csv" "$OutDir\$tag.csv" 2>&1 | Out-Null
  & adb pull "$DEV/$tag.metrics" "$OutDir\$tag.metrics" 2>&1 | Out-Null
  if (Test-Path "$OutDir\$tag.csv") { Write-Host "  -> $tag.csv pulled" } else { Write-Host "  !! no CSV for $tag" }
}

$B = "--moe-stream --cache-mb 5000 --io-threads 4 --overlap"
Run-Cfg "qwen5k_base" $QWEN $B
Run-Cfg "qwen5k_pf1"  $QWEN "$B --prefetch 1"
Write-Host "ALL DONE -> $OutDir"
