# Fetch and build the OpenCL link-time dependency for the optional Android GPU build.
#
# ggml's OpenCL backend needs two things at COMPILE time that the Android NDK does not ship:
# the Khronos headers, and a `libOpenCL.so` to link against. Neither is vendored — they are a
# build prerequisite, not part of this project — so this script clones both from Khronos and
# cross-compiles the ICD loader for the target ABI.
#
# The loader built here is a LINK STUB ONLY and is deliberately not staged into the app. At
# runtime the engine resolves `libOpenCL.so` against the device's own driver in /vendor/lib64
# (the app's ProcessBuilder already puts that on LD_LIBRARY_PATH). Shipping the Khronos loader
# instead would take priority over the vendor driver and then find no ICD to dispatch to, which
# is the failure mode where everything links, nothing crashes, and no GPU is ever found.
#
# Consequence worth knowing before using it: a binary built this way has a hard DT_NEEDED on
# libOpenCL.so, so it will not START on a device with no OpenCL driver at all. That is why
# BMOE_OPENCL is off by default; see docs/gpu-offload.md.
param(
    [string]$Abi      = "arm64-v8a",
    [int]$ApiLevel    = 29,
    [switch]$Force            # re-clone and rebuild even if the SDK dir already looks complete
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk  = Join-Path $root "third_party\opencl-sdk"

# git and cmake both write ordinary progress to stderr, which Windows PowerShell turns into a
# terminating error under `ErrorActionPreference = Stop` even when the command succeeded. Judge
# them by their exit code, which is the only thing that actually says whether they worked.
function Invoke-Native {
    param([string]$What, [scriptblock]$Cmd)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Cmd } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

function Find-Ndk {
    if ($env:ANDROID_NDK_HOME -and (Test-Path $env:ANDROID_NDK_HOME)) { return $env:ANDROID_NDK_HOME }
    $base = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA "Android\Sdk" }
    $ndkDir = Join-Path $base "ndk"
    if (Test-Path $ndkDir) {
        $newest = Get-ChildItem $ndkDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($newest) { return $newest.FullName }
    }
    throw "Android NDK not found. Set ANDROID_NDK_HOME."
}

$headersDir = Join-Path $sdk "OpenCL-Headers"
$loaderDir  = Join-Path $sdk "OpenCL-ICD-Loader"
$installDir = Join-Path $sdk "install\$Abi"
$libOut     = Join-Path $installDir "lib\libOpenCL.so"

if ((Test-Path $libOut) -and -not $Force) {
    Write-Host "opencl-sdk: already built at $installDir (use -Force to rebuild)"
    Write-Host "OpenCL_INCLUDE_DIR=$(Join-Path $installDir 'include')"
    Write-Host "OpenCL_LIBRARY=$libOut"
    exit 0
}

New-Item -ItemType Directory -Force $sdk | Out-Null

if (-not (Test-Path (Join-Path $headersDir "CL"))) {
    Write-Host "opencl-sdk: cloning OpenCL-Headers"
    if (Test-Path $headersDir) { Remove-Item -Recurse -Force $headersDir } # partial clone from a failed run
    Invoke-Native "git clone OpenCL-Headers" { git clone --depth 1 -q https://github.com/KhronosGroup/OpenCL-Headers.git $headersDir }
}
if (-not (Test-Path (Join-Path $loaderDir "CMakeLists.txt"))) {
    Write-Host "opencl-sdk: cloning OpenCL-ICD-Loader"
    if (Test-Path $loaderDir) { Remove-Item -Recurse -Force $loaderDir }
    Invoke-Native "git clone OpenCL-ICD-Loader" { git clone --depth 1 -q https://github.com/KhronosGroup/OpenCL-ICD-Loader.git $loaderDir }
}

# The headers are header-only: install them by copy so the include dir has the layout
# find_package(OpenCL) expects (<prefix>/include/CL/*.h).
$incOut = Join-Path $installDir "include\CL"
New-Item -ItemType Directory -Force $incOut | Out-Null
Copy-Item (Join-Path $headersDir "CL\*.h") $incOut -Force

$ndk = Find-Ndk
$toolchain = Join-Path $ndk "build\cmake\android.toolchain.cmake"
if (-not (Test-Path $toolchain)) { throw "NDK toolchain file not found: $toolchain" }

$loaderBuild = Join-Path $sdk "build-loader-$Abi"
Write-Host "opencl-sdk: building the ICD loader for $Abi (link stub)"
$inc = Join-Path $installDir 'include'
Invoke-Native "ICD loader configure" {
    cmake -S $loaderDir -B $loaderBuild -G "Ninja" `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DANDROID_ABI="$Abi" `
        -DANDROID_PLATFORM="android-$ApiLevel" -DCMAKE_BUILD_TYPE=Release `
        -DOPENCL_ICD_LOADER_HEADERS_DIR="$inc" `
        -DBUILD_TESTING=OFF
}
Invoke-Native "ICD loader build" { cmake --build $loaderBuild -j }

$built = Get-ChildItem $loaderBuild -Recurse -Filter "libOpenCL.so" | Select-Object -First 1
if (-not $built) { throw "ICD loader built but libOpenCL.so was not produced" }
New-Item -ItemType Directory -Force (Split-Path -Parent $libOut) | Out-Null
Copy-Item $built.FullName $libOut -Force

Write-Host ""
Write-Host "opencl-sdk ready:"
Write-Host "  OpenCL_INCLUDE_DIR=$(Join-Path $installDir 'include')"
Write-Host "  OpenCL_LIBRARY=$libOut"
Write-Host "Now run: pwsh scripts/build-android.ps1 -OpenCL"
