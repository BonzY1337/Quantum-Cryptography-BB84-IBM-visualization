[CmdletBinding()]
param(
    [ValidateSet("simulator", "ibm", "both")]
    [string]$Mode = "simulator",

    [switch]$InstallDependencies,

    [string]$ServiceHost = "127.0.0.1",

    [int]$Port = 5000,

    [int]$SimulatorTransmissions = 64,

    [int]$SimulatorAttackTransmissions = 256,

    [int]$IbmTransmissions = 8,

    [string]$IbmChannel = "ibm_quantum_platform",

    [string]$IbmToken = $env:IBM_QUANTUM_TOKEN,

    [string]$IbmInstance = $env:IBM_QUANTUM_INSTANCE,

    [string]$IbmBackend = $env:BB84_IBM_BACKEND,

    [int]$StartupTimeoutSeconds = 30,

    [int]$RequestTimeoutSeconds = 1800
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Wait-ForService {
    param(
        [string]$BaseUrl,
        [int]$TimeoutSeconds,
        [System.Diagnostics.Process]$Process
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($Process.HasExited) {
            throw "The BB84 service exited before it became healthy."
        }

        try {
            $health = Invoke-RestMethod -Uri "$BaseUrl/health" -TimeoutSec 5
            if ($health.status -eq "ok") {
                return
            }
        }
        catch {
            Start-Sleep -Milliseconds 750
        }
    }

    throw "Timed out waiting for the BB84 service health endpoint."
}

function Invoke-Bb84Request {
    param(
        [string]$BaseUrl,
        [int]$N,
        [bool]$Eve,
        [string]$Backend,
        [string]$BackendName,
        [int]$TimeoutSeconds
    )

    $query = "n=$N&eve=$($Eve.ToString().ToLower())&backend=$Backend"
    if ($BackendName) {
        $query += "&backend_name=$([System.Uri]::EscapeDataString($BackendName))"
    }

    $response = Invoke-WebRequest -Uri "$BaseUrl/bb84?$query" -TimeoutSec $TimeoutSeconds -UseBasicParsing
    $payload = $response.Content | ConvertFrom-Json
    [pscustomobject]@{
        Payload = $payload
        Headers = $response.Headers
    }
}

function Show-Bb84Summary {
    param(
        [string]$Label,
        [object]$Result
    )

    $payload = $Result.Payload
    $finalKey = [string]$payload.final_key
    $backendName = $Result.Headers["X-QKD-Backend-Name"]
    $backendMode = $Result.Headers["X-QKD-Backend-Mode"]

    Write-Host "$Label"
    Write-Host "  backend_mode : $backendMode"
    Write-Host "  backend_name : $backendName"
    Write-Host "  valid_bits   : $($payload.valid_bits)"
    Write-Host "  errors       : $($payload.errors)"
    Write-Host "  error_rate   : $($payload.error_rate)"
    Write-Host "  final_key_len: $($finalKey.Length)"
}

$repoRoot = $PSScriptRoot
$logRoot = [System.IO.Path]::GetTempPath()
$stdoutLog = Join-Path $logRoot "bb84-service.stdout.$PID.log"
$stderrLog = Join-Path $logRoot "bb84-service.stderr.$PID.log"
$baseUrl = "http://$ServiceHost`:$Port"

if ($InstallDependencies) {
    Write-Step "Installing Python dependencies"
    & py -m pip install -r (Join-Path $repoRoot "requirements.txt")
}

Write-Step "Preparing environment"
$env:BB84_HOST = $ServiceHost
$env:BB84_PORT = [string]$Port
$env:BB84_BACKEND_MODE = if ($Mode -eq "ibm") { "ibm" } else { "simulator" }
$env:IBM_QUANTUM_CHANNEL = $IbmChannel
if ($IbmToken) {
    $env:IBM_QUANTUM_TOKEN = $IbmToken
}
if ($IbmInstance) {
    $env:IBM_QUANTUM_INSTANCE = $IbmInstance
}
if ($IbmBackend) {
    $env:BB84_IBM_BACKEND = $IbmBackend
}

if (Test-Path -LiteralPath $stdoutLog) {
    Remove-Item -LiteralPath $stdoutLog -Force
}
if (Test-Path -LiteralPath $stderrLog) {
    Remove-Item -LiteralPath $stderrLog -Force
}

$serverProcess = $null

try {
    Write-Step "Starting BB84 backend"
    $serverProcess = Start-Process `
        -FilePath "py" `
        -ArgumentList "-m", "bb84_service" `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru

    Wait-ForService -BaseUrl $baseUrl -TimeoutSeconds $StartupTimeoutSeconds -Process $serverProcess
    Write-Host "Service is healthy at $baseUrl"

    if ($Mode -in @("simulator", "both")) {
        Write-Step "Running simulator checks"
        $simulatorBaseline = Invoke-Bb84Request `
            -BaseUrl $baseUrl `
            -N $SimulatorTransmissions `
            -Eve $false `
            -Backend "simulator" `
            -BackendName "" `
            -TimeoutSeconds $RequestTimeoutSeconds

        $simulatorAttack = Invoke-Bb84Request `
            -BaseUrl $baseUrl `
            -N $SimulatorAttackTransmissions `
            -Eve $true `
            -Backend "simulator" `
            -BackendName "" `
            -TimeoutSeconds $RequestTimeoutSeconds

        Show-Bb84Summary -Label "Simulator baseline" -Result $simulatorBaseline
        Show-Bb84Summary -Label "Simulator intercept-resend attack" -Result $simulatorAttack
    }

    if ($Mode -in @("ibm", "both")) {
        Write-Step "Resolving IBM backend"
        $backendListing = Invoke-RestMethod `
            -Uri "$baseUrl/ibm/backends?operational=true&simulators=false" `
            -TimeoutSec $RequestTimeoutSeconds

        if (-not $IbmBackend) {
            if (-not $backendListing.backends -or $backendListing.backends.Count -eq 0) {
                throw "No IBM backends were returned by /ibm/backends."
            }
            $IbmBackend = $backendListing.backends[0].name
        }

        Write-Host "Selected IBM backend: $IbmBackend"

        $ibmRun = Invoke-Bb84Request `
            -BaseUrl $baseUrl `
            -N $IbmTransmissions `
            -Eve $false `
            -Backend "ibm" `
            -BackendName $IbmBackend `
            -TimeoutSeconds $RequestTimeoutSeconds

        Show-Bb84Summary -Label "IBM backend run" -Result $ibmRun
    }
}
finally {
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Write-Step "Stopping BB84 backend"
        Stop-Process -Id $serverProcess.Id -Force
    }

    Write-Host ""
    Write-Host "stdout log: $stdoutLog"
    Write-Host "stderr log: $stderrLog"
}
