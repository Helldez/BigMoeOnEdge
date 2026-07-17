# On-device benchmark matrix driver: the cache-size × read-lane grid, plus the overlap pair, per
# model. Invokes the instrumented device-side bench-run.sh via bench-lib.ps1.
param(
  [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) ".bench"),
  [int]$NPred = 256,
  [int]$CooldownSec = 45,   # let the SoC cool toward a similar baseline before each run
  [string]$Qwen,
  [string]$Gemma
)
$ErrorActionPreference = "Continue"
. "$PSScriptRoot\bench-lib.ps1"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not $Qwen)  { $Qwen  = $BENCH_QWEN }
if (-not $Gemma) { $Gemma = $BENCH_GEMMA }

function Run-Cfg($tag, $model, $flags) {
  Invoke-BenchCfg -Tag $tag -Model $model -Flags $flags -OutDir $OutDir -NPred $NPred -CooldownSec $CooldownSec
}

$models = @{ qwen = $Qwen; gemma = $Gemma }
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
