<#
.SYNOPSIS
    One-command setup & onboarding launcher for Creo-Automation on Windows.
.EXAMPLE
    .\setup.ps1
    .\setup.ps1 -CheckOnly
    .\setup.ps1 -Auto -InstallMsvc -Mirror cn -SmokeTest
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$Auto,
    [switch]$InstallMsvc,
    [ValidateSet("global", "cn")]
    [string]$Mirror = "global",
    [switch]$SmokeTest,
    [string]$CreoRoot = "",
    [string]$License = "",
    [string]$MsvcSetup = ""
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$pyCmd = Get-Command python -ErrorAction SilentlyContinue
if (-not $pyCmd) {
    $pyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($pyLauncher) {
        $pythonExe = "py"
        $baseArgs = @("-3")
    } else {
        Write-Host "[Creo-Automation] Python 3.10+ was not found on PATH." -ForegroundColor Red
        Write-Host "Install Python cleanly via winget:" -ForegroundColor Yellow
        Write-Host "  winget install -e --id Python.Python.3.12" -ForegroundColor Cyan
        exit 2
    }
} else {
    $pythonExe = "python"
    $baseArgs = @()
}

$argsList = @($baseArgs + @("setup.py"))
if ($CheckOnly) { $argsList += "--check" }
if ($Auto) { $argsList += "--auto" }
if ($InstallMsvc) { $argsList += "--install-msvc" }
if ($Mirror) { $argsList += @("--mirror", $Mirror) }
if ($SmokeTest) { $argsList += "--smoke-test" }
if ($CreoRoot) { $argsList += @("--creo-root", $CreoRoot) }
if ($License) { $argsList += @("--license", $License) }
if ($MsvcSetup) { $argsList += @("--msvc-setup", $MsvcSetup) }

& $pythonExe @argsList
exit $LASTEXITCODE
