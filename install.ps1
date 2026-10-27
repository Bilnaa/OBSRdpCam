<#
.SYNOPSIS
    Installs OBSRdpCam. Run as administrator.
.PARAMETER AutoStart
    Also starts OBSRdpCam when any user logs on.
#>
param([switch]$AutoStart)

$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated (administrator) PowerShell.'
}

if ([Environment]::OSVersion.Version.Build -lt 22000) {
    throw 'OBSRdpCam needs Windows 11 (MFCreateVirtualCamera).'
}

# binaries: release package layout (bin\) or local build output
$bin = Join-Path $PSScriptRoot 'bin'
if (!(Test-Path (Join-Path $bin 'OBSRdpCamSource.dll'))) {
    $bin = Join-Path $PSScriptRoot 'src\x64\Release'
}
foreach ($f in 'OBSRdpCam.exe', 'OBSRdpCamSource.dll') {
    if (!(Test-Path (Join-Path $bin $f))) { throw "$f not found, run build.ps1 first." }
}

# Program Files: only admins can replace a DLL that the Frame Server services (LocalService / SYSTEM) load
$dest = Join-Path $env:ProgramFiles 'OBSRdpCam'
$data = Join-Path $env:ProgramData 'OBSRdpCam'
$dll = Join-Path $dest 'OBSRdpCamSource.dll'

Get-Process OBSRdpCam -ErrorAction SilentlyContinue | Stop-Process -Force
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $dest -Filter '*.old' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

foreach ($f in 'OBSRdpCam.exe', 'OBSRdpCamSource.dll') {
    $target = Join-Path $dest $f
    if (Test-Path $target) {
        # the DLL may still be loaded by the Frame Server: a loaded file can be renamed, not overwritten
        Move-Item $target "$target.$([DateTime]::Now.Ticks).old" -Force
    }
    Copy-Item (Join-Path $bin $f) $target
}

# Start-Process doesn't quote arguments ("Program Files" has a space)
$p = Start-Process regsvr32 -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 failed ($($p.ExitCode))" }

# camera format, written by OBSRdpCam.exe (user), read by the Frame Server
New-Item -ItemType Directory -Force $data | Out-Null
icacls $data /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null

# shortcuts
$shell = New-Object -ComObject WScript.Shell
$startMenu = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\OBS RDP Camera.lnk'
$startup = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\StartUp\OBS RDP Camera.lnk'
$links = @($startMenu)
if ($AutoStart) { $links += $startup } else { Remove-Item $startup -ErrorAction SilentlyContinue }
foreach ($link in $links) {
    $s = $shell.CreateShortcut($link)
    $s.TargetPath = Join-Path $dest 'OBSRdpCam.exe'
    $s.WorkingDirectory = $dest
    $s.Description = 'Relays the OBS virtual camera to a camera that RDP can redirect'
    $s.Save()
}

Write-Host "OBSRdpCam installed in $dest"
Write-Host 'Start the OBS virtual camera, then "OBS RDP Camera" from the Start menu.'
