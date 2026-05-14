[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$BinaryName = "checklist_assistant_server.exe",
    [int]$HealthTimeoutSeconds = 20
)

function Get-FreePort {
    $listener = [System.Net.Sockets.TcpListener]::Create(0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$serverStarted = $false
$stateDir = $null
$stateFile = $null
Push-Location $repoRoot
try {
    $binaryPath = Join-Path (Join-Path $repoRoot $BuildDir) $BinaryName
    if (-not (Test-Path $binaryPath)) {
        Write-Error "Cannot find server binary at $binaryPath"
        exit 1
    }

    $serverHost = "127.0.0.1"
    $port = Get-FreePort
    $healthUrl = "http://$serverHost`:$port/api/v1/health"
    $stateDir = Join-Path ([System.IO.Path]::GetTempPath()) ("chax-start-stop-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
    $stateFile = Join-Path $stateDir 'server.json'
    $shutdownToken = [Guid]::NewGuid().ToString('N')
    $startArgs = @("start", "--host", $serverHost, "--port", $port, "--no-ui", "--state-file", $stateFile, "--shutdown-token", $shutdownToken)

    Write-Host "[smoke] starting server on $serverHost`:$port" -ForegroundColor Cyan
    & $binaryPath @startArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[smoke] start command failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
    $serverStarted = $true

    $deadline = [DateTime]::UtcNow.AddSeconds($HealthTimeoutSeconds)
    $healthy = $false
    while ((-not $healthy) -and [DateTime]::UtcNow -lt $deadline) {
        try {
            $resp = Invoke-WebRequest -Uri $healthUrl -UseBasicParsing -TimeoutSec 2
            if ($resp.StatusCode -eq 200) { $healthy = $true }
        } catch {
            Start-Sleep -Milliseconds 300
        }
    }

    if (-not $healthy) {
        Write-Error "[smoke] server did not become healthy at $healthUrl"
        exit 1
    }
    Write-Host "[smoke] health ok" -ForegroundColor Green

    & $binaryPath status --state-file $stateFile

    Write-Host "[smoke] stopping server" -ForegroundColor Cyan
    & $binaryPath stop --state-file $stateFile
    $stopExit = $LASTEXITCODE

    $stopped = $false
    $stopDeadline = [DateTime]::UtcNow.AddSeconds(8)
    while ((-not $stopped) -and [DateTime]::UtcNow -lt $stopDeadline) {
        try {
            Invoke-WebRequest -Uri $healthUrl -UseBasicParsing -TimeoutSec 2 | Out-Null
        } catch {
            $stopped = $true
            break
        }
        Start-Sleep -Milliseconds 200
    }

    if (-not $stopped) {
        Write-Warning "[smoke] server may still be running; stop exit code $stopExit"
        exit 1
    }

    Write-Host "[smoke] stop ok" -ForegroundColor Green
    $serverStarted = $false
    exit 0
} finally {
    if ($serverStarted) {
        if ($stateFile) {
            & $binaryPath stop --state-file $stateFile | Out-Null
        } else {
            & $binaryPath stop | Out-Null
        }
    }
    if ($stateDir -and (Test-Path -LiteralPath $stateDir)) {
        Remove-Item -LiteralPath $stateDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    Pop-Location
}
