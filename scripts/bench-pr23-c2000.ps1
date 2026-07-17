# Max-throughput follow-up for Qwen: push the cache lever (5000 MiB — fewer misses, closer to the
# ~7 tok/s compute ceiling) and test shallow prefetch (depth 1, low speculative volume) on top of it.
# Baseline vs +prefetch 1, at lane 4 overlap.
param(
  [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) ".bench-pr23"),
  [int]$NPred = 256,
  [int]$CooldownSec = 45,
  [string]$Qwen
)
$ErrorActionPreference = "Continue"
. "$PSScriptRoot\bench-lib.ps1"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not $Qwen) { $Qwen = $BENCH_QWEN }

$match = $BENCH_MATCH_DEFAULT + "|moe-overlap:|moe-prefetch:"

function Run-Cfg($tag, $model, $flags) {
  Invoke-BenchCfg -Tag $tag -Model $model -Flags $flags -OutDir $OutDir -NPred $NPred `
                  -CooldownSec $CooldownSec -Match $match
}

$B = "--moe-stream --cache-mb 5000 --io-threads 4 --overlap"
Run-Cfg "qwen5k_base" $Qwen $B
Run-Cfg "qwen5k_pf1"  $Qwen "$B --prefetch 1"
Write-Host "ALL DONE -> $OutDir"
