$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

if (-not (Test-Path ".env")) {
    Copy-Item ".env.example" ".env"
    Write-Host "Created .env from .env.example"
}

# Simple .env loader for KEY=VALUE lines.
Get-Content ".env" | ForEach-Object {
    if ($_ -match '^\s*([^#][^=]*)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1].Trim(), $matches[2].Trim())
    }
}

docker compose up -d redis

for ($i = 0; $i -lt 30; $i++) {
    $health = docker inspect -f "{{.State.Health.Status}}" distributed-rate-limiter-redis 2>$null
    if ($health -eq "healthy") { break }
    Start-Sleep -Seconds 1
}
if ((docker inspect -f "{{.State.Health.Status}}" distributed-rate-limiter-redis 2>$null) -ne "healthy") {
    throw "Redis did not become healthy."
}

if (-not (Test-Path "build\CMakeCache.txt")) {
    cmake --preset windows-release
}
cmake --build --preset windows-release --target concurrency_demo

$env:RATE_LIMITER_SCRIPT_DIR = Join-Path $Root "scripts"
& ".\build\Release\concurrency_demo.exe"
