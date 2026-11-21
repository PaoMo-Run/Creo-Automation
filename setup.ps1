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

$pythonExe = $null
$baseArgs = @()

$pyCmd = Get-Command python -ErrorAction SilentlyContinue
if ($pyCmd) {
    $pythonExe = $pyCmd.Source
} else {
    $pyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($pyLauncher) {
        $pythonExe = $pyLauncher.Source
        $baseArgs = @("-3")
    } else {
        $probeDirs = @(
            (Join-Path $env:LocalAppData "Programs\Python"),
            (Join-Path $env:ProgramFiles "Python*")
        )
        foreach ($pd in $probeDirs) {
            if (-not $pd) { continue }
            $cands = Get-ChildItem -Path (Join-Path $pd "*\python.exe"), (Join-Path $pd "python.exe") -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending
            if ($cands) {
                $pythonExe = $cands[0].FullName
                break
            }
        }
    }
}

if (-not $pythonExe) {
    Write-Host "[Creo-Automation] Python 3.10+ was not found on PATH or standard install folders." -ForegroundColor Red
    Write-Host "Install Python cleanly via winget:" -ForegroundColor Yellow
    Write-Host "  winget install -e --id Python.Python.3.12" -ForegroundColor Cyan
    exit 2
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
