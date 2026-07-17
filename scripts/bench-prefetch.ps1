# On-device A/B for temporal prefetch, on top of each model's best measured config from
# docs/bench-data/2026-07-12 (Qwen: cache 4000, lane 4, overlap; Gemma: cache 2000, lane 4,
# overlap). One pair per model: base / +prefetch.
param(
  [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) ".bench-pr23"),
  [int]$NPred = 256,
  [int]$CooldownSec = 45,
  [string]$Qwen,
  [string]$Gemma
)
$ErrorActionPreference = "Continue"
. "$PSScriptRoot\bench-lib.ps1"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not $Qwen)  { $Qwen  = $BENCH_QWEN }
if (-not $Gemma) { $Gemma = $BENCH_GEMMA }

# This A/B is about overlap and prefetch, so watch their lines too.
$match = $BENCH_MATCH_DEFAULT + "|moe-overlap:|moe-prefetch:"

function Run-Cfg($tag, $model, $flags) {
  Invoke-BenchCfg -Tag $tag -Model $model -Flags $flags -OutDir $OutDir -NPred $NPred `
                  -CooldownSec $CooldownSec -Match $match
}

# Qwen — best baseline: cache 4000 MiB, lane 4, overlap
$QB = "--moe-stream --cache-mb 4000 --io-threads 4 --overlap"
Run-Cfg "qwen_base"     $Qwen  $QB
Run-Cfg "qwen_pf2"      $Qwen  "$QB --prefetch 2"

# Gemma — best baseline: cache 2000 MiB, lane 4, overlap
$GB = "--moe-stream --cache-mb 2000 --io-threads 4 --overlap"
Run-Cfg "gemma_base"    $Gemma $GB
Run-Cfg "gemma_pf2"     $Gemma "$GB --prefetch 2"

Write-Host "ALL DONE -> $OutDir"
