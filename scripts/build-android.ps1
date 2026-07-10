# Cross-compile bmoe-cli for Android arm64 and stage it into the example app's jniLibs.
#
# The CLI ships as a `lib*.so` and is launched by the app via ProcessBuilder (no JNI) —
# Android only lets an app execute binaries from its nativeLibraryDir, and only files
# named lib*.so are extracted there, hence the rename.
#
# Requires the Android NDK. Point $env:ANDROID_NDK_HOME at it, or let this script try the
# default SDK location. Targets armv8.2-a+dotprod+i8mm+fp16 (int8 SIMD for Q4_K MoE).
param(
    [string]$BuildDir = "build-android",
    [string]$Abi      = "arm64-v8a",
    [int]$ApiLevel    = 29,
    [string]$BuildType = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

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
cmake -S $root -B $buildPath `
    -G "Ninja" `
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
    -DANDROID_ABI="$Abi" `
    -DANDROID_PLATFORM="android-$ApiLevel" `
    -DCMAKE_BUILD_TYPE="$BuildType" `
    -DBMOE_BUILD_TESTS=OFF `
    -DGGML_NATIVE=OFF `
    -DGGML_CPU_ARM_ARCH="armv8.2-a+dotprod+i8mm+fp16" `
    -DLLAMA_CURL=OFF

cmake --build $buildPath -j

# Stage the CLI and the shared libs it needs into the app's jniLibs as lib*.so.
$jni = Join-Path $root "examples\android\app\src\main\jniLibs\$Abi"
New-Item -ItemType Directory -Force -Path $jni | Out-Null

$cli = Join-Path $buildPath "cli\bmoe-cli"
Copy-Item $cli (Join-Path $jni "libbmoe-cli.so") -Force
Get-ChildItem -Path $buildPath -Recurse -Filter "*.so" |
    Where-Object { $_.Name -like "libggml*" -or $_.Name -eq "libllama.so" } |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $jni $_.Name) -Force }

Write-Host "Staged binaries into $jni"
Get-ChildItem $jni | Select-Object Name, Length
