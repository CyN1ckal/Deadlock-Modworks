# Build Deadlock Modworks, then zip a portable package.
# Build finishes before any previous zip or script-owned staging directory is replaced.
# Caller paths are never removed recursively. A destination outside this repository,
# a filesystem or repository root, a relative path that escapes the repository,
# a path that overlaps the build or sources, a missing caller directory, or a
# caller directory that already holds unrelated files is rejected.
#
# Default build: build/release (preset windows-release, Visual Studio 18 2026, Release).
# An existing cache in that directory is reused. This script does not delete it.
# Staging: build/package-staging/<id>
# Zip:     dist/DeadlockModworks-0.1.0-win-x64.zip
# Checksum: dist/DeadlockModworks-0.1.0-win-x64.zip.sha256
param(
    [string]$BuildDirectory = '',
    [string]$Configuration = 'Release',
    [string]$PackagingRoot = '',
    [string]$OutputDirectory = '',
    [string]$OutputZip = ''
)

$ErrorActionPreference = 'Stop'

$ExpectedVersion = '0.1.0'
$ExpectedProduct = 'Deadlock Modworks'
$ExpectedExecutable = 'DeadlockModworks'
$ExpectedExe = 'DeadlockModworks.exe'
$ExpectedZip = 'DeadlockModworks-0.1.0-win-x64.zip'
$OwnerSuffix = '.owner'

$Repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')

function Get-NormalizedPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd('\')
}

function Test-IsInside([string]$Path, [string]$Parent) {
    $path = $Path.TrimEnd('\')
    $parent = $Parent.TrimEnd('\')
    if ($path.Equals($parent, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    return $path.StartsWith($parent + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Test-SameOrNested([string]$Left, [string]$Right) {
    return (Test-IsInside $Left $Right) -or (Test-IsInside $Right $Left)
}

function Test-FileSystemRoot([string]$Path) {
    $root = [IO.Path]::GetPathRoot($Path)
    if ([string]::IsNullOrEmpty($root)) { return $false }
    return $Path.TrimEnd('\').Equals($root.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
}

function Resolve-RepositoryPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'missing: destination path is empty.' }
    $combined = $Path
    if (-not [IO.Path]::IsPathRooted($Path)) {
        $combined = Join-Path $Repository $Path
    }
    return Get-NormalizedPath $combined
}

function Get-ReparseTarget([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $item = Get-Item -LiteralPath $Path -Force
    if (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return $null }
    $target = $item.Target
    if ($target -is [array]) { $target = $target | Select-Object -First 1 }
    if ([string]::IsNullOrWhiteSpace([string]$target)) {
        throw "relative-escape: reparse point has no target: $Path"
    }
    if (-not [IO.Path]::IsPathRooted([string]$target)) {
        $target = Join-Path (Split-Path -Parent $item.FullName) ([string]$target)
    }
    return Get-NormalizedPath ([string]$target)
}

function Assert-StaysInRepository([string]$Path, [string]$Label) {
    $resolved = $Path
    $reparse = Get-ReparseTarget $Path
    if ($reparse) { $resolved = $reparse }
    if (Test-FileSystemRoot $resolved) { throw "root: $Label is a filesystem root ($resolved)." }
    if (-not (Test-IsInside $resolved $Repository)) {
        throw "relative-escape: $Label resolves outside the repository ($resolved)."
    }
    if ($resolved.Equals($Repository, [StringComparison]::OrdinalIgnoreCase)) {
        throw "root: $Label is the repository root."
    }
    return $resolved
}

function Get-ProtectedPaths([string]$BuildRoot) {
    $relative = @(
        '.git', 'src', 'tests', 'docs', 'cmake', 'scripts', 'third_party',
        'assets', 'licenses', 'Backup', 'cmake-build-debug', '.github'
    )
    $paths = @()
    foreach ($name in $relative) {
        $paths += (Join-Path $Repository $name)
    }
    if ($BuildRoot) { $paths += $BuildRoot }
    return $paths
}

function Assert-NoOverlap([string]$Path, [string[]]$Protected, [string]$Label) {
    foreach ($protected in $Protected) {
        $full = Get-NormalizedPath $protected
        if (Test-SameOrNested $Path $full) {
            throw "overlapping: $Label overlaps $full."
        }
    }
}

function Assert-CallerDirectory([string]$Path, [string]$Label, [string[]]$AllowedNames) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "missing: $Label does not exist ($Path)." }
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer) { throw "overlapping: $Label is a file, not a directory ($Path)." }
    $children = @(Get-ChildItem -LiteralPath $Path -Force)
    foreach ($child in $children) {
        if ($AllowedNames -notcontains $child.Name) {
            throw "non-empty: $Label contains '$($child.Name)', which this script will not delete ($Path)."
        }
    }
}

function Remove-PackageFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer) { throw "Refusing to delete directory $Path." }
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing to delete reparse point $Path."
    }
    Remove-Item -LiteralPath $Path -Force
}

function Remove-OwnedStaging([string]$Root, [string]$KeepName) {
    $root = Assert-StaysInRepository (Get-NormalizedPath $Root) 'Packaging root'
    if (-not (Test-IsInside $root (Join-Path $Repository 'build'))) {
        throw "overlapping: refusing to clean staging outside build/ ($root)."
    }
    $children = @(Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue)
    foreach ($child in $children) {
        if ($child.Name -notlike "*$OwnerSuffix") { continue }
        if ($child.PSIsContainer) { throw "Refusing to treat owner marker as a directory: $($child.FullName)" }
        $dirName = $child.Name.Substring(0, $child.Name.Length - $OwnerSuffix.Length)
        if ($dirName -eq $KeepName) { continue }
        if ([string]::IsNullOrWhiteSpace($dirName) -or $dirName.Contains('\') -or $dirName.Contains('/')) {
            throw "Refusing to clean unexpected owner marker $($child.Name)."
        }
        $dir = Join-Path $root $dirName
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
        $dirFull = Get-NormalizedPath $dir
        if ((Split-Path -Parent $dirFull) -ne $root) { throw "Refusing to clean $dirFull." }
        if (-not (Test-IsInside $dirFull $root)) { throw "relative-escape: staging dir $dirFull leaves $root." }
        $dirItem = Get-Item -LiteralPath $dirFull -Force
        if ($dirItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to delete reparse point $dirFull."
        }
        Remove-Item -LiteralPath $dirFull -Recurse -Force
        Remove-PackageFile $child.FullName
    }
}

function Find-Dumpbin {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere was not found at $vswhere." }
    $install = & $vswhere -prerelease -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($install)) {
        throw 'Visual Studio C++ tools were not found (vswhere -prerelease).'
    }
    $tools = Join-Path $install.Trim() 'VC\Tools\MSVC'
    $found = @(Get-ChildItem -Path $tools -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\bin\\Hostx64\\x64\\dumpbin\.exe$' } |
        Sort-Object FullName -Descending)
    if ($found.Count -lt 1) { throw "dumpbin.exe was not found under $tools." }
    return $found[0].FullName
}

function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $Path" }
        $stream.Position = 0x3C
        $pe = $reader.ReadInt32()
        $stream.Position = $pe
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Not a PE file: $Path" }
        return $reader.ReadUInt16()
    } finally {
        $stream.Dispose()
    }
}

function Get-ImportedDlls([string]$ExePath, [string]$DumpbinPath) {
    $output = & $DumpbinPath /nologo /dependents $ExePath
    if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents failed for $ExePath." }
    $dlls = New-Object System.Collections.Generic.List[string]
    $inList = $false
    foreach ($line in @($output)) {
        if ($line -match 'Image has the following dependencies') { $inList = $true; continue }
        if (-not $inList) { continue }
        if ($line -match '^\s+Summary\s*$' -or $line -match '^\s+Dump of file') { break }
        if ($line -match '^\s+([A-Za-z0-9_+.-]+\.dll)\s*$') { $dlls.Add($Matches[1]) }
    }
    if ($dlls.Count -lt 1) { throw "dumpbin reported no DLL dependencies for $ExePath." }
    return @($dlls)
}

function Test-VcRuntimeDll([string]$Name) {
    return $Name -match '^(?i)(vcruntime|msvcp|concrt|vcomp|vccorlib).*\.dll$'
}

function Test-DynamicCrtImport([string]$Name) {
    if (Test-VcRuntimeDll $Name) { return $true }
    return $Name -match '^(?i)api-ms-win-crt-'
}

function Test-SystemDll([string]$Name) {
    $system = @(
        'kernel32.dll', 'kernelbase.dll', 'ntdll.dll', 'user32.dll', 'gdi32.dll', 'gdiplus.dll',
        'shell32.dll', 'ole32.dll', 'oleaut32.dll', 'combase.dll', 'advapi32.dll', 'bcrypt.dll',
        'comctl32.dll', 'comdlg32.dll', 'windowscodecs.dll', 'ws2_32.dll', 'wsock32.dll',
        'dbghelp.dll', 'shlwapi.dll', 'imm32.dll', 'setupapi.dll', 'version.dll', 'winmm.dll',
        'cfgmgr32.dll', 'uxtheme.dll', 'dwmapi.dll', 'msimg32.dll', 'propsys.dll', 'shcore.dll',
        'crypt32.dll', 'sechost.dll', 'rpcrt4.dll', 'userenv.dll', 'iphlpapi.dll', 'wintrust.dll',
        'imagehlp.dll', 'powrprof.dll', 'normaliz.dll', 'bcryptprimitives.dll', 'msasn1.dll',
        'wldap32.dll', 'dnsapi.dll', 'nsi.dll', 'cryptbase.dll', 'profapi.dll', 'msvcrt.dll',
        'ucrtbase.dll', 'ntdll.dll', 'psapi.dll', 'oleacc.dll', 'usp10.dll', 'msctf.dll'
    )
    if ($system -contains $Name.ToLowerInvariant()) { return $true }
    if ($Name -match '^(?i)(api-ms-win-|ext-ms-)') { return $true }
    return $false
}

function Find-VcRedistDll([string]$Name) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $install = & $vswhere -prerelease -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($install)) { return $null }
    $redist = Join-Path $install.Trim() 'VC\Redist\MSVC'
    if (-not (Test-Path -LiteralPath $redist)) { return $null }
    $hit = Get-ChildItem -Path $redist -Filter $Name -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC.*\.CRT\\' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

function Read-RequiredText([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Cannot find $Label at $Path" }
    return [IO.File]::ReadAllText($Path)
}

function Copy-RequiredFile([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Cannot locate $Source" }
    $parent = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
    Copy-Item -LiteralPath $Source -Destination $Destination
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

if ($Configuration -ne 'Release') { throw "Only Release packages are supported (got $Configuration)." }

$BuildGiven = $PSBoundParameters.ContainsKey('BuildDirectory') -and -not [string]::IsNullOrWhiteSpace($BuildDirectory)
$PackageGiven = $PSBoundParameters.ContainsKey('PackagingRoot') -and -not [string]::IsNullOrWhiteSpace($PackagingRoot)
$OutputDirGiven = $PSBoundParameters.ContainsKey('OutputDirectory') -and -not [string]::IsNullOrWhiteSpace($OutputDirectory)
$OutputZipGiven = $PSBoundParameters.ContainsKey('OutputZip') -and -not [string]::IsNullOrWhiteSpace($OutputZip)

if (-not $BuildGiven) { $BuildDirectory = Join-Path $Repository 'build\release' }
if (-not $PackageGiven) { $PackagingRoot = Join-Path $Repository 'build\package-staging' }
$BuildDirectory = Assert-StaysInRepository (Resolve-RepositoryPath $BuildDirectory) 'Build directory'
$PackagingRoot = Assert-StaysInRepository (Resolve-RepositoryPath $PackagingRoot) 'Packaging root'
if (-not (Test-IsInside $BuildDirectory (Join-Path $Repository 'build'))) {
    throw "overlapping: build directory must stay under build/ ($BuildDirectory)."
}
if (-not (Test-IsInside $PackagingRoot (Join-Path $Repository 'build'))) {
    throw "overlapping: packaging root must stay under build/ ($PackagingRoot)."
}
Assert-NoOverlap $PackagingRoot (Get-ProtectedPaths $BuildDirectory) 'Packaging root'
Assert-NoOverlap $BuildDirectory (Get-ProtectedPaths '') 'Build directory'

$defaultBuild = Get-NormalizedPath (Join-Path $Repository 'build\release')
if ($OutputZipGiven) {
    $OutputZip = Assert-StaysInRepository (Resolve-RepositoryPath $OutputZip) 'Output zip'
    if ((Split-Path -Leaf $OutputZip) -ne $ExpectedZip) {
        throw "Output zip must be named $ExpectedZip."
    }
    $zipParent = Split-Path -Parent $OutputZip
} else {
    $zipParent = $null
}
if ($OutputDirGiven) {
    $OutputDirectory = Assert-StaysInRepository (Resolve-RepositoryPath $OutputDirectory) 'Output directory'
} elseif ($zipParent) {
    $OutputDirectory = $zipParent
} else {
    $OutputDirectory = Get-NormalizedPath (Join-Path $Repository 'dist')
}
if ($OutputZipGiven -and $OutputDirGiven) {
    $expected = Get-NormalizedPath (Join-Path $OutputDirectory $ExpectedZip)
    if ($expected -ne $OutputZip) { throw "OutputZip must be OutputDirectory\$ExpectedZip." }
}
if (-not $OutputZipGiven) { $OutputZip = Get-NormalizedPath (Join-Path $OutputDirectory $ExpectedZip) }
$OutputDirectory = Assert-StaysInRepository $OutputDirectory 'Output directory'
$OutputZip = Assert-StaysInRepository $OutputZip 'Output zip'
Assert-NoOverlap $OutputDirectory (Get-ProtectedPaths $BuildDirectory) 'Output directory'
Assert-NoOverlap $OutputDirectory @($PackagingRoot) 'Output directory'
Assert-NoOverlap $PackagingRoot @($OutputDirectory) 'Packaging root'

$allowedOutputNames = @($ExpectedZip, ($ExpectedZip + '.sha256'), ($ExpectedZip + '.partial'))
if ($OutputDirGiven) {
    Assert-CallerDirectory $OutputDirectory 'Output directory' $allowedOutputNames
} elseif (Test-Path -LiteralPath $OutputDirectory) {
    $existing = Get-Item -LiteralPath $OutputDirectory -Force
    if (-not $existing.PSIsContainer) { throw "non-empty: output path exists and is not a directory ($OutputDirectory)." }
    $null = Assert-StaysInRepository $OutputDirectory 'Output directory'
} else {
    $parent = Split-Path -Parent $OutputDirectory
    if (-not (Test-Path -LiteralPath $parent)) { throw "missing: parent of output directory does not exist ($parent)." }
    if (Test-FileSystemRoot $parent) { throw "root: parent of output directory is a filesystem root ($parent)." }
    if (-not (Test-IsInside $parent $Repository)) {
        throw "relative-escape: parent of output directory is outside the repository ($parent)."
    }
}

if ($PackageGiven) {
    $owned = @()
    if (Test-Path -LiteralPath $PackagingRoot) {
        foreach ($child in @(Get-ChildItem -LiteralPath $PackagingRoot -Force)) {
            $allowed = $false
            if ($child.Name -like "*$OwnerSuffix") { $allowed = $true }
            $marker = Join-Path $PackagingRoot ($child.Name + $OwnerSuffix)
            if (Test-Path -LiteralPath $marker -PathType Leaf) { $allowed = $true }
            if (-not $allowed) { $owned += $child.Name }
        }
    }
    if ($owned.Count -gt 0) {
        throw "non-empty: packaging root contains '$($owned -join ', ')' ($PackagingRoot)."
    }
    if (-not (Test-Path -LiteralPath $PackagingRoot)) { throw "missing: packaging root does not exist ($PackagingRoot)." }
}

$cache = Join-Path $BuildDirectory 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) {
    if ($BuildDirectory -ne $defaultBuild) {
        throw "missing: $BuildDirectory has no CMake cache. Refusing to create or delete it."
    }
    if (Test-Path -LiteralPath $BuildDirectory) {
        $existingBuild = @(Get-ChildItem -LiteralPath $BuildDirectory -Force)
        if ($existingBuild.Count -gt 0) {
            throw "non-empty: $BuildDirectory has no CMake cache. Refusing to delete it."
        }
    }
    Push-Location $Repository
    try {
        & cmake --preset windows-release
        if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
    } finally {
        Pop-Location
    }
}
if (-not (Test-Path -LiteralPath $cache)) { throw "missing: configure did not write $cache." }

$identityPath = Join-Path $BuildDirectory 'product_package_name.txt'
$identity = @(Get-Content -LiteralPath $identityPath)
if ($identity.Count -lt 4) { throw 'Product package identity is incomplete.' }
$exeName = $identity[0].Trim()
$zipName = $identity[1].Trim()
$version = $identity[2].Trim()
$runtime = $identity[3].Trim()
if ($exeName -ne $ExpectedExe -or $zipName -ne $ExpectedZip -or $version -ne $ExpectedVersion) {
    throw "Package identity disagrees with $ExpectedExe / $ExpectedVersion / $ExpectedZip."
}
if ($runtime -ne 'static' -and $runtime -ne 'dynamic') { throw "Unknown MSVC runtime policy '$runtime'." }
$target = [IO.Path]::GetFileNameWithoutExtension($exeName)
if ($target -ne $ExpectedExecutable) { throw "Executable target '$target' is not $ExpectedExecutable." }

& cmake --build $BuildDirectory --config Release --target $target
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

$headerPath = Join-Path $BuildDirectory 'generated\product_version.hpp'
$header = Read-RequiredText $headerPath 'product_version.hpp'
foreach ($needle in @(
    '#define SR_PRODUCT_VERSION "0.1.0"',
    '#define SR_PRODUCT_NAME "Deadlock Modworks"',
    '#define SR_PRODUCT_EXECUTABLE "DeadlockModworks"'
)) {
    if ($header.IndexOf($needle) -lt 0) { throw "Generated version header does not contain $needle" }
}
$cacheText = Read-RequiredText $cache 'CMakeCache.txt'
if ($cacheText -notmatch '(?m)^CMAKE_PROJECT_VERSION:STATIC=0\.1\.0\r?$') {
    throw 'CMake cache version is not 0.1.0.'
}
if ($cacheText -notmatch '(?m)^SR_BUILD_MODEL_TOOLS:BOOL=ON\r?$') {
    throw 'Refusing to package a build with SR_BUILD_MODEL_TOOLS off.'
}
if ($cacheText -match '(?m)^SR_TRACY:BOOL=ON\r?$') {
    throw 'Refusing to package a Tracy profile build.'
}

$exe = Join-Path $BuildDirectory "bin\Release\$exeName"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Cannot find $exe" }
if ((Get-PeMachine $exe) -ne 0x8664) { throw "$exe is not an x64 PE." }

$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
if ($info.ProductName -ne $ExpectedProduct) { throw "ProductName is '$($info.ProductName)'." }
if ($info.ProductVersion -ne $ExpectedVersion) { throw "ProductVersion is '$($info.ProductVersion)'." }
if ($info.FileVersion -ne "$ExpectedVersion.0") { throw "FileVersion is '$($info.FileVersion)'." }
if ($info.OriginalFilename -ne $ExpectedExe) { throw "OriginalFilename is '$($info.OriginalFilename)'." }
if ($info.InternalName -ne $ExpectedExecutable) { throw "InternalName is '$($info.InternalName)'." }
if ($info.FileDescription -ne "$ExpectedProduct $ExpectedVersion") {
    throw "FileDescription is '$($info.FileDescription)'."
}

$dumpbin = Find-Dumpbin
$imports = @(Get-ImportedDlls $exe $dumpbin)
$shippedDlls = New-Object System.Collections.Generic.List[string]
$external = New-Object System.Collections.Generic.List[string]
foreach ($dll in $imports) {
    if ($dll.Equals('vulkan-1.dll', [StringComparison]::OrdinalIgnoreCase)) { $external.Add($dll); continue }
    if (Test-DynamicCrtImport $dll) {
        if ($runtime -eq 'static') { throw "Static runtime policy failed: $exe imports $dll." }
        if ($dll -match '^(?i)api-ms-win-crt-') { continue }
        $beside = Join-Path (Split-Path -Parent $exe) $dll
        $source = $null
        if (Test-Path -LiteralPath $beside -PathType Leaf) { $source = $beside }
        else { $source = Find-VcRedistDll $dll }
        if (-not $source) { throw "Required runtime DLL is not beside the exe and was not found in the VC redist: $dll" }
        $shippedDlls.Add($source + '|' + $dll)
        continue
    }
    if (Test-SystemDll $dll) { continue }
    $beside = Join-Path (Split-Path -Parent $exe) $dll
    if (-not (Test-Path -LiteralPath $beside -PathType Leaf)) {
        throw "Required DLL is not beside the exe: $dll"
    }
    $shippedDlls.Add($beside + '|' + $dll)
}
if ($runtime -eq 'static' -and $shippedDlls.Count -gt 0) {
    throw 'Static runtime policy failed: the package would ship extra DLLs.'
}

# The build succeeded. Only script-owned staging dirs and the previous package files can be replaced.
if (-not (Test-Path -LiteralPath $PackagingRoot)) {
    $packageParent = Split-Path -Parent $PackagingRoot
    if (-not (Test-Path -LiteralPath $packageParent)) { throw "missing: parent of packaging root does not exist ($packageParent)." }
    New-Item -ItemType Directory -Path $PackagingRoot | Out-Null
}
$PackagingRoot = Assert-StaysInRepository (Get-NormalizedPath $PackagingRoot) 'Packaging root'
if (-not (Test-Path -LiteralPath $OutputDirectory)) {
    New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
}
$OutputDirectory = Assert-StaysInRepository (Get-NormalizedPath $OutputDirectory) 'Output directory'

$stamp = Get-Date -Format 'yyyyMMddHHmmss'
$stageId = "$stamp-$PID"
$staging = Join-Path $PackagingRoot $stageId
$marker = $staging + $OwnerSuffix
if ((Test-Path -LiteralPath $staging) -or (Test-Path -LiteralPath $marker)) {
    throw "Staging id already exists: $staging"
}
New-Item -ItemType Directory -Path $staging | Out-Null
[IO.File]::WriteAllText($marker, "deadlock-modworks-publish`r`n")

$licenseOutput = Join-Path $staging 'licenses'
New-Item -ItemType Directory -Path $licenseOutput | Out-Null
Copy-RequiredFile $exe (Join-Path $staging $exeName)
Copy-RequiredFile (Join-Path $Repository 'README.md') (Join-Path $staging 'README.md')
Copy-RequiredFile (Join-Path $Repository 'THIRD_PARTY_NOTICES.md') (Join-Path $staging 'THIRD_PARTY_NOTICES.md')
$projectLicense = Join-Path $Repository 'LICENSE'
$shippedLicense = $false
if (Test-Path -LiteralPath $projectLicense -PathType Leaf) {
    Copy-RequiredFile $projectLicense (Join-Path $staging 'LICENSE')
    $shippedLicense = $true
}
$licenseSources = @(
    @{ Source = Join-Path $Repository 'licenses\ValveResourceFormat-LICENSE.txt'; Name = 'ValveResourceFormat-LICENSE.txt' },
    @{ Source = Join-Path $BuildDirectory '_deps\lz4-src\LICENSE'; Name = 'lz4-LICENSE.txt' },
    @{ Source = Join-Path $BuildDirectory '_deps\zstd-src\LICENSE'; Name = 'zstd-LICENSE.txt' },
    @{ Source = Join-Path $BuildDirectory '_deps\nlohmann_json-src\LICENSE.MIT'; Name = 'nlohmann-json-LICENSE.MIT' },
    @{ Source = Join-Path $BuildDirectory '_deps\meshoptimizer-src\LICENSE.md'; Name = 'meshoptimizer-LICENSE.md' },
    @{ Source = Join-Path $BuildDirectory '_deps\imgui-src\LICENSE.txt'; Name = 'imgui-LICENSE.txt' },
    @{ Source = Join-Path $Repository 'third_party\tracy\LICENSE'; Name = 'tracy-LICENSE.txt' }
)
foreach ($license in $licenseSources) {
    Copy-RequiredFile $license.Source (Join-Path $licenseOutput $license.Name)
}
foreach ($pair in $shippedDlls) {
    $parts = $pair.Split('|')
    Copy-RequiredFile $parts[0] (Join-Path $staging $parts[1])
}

$blocked = @(Get-ChildItem -LiteralPath $staging -Recurse -Force -File | Where-Object {
    $_.Extension -in @('.vpk', '.pdb', '.obj', '.ilk', '.exp', '.lib', '.spv', '.tracy')
})
if ($blocked.Count -gt 0) {
    throw "Staging contains excluded files: $($blocked.Name -join ', ')"
}

$files = @(Get-ChildItem -LiteralPath $staging -Recurse -Force -File | Sort-Object FullName)
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('Deadlock Modworks package manifest')
$lines.Add("Product: $ExpectedProduct")
$lines.Add("Version: $ExpectedVersion")
$lines.Add("Executable: $ExpectedExe")
$lines.Add("Zip: $ExpectedZip")
$lines.Add("Runtime: $runtime")
$lines.Add('Shaders: embedded in the executable')
$lines.Add('GameAssets: none')
if ($shippedLicense) { $lines.Add('ProjectLicense: LICENSE') } else { $lines.Add('ProjectLicense: absent') }
if ($external.Count -gt 0) { $lines.Add('ExternalImports: ' + ($external -join ', ')) } else { $lines.Add('ExternalImports: none') }
$shippedNames = @($shippedDlls | ForEach-Object { $_.Split('|')[1] })
if ($shippedNames.Count -gt 0) { $lines.Add('ShippedDlls: ' + ($shippedNames -join ', ')) } else { $lines.Add('ShippedDlls: none') }
$lines.Add('Imports:')
foreach ($dll in ($imports | Sort-Object)) { $lines.Add("  $dll") }
$lines.Add('Files:')
foreach ($file in $files) {
    $relative = $file.FullName.Substring($staging.Length).TrimStart('\').Replace('\', '/')
    $hash = Get-Sha256 $file.FullName
    $lines.Add("$relative`t$($file.Length)`t$hash")
}
$manifestPath = Join-Path $staging 'PACKAGE_MANIFEST.txt'
$utf8 = New-Object System.Text.UTF8Encoding $false
[IO.File]::WriteAllText($manifestPath, (($lines -join "`r`n") + "`r`n"), $utf8)

$partial = Join-Path $OutputDirectory ($ExpectedZip + '.partial')
Remove-PackageFile $partial
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path -LiteralPath $tar)) { throw "tar.exe is required to create the zip ($tar)." }
& $tar -a -c -f $partial -C $staging .
if ($LASTEXITCODE -ne 0) { throw "tar failed to create $partial." }
if (-not (Test-Path -LiteralPath $partial -PathType Leaf)) { throw "Zip was not written: $partial" }

$finalZip = Join-Path $OutputDirectory $ExpectedZip
$finalHash = Join-Path $OutputDirectory ($ExpectedZip + '.sha256')
Remove-PackageFile $finalZip
Move-Item -LiteralPath $partial -Destination $finalZip
$hash = Get-Sha256 $finalZip
[IO.File]::WriteAllText($finalHash, "$hash  $ExpectedZip`r`n", $utf8)
Remove-OwnedStaging $PackagingRoot $stageId

Write-Output "Portable package: $finalZip"
Write-Output "SHA-256: $hash"
Write-Output "Runtime: $runtime"
Write-Output ("Imports: " + ($imports -join ', '))
