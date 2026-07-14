# On-device A/B for temporal prefetch, on top of each model's best measured config from
# docs/bench-data/2026-07-12 (Qwen: cache 4000, lane 4, overlap; Gemma: cache 2000, lane 4,
# overlap). One matrix per model: base / +prefetch.
param(
  [string]$OutDir = "C:\Users\raffa\Documents\BigMoeOnEdge\.bench-pr23",
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
  if (Test-Path "$OutDir\$tag.csv") { Write-Host "  -> $tag.csv pulled" } else { Write-Host "  !! no CSV for $tag" }
}

# Qwen — best baseline: cache 4000 MiB, lane 4, overlap
$QB = "--moe-stream --cache-mb 4000 --io-threads 4 --overlap"
Run-Cfg "qwen_base"     $QWEN  $QB
Run-Cfg "qwen_pf2"      $QWEN  "$QB --prefetch 2"

# Gemma — best baseline: cache 2000 MiB, lane 4, overlap
$GB = "--moe-stream --cache-mb 2000 --io-threads 4 --overlap"
Run-Cfg "gemma_base"    $GEMMA $GB
Run-Cfg "gemma_pf2"     $GEMMA "$GB --prefetch 2"

Write-Host "ALL DONE -> $OutDir"
