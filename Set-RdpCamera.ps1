<#
.SYNOPSIS
    Points a Remote Desktop (.rdp) file at the "OBS RDP Camera" (camerastoredirect).
.DESCRIPTION
    OBSRdpCam.exe must be running, since the camera only exists while it runs.
    Without -RdpFile, only prints the camera's device path. A backup (.rdp.bak) is made
    before the file is changed.
.EXAMPLE
    .\Set-RdpCamera.ps1 -RdpFile "$env:USERPROFILE\Desktop\Work.rdp"
#>
param([string]$RdpFile)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } |
    Select-Object -First 1
$op = [Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync([Windows.Devices.Enumeration.DeviceClass]::VideoCapture)
$task = $asTask.MakeGenericMethod([Windows.Devices.Enumeration.DeviceInformationCollection]).Invoke($null, @($op))
$task.Wait()

$camera = $task.Result | Where-Object { $_.Name -like 'OBS RDP Camera*' } | Select-Object -First 1
if (!$camera) { throw '"OBS RDP Camera" not found: start OBS RDP Camera first.' }
Write-Host "Camera: $($camera.Name)"
Write-Host "Device: $($camera.Id)"

if (!$RdpFile) { return }

$path = (Resolve-Path $RdpFile).Path
$bytes = [IO.File]::ReadAllBytes($path)
# mstsc saves .rdp files as UTF-16 LE
$encoding = if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { [Text.Encoding]::Unicode } else { New-Object Text.UTF8Encoding $false }
$lines = [Collections.Generic.List[string]]($encoding.GetString($bytes).TrimStart([char]0xFEFF) -split "`r?`n")
while ($lines.Count -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

$setting = "camerastoredirect:s:$($camera.Id)"
$index = $lines.FindIndex({ param($l) $l -like 'camerastoredirect:*' })
if ($index -ge 0) { $lines[$index] = $setting } else { $lines.Add($setting) }

Copy-Item $path "$path.bak" -Force
[IO.File]::WriteAllText($path, (($lines -join "`r`n") + "`r`n"), $encoding)
Write-Host "Updated $path (backup: $path.bak)"
