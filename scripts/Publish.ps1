param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repository 'dist/DeadlockModworks' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$build = Join-Path $repository 'build/release'

if (!(Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))) {
    $configured = $false
    foreach ($generator in @('Visual Studio 18 2026', 'Visual Studio 17 2022')) {
        if (Test-Path -LiteralPath $build) { Remove-Item -LiteralPath $build -Recurse -Force }
        & cmake -G $generator -A x64 -S $repository -B $build
        if ($LASTEXITCODE -eq 0) {
            $configured = $true
            break
        }
    }
    if (!$configured) { throw 'No supported Visual Studio generator was found.' }
}

$identityPath = Join-Path $build 'product_package_name.txt'
if (!(Test-Path -LiteralPath $identityPath)) { throw "Cannot find $identityPath" }
$identity = @(Get-Content -LiteralPath $identityPath)
if ($identity.Count -lt 2) { throw 'Product package identity is incomplete.' }
$exeName = $identity[0].Trim()
$zipName = $identity[1].Trim()
$target = [IO.Path]::GetFileNameWithoutExtension($exeName)

& cmake --build $build --config Release --target $target
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

$exe = Join-Path $build "bin\Release\$exeName"
if (!(Test-Path -LiteralPath $exe)) { throw "Cannot find $exe" }

if (Test-Path -LiteralPath $OutputDirectory) { Remove-Item -LiteralPath $OutputDirectory -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $OutputDirectory $exeName)
Copy-Item -LiteralPath (Join-Path $repository 'README.md') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $repository 'THIRD_PARTY_NOTICES.md') -Destination $OutputDirectory

$licenseOutput = Join-Path $OutputDirectory 'licenses'
New-Item -ItemType Directory -Force -Path $licenseOutput | Out-Null
$licenseSources = @(
    @{ Source = Join-Path $repository 'licenses\ValveResourceFormat-LICENSE.txt'; Name = 'ValveResourceFormat-LICENSE.txt' },
    @{ Source = Join-Path $build '_deps\lz4-src\LICENSE'; Name = 'lz4-LICENSE.txt' },
    @{ Source = Join-Path $build '_deps\zstd-src\LICENSE'; Name = 'zstd-LICENSE.txt' },
    @{ Source = Join-Path $build '_deps\nlohmann_json-src\LICENSE.MIT'; Name = 'nlohmann-json-LICENSE.MIT' },
    @{ Source = Join-Path $build '_deps\meshoptimizer-src\LICENSE.md'; Name = 'meshoptimizer-LICENSE.md' }
    @{ Source = Join-Path $build '_deps\imgui-src\LICENSE.txt'; Name = 'imgui-LICENSE.txt' }
    @{ Source = Join-Path $repository 'third_party\tracy\LICENSE'; Name = 'tracy-LICENSE.txt' }
)
foreach ($license in $licenseSources) {
    if (!(Test-Path -LiteralPath $license.Source)) { throw "Cannot locate $($license.Source)" }
    Copy-Item -LiteralPath $license.Source -Destination (Join-Path $licenseOutput $license.Name)
}

$zipPath = Join-Path (Split-Path -Parent $OutputDirectory) $zipName
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $OutputDirectory '*') -DestinationPath $zipPath -Force
Write-Output "Portable package: $zipPath"
