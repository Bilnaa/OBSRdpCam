<#
.SYNOPSIS
    Builds OBSRdpCam (x64) and packages dist\OBSRdpCam-x64.zip.
.DESCRIPTION
    Needs Visual Studio 2022+ (or its Build Tools) with the "Desktop development with C++"
    workload and a Windows 11 SDK. NuGet packages listed in packages.config are downloaded
    from nuget.org into src\packages.
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = $PSScriptRoot
$src = Join-Path $root 'src'

# NuGet packages (packages.config style, restored by hand so nuget.exe isn't needed)
$packagesDir = Join-Path $src 'packages'
$packages = @{}
foreach ($config in Get-ChildItem $src -Recurse -Filter packages.config) {
    foreach ($p in ([xml](Get-Content $config.FullName -Raw)).packages.package) {
        $packages["$($p.id).$($p.version)"] = $p
    }
}

foreach ($p in $packages.Values) {
    $dir = Join-Path $packagesDir "$($p.id).$($p.version)"
    if (Test-Path $dir) { continue }

    $id = $p.id.ToLowerInvariant()
    $zip = Join-Path ([IO.Path]::GetTempPath()) "$id.$($p.version).zip"
    Write-Host "Downloading $($p.id) $($p.version)"
    Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$id/$($p.version)/$id.$($p.version).nupkg" -OutFile $zip -UseBasicParsing
    Expand-Archive $zip -DestinationPath $dir
    Remove-Item $zip
}

# MSBuild & matching platform toolset
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'Visual Studio (or Build Tools) not found.' }

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json | Select-Object -First 1
if (!$vs) { throw 'No Visual Studio installation with the C++ x64 tools found.' }

$toolset = switch ([int]($vs.installationVersion.Split('.')[0])) {
    17 { 'v143' }
    18 { 'v145' }
    default { throw "Unsupported Visual Studio version $($vs.installationVersion)" }
}

$msbuild = Join-Path $vs.installationPath 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild (Join-Path $src 'OBSRdpCam.sln') -m -nologo -v:minimal "-p:Configuration=$Configuration" -p:Platform=x64 "-p:PlatformToolset=$toolset"
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

# release package
$out = Join-Path $src "x64\$Configuration"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'OBSRdpCam'
Remove-Item -Recurse -Force $dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory (Join-Path $stage 'bin') | Out-Null

Copy-Item (Join-Path $out 'OBSRdpCam.exe'), (Join-Path $out 'OBSRdpCamSource.dll') (Join-Path $stage 'bin')
Copy-Item (Join-Path $root 'install.ps1'), (Join-Path $root 'uninstall.ps1'), (Join-Path $root 'Set-RdpCamera.ps1'), (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE') $stage

$zipPath = Join-Path $dist 'OBSRdpCam-x64.zip'
Compress-Archive (Join-Path $stage '*') $zipPath
$hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash
Set-Content (Join-Path $dist 'OBSRdpCam-x64.zip.sha256') "$hash  OBSRdpCam-x64.zip" -Encoding ascii

Write-Host "Built $zipPath"
Write-Host "SHA256 $hash"
