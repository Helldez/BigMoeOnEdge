# Cross-compile bmoe-cli for Android arm64 and stage it into the example app's jniLibs. CPU only:
# the release recipe is scripts/build-hexagon-android.sh, which adds the Hexagon, Vulkan and OpenCL
# backends inside upstream's Snapdragon toolchain container.
#
# The CLI ships as a `lib*.so` and is launched by the app via ProcessBuilder (no JNI) —
# Android only lets an app execute binaries from its nativeLibraryDir, and only files
# named lib*.so are extracted there, hence the rename.
#
# Requires the Android NDK. Point $env:ANDROID_NDK_HOME at it, or let this script try the
# default SDK location.
#
# The CPU backend is built once per instruction-set tier and the best one is picked on the phone at
# start-up (BMOE_BACKEND_MODULES, the top-level CMakeLists), so one APK runs the right kernels on a
# 2018 SoC and on one with i8mm or SME without pinning a target here.
param(
    [string]$BuildDir = "build-android",
    [string]$Abi      = "arm64-v8a",
    [int]$ApiLevel    = 29,
    [string]$BuildType = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# cmake and the NDK toolchain write ordinary progress to stderr, which Windows PowerShell turns
# into a terminating error under `ErrorActionPreference = Stop` even on success (pwsh does not).
# Judge them by their exit code so the script behaves the same in both shells.
function Invoke-Native {
    param([string]$What, [scriptblock]$Cmd)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Cmd } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

function Find-Ndk {
    if ($env:ANDROID_NDK_HOME -and (Test-Path $env:ANDROID_NDK_HOME)) { return $env:ANDROID_NDK_HOME }
    $sdk = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { "$env:LOCALAPPDATA\Android\Sdk" }
    $ndkRoot = Join-Path $sdk "ndk"
    if (Test-Path $ndkRoot) {
        $latest = Get-ChildItem $ndkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($latest) { return $latest.FullName }
    }
    throw "Android NDK not found. Set ANDROID_NDK_HOME."
}

$ndk = Find-Ndk
$toolchain = Join-Path $ndk "build\cmake\android.toolchain.cmake"
Write-Host "Using NDK: $ndk"

$buildPath = Join-Path $root $BuildDir
Invoke-Native "cmake configure" {
    cmake -S $root -B $buildPath `
        -G "Ninja" `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
        -DANDROID_ABI="$Abi" `
        -DANDROID_PLATFORM="android-$ApiLevel" `
        -DCMAKE_BUILD_TYPE="$BuildType" `
        -DBMOE_BUILD_TESTS=OFF `
        -DBMOE_BUILD_SERVER=ON `
        -DGGML_NATIVE=OFF `
        -DGGML_OPENCL=OFF `
        -DGGML_OPENMP=OFF `
        -DLLAMA_CURL=OFF
}

Invoke-Native "cmake build" { cmake --build $buildPath -j }

# Stage the CLI, the server and every library beside them into the app's jniLibs as lib*.so.
# Only the build's bin directory is read, never the whole tree, and jniLibs is wiped first: a
# recursive sweep once staged a libggml-opencl.so left over from an experiment's cmake cache, and
# it rode along into two published APKs. bin holds exactly what this configuration built.
$jni = Join-Path $root "examples\android\app\src\main\jniLibs\$Abi"
New-Item -ItemType Directory -Force -Path $jni | Out-Null
Get-ChildItem $jni -Filter "*.so" | Remove-Item -Force

$bin = Join-Path $buildPath "bin"
Copy-Item (Join-Path $bin "bmoe-cli") (Join-Path $jni "libbmoe-cli.so") -Force
Copy-Item (Join-Path $bin "bmoe-server") (Join-Path $jni "libbmoe-server.so") -Force
$libs = Get-ChildItem $bin -Filter "lib*.so"
if (-not ($libs | Where-Object { $_.Name -like "libggml-cpu-*.so" })) { throw "no CPU backend module under $bin" }
foreach ($lib in $libs) { Copy-Item $lib.FullName (Join-Path $jni $lib.Name) -Force }

# bmoe-cli links the c++_shared STL, so its runtime must ride along in the APK —
# it lives in the NDK sysroot, not the build tree.
$stl = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"
Copy-Item $stl (Join-Path $jni "libc++_shared.so") -Force

Write-Host "Staged binaries into $jni"
Get-ChildItem $jni | Select-Object Name, Length
