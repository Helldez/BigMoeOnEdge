# Cross-compile bmoe-cli for Android arm64 and stage it into the example app's jniLibs.
#
# The CLI ships as a `lib*.so` and is launched by the app via ProcessBuilder (no JNI) —
# Android only lets an app execute binaries from its nativeLibraryDir, and only files
# named lib*.so are extracted there, hence the rename.
#
# Requires the Android NDK. Point $env:ANDROID_NDK_HOME at it, or let this script try the
# default SDK location.
#
# CPU target — armv8.2-a + dotprod + fp16, deliberately NOT i8mm. i8mm (armv8.6) is only on
# ~2021+ flagships; a build that pins it SIGILLs during the prefill GEMM on older SoCs (e.g. the
# Snapdragon 865 / OnePlus 8 Pro). dotprod + fp16 (armv8.2, ~2018+) is supported by every SoC
# that can realistically run a >RAM MoE model, so this one binary is device-agnostic across the
# viable device range while keeping every int8/fp16 kernel that matters for Q4_K experts.
#
# Note: ggml's per-device runtime dispatch (GGML_CPU_ALL_VARIANTS + GGML_BACKEND_DL) is NOT used
# because it splits the CPU backend into dlopen'd variant .so's, which drops the bmoe fork's
# statically-linked `ggml_cpu_set_expert_ready_hook` (the I/O–compute overlap hook) — the two are
# mutually exclusive today. A single dotprod baseline keeps overlap and still covers the field.
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
        -DGGML_NATIVE=OFF `
        -DGGML_OPENCL=OFF `
        -DGGML_OPENMP=OFF `
        -DGGML_CPU_ARM_ARCH="armv8.2-a+dotprod+fp16" `
        -DLLAMA_CURL=OFF
}

Invoke-Native "cmake build" { cmake --build $buildPath -j }

# Stage the CLI and the shared libs it needs into the app's jniLibs as lib*.so.
# The list is explicit, and jniLibs is wiped first: a recursive sweep of the build tree once
# staged a libggml-opencl.so left over from a GPU experiment's cmake cache, and it rode along
# into two published APKs. If a submodule bump adds a library, the app fails at dlopen — loud —
# instead of the APK silently growing a binary nobody chose to ship.
$libs = @("libggml.so", "libggml-base.so", "libggml-cpu.so", "libllama.so", "libllama-common.so")

$jni = Join-Path $root "examples\android\app\src\main\jniLibs\$Abi"
New-Item -ItemType Directory -Force -Path $jni | Out-Null
Get-ChildItem $jni -Filter "*.so" | Remove-Item -Force

$cli = Join-Path $buildPath "cli\bmoe-cli"
Copy-Item $cli (Join-Path $jni "libbmoe-cli.so") -Force
foreach ($name in $libs) {
    $src = Get-ChildItem -Path $buildPath -Recurse -Filter $name | Select-Object -First 1
    if (-not $src) { throw "$name not found in $buildPath" }
    Copy-Item $src.FullName (Join-Path $jni $name) -Force
}

# bmoe-cli links the c++_shared STL, so its runtime must ride along in the APK —
# it lives in the NDK sysroot, not the build tree.
$stl = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"
Copy-Item $stl (Join-Path $jni "libc++_shared.so") -Force

Write-Host "Staged binaries into $jni"
Get-ChildItem $jni | Select-Object Name, Length
