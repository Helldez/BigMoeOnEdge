# Stage a Hexagon build (scripts/build-hexagon-android.sh) into the example app's jniLibs.
#
# Same rules as build-android.ps1: jniLibs is wiped first and only the build's bin directory is
# read, so nothing a previous experiment left elsewhere in the tree rides along into the APK. The
# CLI and the server ship as libbmoe-cli.so and libbmoe-server.so because Android only extracts
# lib*.so from an APK.
param(
    [string]$BuildDir = "",
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

$bin = Join-Path $BuildDir "bin"
Copy-Item (Join-Path $bin "bmoe-cli") (Join-Path $jni "libbmoe-cli.so") -Force
Copy-Item (Join-Path $bin "bmoe-server") (Join-Path $jni "libbmoe-server.so") -Force
# Every library the build put beside the engine: ggml, llama, and one module per backend.
$libs = Get-ChildItem $bin -Filter "lib*.so"
foreach ($want in @("libggml-cpu-*.so", "libggml-hexagon.so", "libggml-vulkan.so", "libggml-opencl.so")) {
    if (-not ($libs | Where-Object { $_.Name -like $want })) { throw "missing $want under $bin" }
}
foreach ($lib in $libs) { Copy-Item $lib.FullName (Join-Path $jni $lib.Name) -Force }
Copy-Item (Find-One "libc++_shared.so") (Join-Path $jni "libc++_shared.so") -Force
# Every DSP skel the build made: the backend picks the one matching the phone's NPU at run time.
$skels = Get-ChildItem -Path $BuildDir -Recurse -Filter "libggml-htp-v*.so" -File |
    Sort-Object LastWriteTime -Descending | Group-Object Name | ForEach-Object { $_.Group[0] }
if (-not $skels) { throw "no libggml-htp-v*.so under $BuildDir" }
foreach ($s in $skels) { Copy-Item $s.FullName (Join-Path $jni $s.Name) -Force }
Get-ChildItem $jni -Filter "*.so" | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
