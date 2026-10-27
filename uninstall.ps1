<#
.SYNOPSIS
    Uninstalls OBSRdpCam. Run as administrator.
#>
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated (administrator) PowerShell.'
}

$dest = Join-Path $env:ProgramFiles 'OBSRdpCam'
$dll = Join-Path $dest 'OBSRdpCamSource.dll'

Get-Process OBSRdpCam -ErrorAction SilentlyContinue | Stop-Process -Force
if (Test-Path $dll) {
    Start-Process regsvr32 -ArgumentList '/u', '/s', "`"$dll`"" -Wait
}

Remove-Item (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\OBS RDP Camera.lnk'),
    (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\StartUp\OBS RDP Camera.lnk') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force (Join-Path $env:ProgramData 'OBSRdpCam') -ErrorAction SilentlyContinue

# the DLL may still be loaded by the Frame Server until it unloads it (or until reboot)
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
if (Test-Path $dest) {
    Write-Host "Some files are still in use, delete $dest after a reboot."
}

Write-Host 'OBSRdpCam uninstalled.'
