#requires -Version 5.1
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

Write-Host "=== Distributed Rate Limiter / Windows setup ===" -ForegroundColor Cyan

function Require-Command($Name, $Hint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name was not found. $Hint"
    }
}

Require-Command "git" "Install Git for Windows."
Require-Command "cmake" "Install CMake and ensure it is on PATH."
Require-Command "docker" "Install Docker Desktop and start it."

if (-not $env:VCPKG_ROOT -and $env:VCPKG_INSTALLATION_ROOT) {
    $env:VCPKG_ROOT = $env:VCPKG_INSTALLATION_ROOT
}
if (-not $env:VCPKG_ROOT) {
    if (Test-Path "C:\vcpkg\scripts\buildsystems\vcpkg.cmake") {
        $env:VCPKG_ROOT = "C:\vcpkg"
    } else {
        $env:VCPKG_ROOT = Join-Path $env:USERPROFILE "vcpkg"
    }
}

if (-not (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
    Write-Host "Installing vcpkg at $env:VCPKG_ROOT ..." -ForegroundColor Yellow
    git clone https://github.com/microsoft/vcpkg.git $env:VCPKG_ROOT
    & "$env:VCPKG_ROOT\bootstrap-vcpkg.bat" -disableMetrics
}

Write-Host "Installing C++ dependencies..." -ForegroundColor Yellow
& "$env:VCPKG_ROOT\vcpkg.exe" install --triplet x64-windows

# Persist the dependency root for future terminals and CMakePresets.json.
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $env:VCPKG_ROOT, "User")

Write-Host "Starting Redis..." -ForegroundColor Yellow
docker compose up -d redis

Write-Host "Waiting for Redis health..." -ForegroundColor Yellow
for ($i = 0; $i -lt 30; $i++) {
    $health = docker inspect -f "{{.State.Health.Status}}" distributed-rate-limiter-redis 2>$null
    if ($health -eq "healthy") { break }
    Start-Sleep -Seconds 1
}
$health = docker inspect -f "{{.State.Health.Status}}" distributed-rate-limiter-redis 2>$null
if ($health -ne "healthy") {
    throw "Redis did not become healthy. Run 'docker compose logs redis'."
}

# Avoid the common CMake generator/toolset cache mismatch.
if (Test-Path "build\CMakeCache.txt") {
    Write-Host "Removing previous build cache so generator/toolset changes cannot conflict..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force "build"
}

Write-Host "Configuring..." -ForegroundColor Yellow
cmake --preset windows-release

Write-Host "Building..." -ForegroundColor Yellow
cmake --build --preset windows-release --parallel

Write-Host "Running tests..." -ForegroundColor Yellow
ctest --preset windows-release

Write-Host ""
Write-Host "Setup complete." -ForegroundColor Green
Write-Host "Run demo: .\scripts\run_demo.ps1"
Write-Host "Run benchmark: .\build\Release\bench.exe 16 10000 1000"
Write-Host "Run Redis benchmark: .\build\Release\bench_redis.exe"
