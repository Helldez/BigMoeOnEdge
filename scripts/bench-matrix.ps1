# On-device benchmark matrix driver. Invokes the instrumented device-side bench-run.sh.
param(
  [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) ".bench"),
  [int]$NPred = 256,
  [int]$CooldownSec = 45   # let the SoC cool to a similar baseline before each run
)
$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$DEV = "/data/local/tmp"

$QWEN  = "/sdcard/Download/Qwen3-30B-A3B-Q4_K_M.gguf"
$GEMMA = "/sdcard/Download/google_gemma-4-26B-A4B-it-Q4_K_M.gguf"

function Run-Cfg($tag, $model, $flags) {
  Write-Host "==================== $tag ===================="
  Start-Sleep -Seconds $CooldownSec   # thermal cooldown so runs start from a similar baseline
  $t0 = Get-Date
  $log = & adb shell "sh $DEV/bench-run.sh $NPred $model $DEV/$tag.csv $DEV/$tag.metrics $flags" 2>&1
  $dt = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
  $log | Where-Object { $_ -match "generation:|prefill:|moe-stream:|moe-cache:|peak_rss|mem_avail_floor|batt_temp_max|cpu_temp_max|charge_" } | ForEach-Object { Write-Host "  $_" }
  Write-Host "  wall(incl load)=${dt}s"
  $log | Out-File -FilePath "$OutDir\$tag.log" -Encoding utf8
  & adb pull "$DEV/$tag.csv" "$OutDir\$tag.csv" 2>&1 | Out-Null
  & adb pull "$DEV/$tag.metrics" "$OutDir\$tag.metrics" 2>&1 | Out-Null
  if (Test-Path "$OutDir\$tag.csv") { Write-Host "  -> $tag.csv + .metrics pulled" } else { Write-Host "  !! no CSV for $tag" }
}

$models = @{ qwen = $QWEN; gemma = $GEMMA }
foreach ($name in @("qwen","gemma")) {
  $m = $models[$name]
  Run-Cfg "${name}_mmap"     $m ""
  Run-Cfg "${name}_stream"   $m "--moe-stream"
  Run-Cfg "${name}_c2000_l2" $m "--moe-stream --cache-mb 2000 --io-threads 2"
  Run-Cfg "${name}_c2000_l4" $m "--moe-stream --cache-mb 2000 --io-threads 4"
  Run-Cfg "${name}_c4000_l2" $m "--moe-stream --cache-mb 4000 --io-threads 2"
  Run-Cfg "${name}_c4000_l4" $m "--moe-stream --cache-mb 4000 --io-threads 4"
  # intra-layer I/O–compute overlap: mirror the two lead configs with --overlap
  Run-Cfg "${name}_c4000_l4_ov" $m "--moe-stream --cache-mb 4000 --io-threads 4 --overlap"
  Run-Cfg "${name}_stream_ov"   $m "--moe-stream --cache-mb 0 --io-threads 4 --overlap"
}
Write-Host "ALL DONE"
