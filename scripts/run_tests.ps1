param(
  [ValidateSet("smoke", "all")]
  [string]$Label = "smoke",
  [string]$BuildDir = "build",
  [switch]$SkipEncodingCheck
)

$ErrorActionPreference = 'Stop'

function Ensure-Build {
  param($Dir)
  if (-not (Test-Path "$Dir/CMakeCache.txt")) {
    Write-Host "Configuring with CMake (Ninja)..."
    cmake -S . -B $Dir -G Ninja | Write-Host
  }
}

Ensure-Build -Dir $BuildDir

if (-not $SkipEncodingCheck) {
  Write-Host "Checking repository text encodings (UTF-8, no BOM)..."
  & "$PSScriptRoot/check_text_encoding.ps1"
}

Write-Host "Building..."
$buildOutput = & cmake --build $BuildDir 2>&1
$buildOutput | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
  throw "Build failed with exit code $LASTEXITCODE."
}

Write-Host "Running ctest (label=$Label)..."
$ctestOutput = & ctest --test-dir $BuildDir --output-on-failure --label-regex $Label 2>&1
$ctestOutput | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
  throw "ctest failed with exit code $LASTEXITCODE."
}
