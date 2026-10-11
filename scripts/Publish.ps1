param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'dist/DeadlockModelUpdater' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

dotnet publish (Join-Path $repository 'DeadlockModelUpdater') -c Release -r win-x64 --self-contained true -o $OutputDirectory -m:1 -p:RestoreLockedMode=true
if ($LASTEXITCODE -ne 0) { throw 'Portable publish failed.' }
dotnet publish (Join-Path $repository 'DeadlockModelUpdater.Cli') -c Release -r win-x64 --self-contained true -o $OutputDirectory -m:1 -p:RestoreLockedMode=true
if ($LASTEXITCODE -ne 0) { throw 'Console launcher publish failed.' }

# Keep license files and original package metadata beside the distributed binaries.
$assets = Get-Content (Join-Path $repository 'DeadlockModelUpdater/obj/project.assets.json') -Raw | ConvertFrom-Json
$packageRoots = @($assets.packageFolders.PSObject.Properties.Name)
$licenseOutput = Join-Path $OutputDirectory 'licenses'
New-Item -ItemType Directory -Force -Path $licenseOutput | Out-Null
Copy-Item -Path (Join-Path $repository 'licenses/*') -Destination $licenseOutput
foreach ($library in $assets.libraries.PSObject.Properties) {
    if ($library.Value.type -ne 'package') { continue }
    $packageDirectory = $packageRoots | ForEach-Object { Join-Path $_ $library.Value.path } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (!$packageDirectory) { throw "Cannot locate package notices for $($library.Name)" }
    $packageOutput = Join-Path $licenseOutput ($library.Name -replace '/', '-')
    New-Item -ItemType Directory -Force -Path $packageOutput | Out-Null
    Get-ChildItem -LiteralPath $packageDirectory -File | Where-Object { $_.Name -match '(?i)license|copying|notice|\.nuspec$' } |
        Copy-Item -Destination $packageOutput
}

# Runtime packs are SDK inputs, rather than ordinary libraries in project.assets.json.
foreach ($runtimeId in @('microsoft.netcore.app.runtime.win-x64', 'microsoft.windowsdesktop.app.runtime.win-x64')) {
    $runtimeConfig = Get-Content (Join-Path $OutputDirectory 'DeadlockModelUpdater.runtimeconfig.json') -Raw | ConvertFrom-Json
    $frameworkName = if ($runtimeId -like '*windowsdesktop*') { 'Microsoft.WindowsDesktop.App' } else { 'Microsoft.NETCore.App' }
    $runtimeVersion = ($runtimeConfig.runtimeOptions.includedFrameworks | Where-Object name -eq $frameworkName).version
    $copiedRuntime = $false
    foreach ($packageRoot in $packageRoots) {
        $runtimeDirectory = Join-Path $packageRoot "$runtimeId/$runtimeVersion"
        if (!(Test-Path -LiteralPath $runtimeDirectory)) { continue }
        $runtimeOutput = Join-Path $licenseOutput $runtimeId
        New-Item -ItemType Directory -Force -Path $runtimeOutput | Out-Null
        Get-ChildItem -LiteralPath $runtimeDirectory -File | Where-Object { $_.Name -match '(?i)license|notice|\.nuspec$' } |
            Copy-Item -Destination $runtimeOutput
        $copiedRuntime = $true
        break
    }
    if (!$copiedRuntime) { throw "Cannot locate notices for runtime $runtimeId/$runtimeVersion" }
}

$zipPath = Join-Path (Split-Path -Parent $OutputDirectory) 'DeadlockModelUpdater-win-x64.zip'
Compress-Archive -Path (Join-Path $OutputDirectory '*') -DestinationPath $zipPath -Force
Write-Output "Portable package: $zipPath"
