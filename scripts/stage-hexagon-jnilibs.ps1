# Stage a Hexagon build (scripts/build-hexagon-android.sh) into the example app's jniLibs.
#
# Same rules as build-android.ps1: jniLibs is wiped first and the list is explicit, so nothing a
# previous experiment left in the build tree rides along into the APK. The CLI ships as
# libbmoe-cli.so because Android only extracts lib*.so from an APK.
param(
    [string]$BuildDir = "",
    [string]$HtpArch = "v81",
    [string]$Abi = "arm64-v8a"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build-hexagon" }
if (-not (Test-Path $BuildDir)) { throw "no Hexagon build at $BuildDir; run scripts/build-hexagon-android.sh first" }

function Find-One([string]$name) {
    $hit = Get-ChildItem -Path $BuildDir -Recurse -Filter $name -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $hit) { throw "missing $name under $BuildDir" }
    return $hit.FullName
}

$jni = Join-Path $root "examples\android\app\src\main\jniLibs\$Abi"
New-Item -ItemType Directory -Force $jni | Out-Null
Get-ChildItem $jni -Filter "*.so" | Remove-Item -Force

Copy-Item (Find-One "bmoe-cli") (Join-Path $jni "libbmoe-cli.so") -Force
$libs = @("libggml.so", "libggml-base.so", "libggml-cpu.so", "libllama.so", "libllama-common.so",
          "libggml-hexagon.so", "libggml-htp-$HtpArch.so", "libc++_shared.so")
foreach ($l in $libs) {
    Copy-Item (Find-One $l) (Join-Path $jni $l) -Force
}
Get-ChildItem $jni -Filter "*.so" | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
