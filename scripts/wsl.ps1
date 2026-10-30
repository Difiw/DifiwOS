# Runs a project make target inside WSL from Windows PowerShell.
#   powershell -File scripts\wsl.ps1 build
param(
    [Parameter(Mandatory = $true)][ValidateSet('check', 'fetch', 'configure', 'build', 'verify', 'run', 'test', 'clean', 'legal-info')]
    [string]$Target,
    [string]$Distro = 'Ubuntu-24.04'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$wslRoot = (& wsl.exe -d $Distro -- wslpath -a ($root -replace '\\', '/')).Trim()
& wsl.exe -d $Distro -- bash -lc "cd '$wslRoot' && make $Target"
exit $LASTEXITCODE
