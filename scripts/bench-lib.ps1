# Shared plumbing for the on-device benchmark drivers. Dot-source it:
#
#     . "$PSScriptRoot\bench-lib.ps1"
#
# Every driver does the same thing: cool the SoC, run one config through the device-side
# bench-run.sh, echo the lines worth watching, pull the CSV + metrics back. What differs between
# drivers is the config matrix and which perf lines matter — that IS the driver. This is everything
# else, and it used to be copied verbatim into each of them.

# Where bench-run.sh and the per-run artifacts live on the device.
$script:DEV_DIR = "/data/local/tmp"

# Model paths on the test device. Defaults, not constants: pass -Model whatever you are benching.
$script:BENCH_QWEN  = "/sdcard/Download/Qwen3-30B-A3B-Q4_K_M.gguf"
$script:BENCH_GEMMA = "/sdcard/Download/google_gemma-4-26B-A4B-it-Q4_K_M.gguf"

# The perf lines worth echoing for any run. A driver exercising a specific feature appends its own
# (e.g. "|moe-prefetch:") via -Match rather than re-stating the whole pattern.
$script:BENCH_MATCH_DEFAULT =
  "generation:|prefill:|moe-stream:|moe-cache:|peak_rss|mem_avail_floor|batt_temp_max|cpu_temp_max|charge_"

<#
.SYNOPSIS
Run one benchmark config on the device and pull its artifacts.

.DESCRIPTION
One cell of a matrix: cooldown, run, echo, pull. The tag names the run and its files.

KNOWN LIMITATION — the cooldown is a timer, and a timer is the wrong instrument. A fixed wait does
not reliably reach a thermal baseline, so a long matrix can drift and tok/s starts tracking run
order rather than the config under test. Read a suspicious matrix with that in mind: check the
temperature columns before believing a trend. Making this a condition (wait until the SoC is
actually cool) is the fix, and it needs device time to calibrate.
#>
function Invoke-BenchCfg {
    param(
        [Parameter(Mandatory)][string]$Tag,
        [Parameter(Mandatory)][string]$Model,
        [Parameter(Mandatory)][string]$OutDir,
        [string]$Flags = "",
        [int]$NPred = 256,
        [int]$CooldownSec = 45,
        [string]$Match = $script:BENCH_MATCH_DEFAULT
    )
    Write-Host "==================== $Tag ===================="
    Start-Sleep -Seconds $CooldownSec   # see the note above: this is a timer, not a guarantee
    $t0 = Get-Date
    $log = & adb shell "sh $script:DEV_DIR/bench-run.sh $NPred $Model $script:DEV_DIR/$Tag.csv $script:DEV_DIR/$Tag.metrics $Flags" 2>&1
    $dt = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $log | Where-Object { $_ -match $Match } | ForEach-Object { Write-Host "  $_" }
    Write-Host "  wall(incl load)=${dt}s"
    $log | Out-File -FilePath "$OutDir\$Tag.log" -Encoding utf8
    & adb pull "$script:DEV_DIR/$Tag.csv" "$OutDir\$Tag.csv" 2>&1 | Out-Null
    & adb pull "$script:DEV_DIR/$Tag.metrics" "$OutDir\$Tag.metrics" 2>&1 | Out-Null
    if (Test-Path "$OutDir\$Tag.csv") {
        Write-Host "  -> $Tag.csv + .metrics pulled"
    } else {
        Write-Host "  !! no CSV for $Tag"
    }
}
