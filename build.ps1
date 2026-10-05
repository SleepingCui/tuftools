# Build tuftools.exe. Output: build/tuftools.exe
#
# Usage:
#   pwsh -File build.ps1                 # cmake build, Release (default)
#   pwsh -File build.ps1 cmake -Debug    # cmake build, Debug
#   pwsh -File build.ps1 mingw           # plain single g++ command, no cmake
#   pwsh -File build.ps1 -Clean          # wipe build/ first
param(
    # No [Parameter()] attribute here on purpose: that would make this an advanced
    # script and auto-add the common -Debug switch, colliding with ours below.
    [ValidateSet("cmake", "mingw")]
    [string]$Backend = "cmake",

    [switch]$Debug,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here "src"
$thirdParty = Join-Path $here "third_party"
$out = Join-Path $here "build"
$exe = Join-Path $out "tuftools.exe"
$config = if ($Debug) { "Debug" } else { "Release" }

if (-not (Test-Path (Join-Path $thirdParty "nlohmann\json.hpp"))) {
    throw "vendored headers missing: expected $thirdParty\nlohmann\json.hpp"
}

if ($Clean -and (Test-Path $out)) {
    Write-Host "removing $out"
    # Data files live next to the executable, so keep them across -Clean.
    $saved = Join-Path ([System.IO.Path]::GetTempPath()) ("tuftools-data-" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $saved | Out-Null
    foreach ($name in @("costs.json", "difficulties.json")) {
        $file = Join-Path $out $name
        if (Test-Path $file) { Copy-Item $file $saved }
    }
    Remove-Item -Recurse -Force $out
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    foreach ($name in @("costs.json", "difficulties.json")) {
        $file = Join-Path $saved $name
        if (Test-Path $file) { Copy-Item $file (Join-Path $out $name) }
    }
    Remove-Item -Recurse -Force $saved
}
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Resolve-Gxx {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "g++ was not found. Install MinGW-w64 and add its bin directory to PATH."
}

function Resolve-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "CMake\bin\cmake.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\CMake\bin\cmake.exe")
    )
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem).Root) {
        $candidates += (Join-Path $drive "Program Files\CMake\bin\cmake.exe")
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }
    throw "cmake was not found. Install CMake and add its bin directory to PATH, or run: .\build.ps1 mingw"
}

function Resolve-CMakeGenerator {
    param([string]$Gxx)
    if (Get-Command ninja -ErrorAction SilentlyContinue) { return "Ninja" }
    if (Test-Path (Join-Path (Split-Path -Parent $Gxx) "mingw32-make.exe")) { return "MinGW Makefiles" }
    return $null
}

if ($Backend -eq "mingw") {
    # Plain one-command build: no cmake, no generated files.
    $gxx = Resolve-Gxx
    $sources = Get-ChildItem -Path $src -Filter *.cpp | ForEach-Object { $_.FullName }
    $flags = @("-std=c++17", "-Wall", "-I$thirdParty")
    if ($Debug) {
        $flags += @("-O0", "-g")
    } else {
        $flags += @("-O2")
    }
    Write-Host "compiling with $gxx (mingw backend)"
    # -static keeps the result dependency-free (no libwinpthread-1.dll / libstdc++-6.dll).
    & $gxx @flags -o $exe @sources -lwinhttp -static
    if ($LASTEXITCODE -ne 0) {
        throw "compilation failed with exit code $LASTEXITCODE"
    }
} else {
    $cmake = Resolve-CMake
    $gxx = Resolve-Gxx
    $generator = Resolve-CMakeGenerator -Gxx $gxx

    # A build tree is tied to the generator that created it.
    $cache = Join-Path $out "CMakeCache.txt"
    if ($generator -and (Test-Path $cache)) {
        $match = Select-String -Path $cache -Pattern "^CMAKE_GENERATOR:INTERNAL=(.+)$" | Select-Object -First 1
        if ($match) {
            $previous = $match.Matches[0].Groups[1].Value.Trim()
            if ($previous -ne $generator) {
                throw "build\ was configured for generator '$previous', but this run selected '$generator'. Run: .\build.ps1 -Clean"
            }
        }
    }

    $configure = @(
        "-S", $here,
        "-B", $out,
        "-DCMAKE_BUILD_TYPE=$config",
        "-DCMAKE_CXX_COMPILER=$gxx"
    )
    if ($generator) { $configure += @("-G", $generator) }
    if ($generator -eq "MinGW Makefiles") {
        $make = Join-Path (Split-Path -Parent $gxx) "mingw32-make.exe"
        $configure += "-DCMAKE_MAKE_PROGRAM=$make"
    }

    $label = if ($generator) { $generator } else { "default generator" }
    Write-Host "configuring with $cmake ($label, $config)"
    & $cmake @configure
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed with exit code $LASTEXITCODE"
    }

    Write-Host "compiling with $gxx"
    # Drop the artifact first: make would otherwise consider the target up to date
    # when a previous non-cmake build (or another config) left a newer exe there.
    if (Test-Path $exe) { Remove-Item $exe -Force }
    & $cmake --build $out --config $config --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path $exe)) {
    throw "expected artifact not found: $exe"
}
Write-Host "built $exe"
