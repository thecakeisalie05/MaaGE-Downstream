# Ctrl+B: build MaaGE's Windows Client and Launcher, then launch the
# selected installation through MaaGE's validated development import flow.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$preset = 'windows-msvc-x64'
$buildPreset = "windows-msvc-x64-$($Configuration.ToLowerInvariant())"
$bin = Join-Path $root "build/cmake/windows-msvc-x64/bin/$Configuration"
$launcher = Join-Path $bin 'MaaGE Launcher.exe'

function Invoke-CMake {
    param([string[]]$Arguments)
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake failed (exit $LASTEXITCODE): $($Arguments -join ' ')"
    }
}

Push-Location $root
try {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw 'CMake 3.21+ must be installed and available on PATH.'
    }
    $cache = Join-Path $root 'cmake/windows-msvc-x64/CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
        Write-Host 'Configuring the Windows x64 development build...'
        Invoke-CMake -Arguments @('--preset', $preset)
    }
    Write-Host "Building the MaaGE Launcher and Client ($Configuration)..."
    Invoke-CMake -Arguments @('--build', '--preset', $buildPreset, '--target', 'maage_launcher', 'maage_client_manifest')
    if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
        throw "Built Launcher not found: $launcher"
    }

    $packages = @(Get-ChildItem -LiteralPath $bin -Directory |
        Where-Object { $_.Name -like 'MaaGE-Client-*-Windows-*' } |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName 'maage-client.exe') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'maage-client.json') -PathType Leaf)
        })
    if ($packages.Count -eq 0) { throw "No complete Client package found beneath $bin" }
    $package = $packages |
        Sort-Object { (Get-Item -LiteralPath (Join-Path $_.FullName 'maage-client.json')).LastWriteTimeUtc } -Descending |
        Select-Object -First 1
    $client = Join-Path $package.FullName 'maage-client.exe'
    Write-Host "Starting MaaGE Launcher with Client package: $client"
    Start-Process -FilePath $launcher -ArgumentList @('--dev-launch-client', ('"' + $client + '"')) -WorkingDirectory (Split-Path $launcher -Parent)
}
finally {
    Pop-Location
}
