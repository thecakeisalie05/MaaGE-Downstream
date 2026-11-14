# Ctrl+B: safely update main, build Launcher/Client, run Client, archive diagnostics.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$preset = 'windows-msvc-x64'
$buildPreset = "windows-msvc-x64-$($Configuration.ToLowerInvariant())"
$bin = Join-Path $root "build/windows-msvc-x64/bin/$Configuration"
$launcher = Join-Path $bin 'MaaGE Launcher.exe'

function Invoke-CheckedCommand {
    param([string]$Name, [string[]]$Arguments)
    & $Name @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed (exit $LASTEXITCODE): $($Arguments -join ' ')"
    }
}

function Update-Source {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw 'Git is required for the automatic source update.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root '.git'))) {
        throw "Not a Git checkout: $root"
    }
    $branch = (& git branch --show-current).Trim()
    if ($LASTEXITCODE -ne 0 -or $branch -ne 'main') {
        throw "Switch the source checkout to main before Ctrl+B (current: $branch)."
    }
    $changes = @(& git status --porcelain --untracked-files=no)
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the local Git checkout.' }
    if ($changes.Count -gt 0) {
        throw 'Tracked local edits exist. Commit, stash, or revert them before Ctrl+B; no files were overwritten.'
    }
    Write-Host 'Fetching downstream main...'
    Invoke-CheckedCommand -Name 'git' -Arguments @('fetch', 'origin', 'main')
    Write-Host 'Fast-forwarding local main...'
    Invoke-CheckedCommand -Name 'git' -Arguments @('merge', '--ff-only', 'origin/main')
}

function Get-ClientDetails {
    param([datetime]$StartedAt)
    # A Client is spawned by Launcher, not directly by this PowerShell task.
    $processes = @(Get-CimInstance Win32_Process -Filter "Name = 'maage-client.exe'" -ErrorAction Stop)
    foreach ($entry in $processes) {
        if (-not $entry.CommandLine) { continue }
        if ([datetime]$entry.CreationDate -lt $StartedAt.AddSeconds(-2)) { continue }
        $idMatch = [regex]::Match($entry.CommandLine, '(?i)--instance=([0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12})')
        $rootMatch = [regex]::Match($entry.CommandLine, '(?i)(?:"--data-directory=([^"]+)"|--data-directory="([^"]+)"|--data-directory=(\S+))')
        if (-not $idMatch.Success -or -not $rootMatch.Success) { continue }
        $dataRoot = @($rootMatch.Groups[1].Value, $rootMatch.Groups[2].Value, $rootMatch.Groups[3].Value) |
            Where-Object { $_ } | Select-Object -First 1
        if (-not $dataRoot) { continue }
        return @{
            ProcessId = [int]$entry.ProcessId
            InstanceId = $idMatch.Groups[1].Value
            DataRoot = [System.IO.Path]::GetFullPath($dataRoot)
        }
    }
    return $null
}

function Export-SessionDiagnostics {
    param(
        [hashtable]$Client,
        [datetime]$StartedAt,
        [datetime]$FinishedAt,
        [string]$Revision,
        [string]$Configuration,
        [string]$SessionId
    )
    $instanceRoot = Join-Path (Join-Path $Client.DataRoot 'instances') $Client.InstanceId
    $instanceLogs = Join-Path $instanceRoot 'logs'
    $instanceScreenshots = Join-Path $instanceRoot 'screenshots'
    $reportDir = Join-Path (Split-Path $root -Parent) 'profile-reports'
    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $stamp = $StartedAt.ToUniversalTime().ToString('yyyyMMdd-HHmmss')
    $archivePath = Join-Path $reportDir ("MaaGE-profile-{0}-{1}.zip" -f $stamp, $Client.InstanceId.Substring(0, 8))
    if (Test-Path -LiteralPath $archivePath) {
        $archivePath = Join-Path $reportDir ("MaaGE-profile-{0}-{1}-{2}.zip" -f $stamp, $Client.InstanceId.Substring(0, 8), [guid]::NewGuid().ToString('N').Substring(0, 8))
    }
    $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ('maage-diagnostics-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
    try {
        $copyDir = Join-Path $tempDir 'instance-logs'
        New-Item -ItemType Directory -Path $copyDir | Out-Null
        $included = @()
        if (Test-Path -LiteralPath $instanceLogs -PathType Container) {
            $included = @(Get-ChildItem -LiteralPath $instanceLogs -File -ErrorAction Stop |
                Where-Object {
                    $_.LastWriteTime -ge $StartedAt.AddSeconds(-5) -and
                    ($_.Name -eq 'client.log' -or
                     $_.Name -match '^profile-[0-9]+-[0-9]+\.csv$' -or
                     $_.Name -match '^client.*\.log(?:\.gz)?$')
                })
            foreach ($file in $included) {
                Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $copyDir $file.Name) -ErrorAction Stop
            }
        } else {
            Write-Warning "Instance logs folder not found: $instanceLogs"
        }

        $screenshots = @()
        if (Test-Path -LiteralPath $instanceScreenshots -PathType Container) {
            $screenshotPattern = '^screenshot-' + [regex]::Escape($SessionId) + '-[0-9]+\.png$'
            $screenshots = @(Get-ChildItem -LiteralPath $instanceScreenshots -File -ErrorAction Stop |
                Where-Object { $_.Name -match $screenshotPattern })
            if ($screenshots.Count -gt 0) {
                $screenshotFolder = Join-Path $tempDir 'screenshots'
                New-Item -ItemType Directory -Path $screenshotFolder | Out-Null
                foreach ($shot in $screenshots) {
                    Copy-Item -LiteralPath $shot.FullName -Destination (Join-Path $screenshotFolder $shot.Name) -ErrorAction Stop
                }
            }
        }
        [ordered]@{
            started_utc = $StartedAt.ToUniversalTime().ToString('o')
            ended_utc = $FinishedAt.ToUniversalTime().ToString('o')
            git_revision = $Revision
            build_configuration = $Configuration
            instance_id = $Client.InstanceId
            screenshot_session_id = $SessionId
            files = @($included | ForEach-Object { $_.Name })
            screenshots = @($screenshots | ForEach-Object { $_.Name })
            note = 'Client profiler captures CPU wall timing, not GPU execution. Logs may contain private details.'
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $tempDir 'run.json') -Encoding UTF8
        Compress-Archive -Path (Join-Path $tempDir '*') -DestinationPath $archivePath -CompressionLevel Optimal -ErrorAction Stop
        Write-Host "Diagnostic ZIP: $archivePath" -ForegroundColor Green
        Write-Host "Captured $($included.Count) log/profile file(s) and $($screenshots.Count) screenshot(s)."
        Write-Host 'Review all contents, especially logs and screenshots, before sharing.'
    }
    finally {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Wait-And-Archive {
    param(
        [System.Diagnostics.Process]$LauncherProcess,
        [datetime]$StartedAt,
        [string]$Revision,
        [string]$SessionId
    )
    Write-Host 'Waiting for MaaGE Client startup and exit to package diagnostic output...'
    $client = $null
    while (-not $client) {
        $client = Get-ClientDetails -StartedAt $StartedAt
        if ($client) { break }
        $LauncherProcess.Refresh()
        if ($LauncherProcess.HasExited) {
            Write-Warning 'Launcher exited without a detected Client. No game profile was captured.'
            return
        }
        Start-Sleep -Seconds 1
    }
    Write-Host "Tracking Client PID $($client.ProcessId) for instance $($client.InstanceId)."
    while (Get-Process -Id $client.ProcessId -ErrorAction SilentlyContinue) {
        Start-Sleep -Seconds 1
    }
    Start-Sleep -Milliseconds 500
    Export-SessionDiagnostics -Client $client -StartedAt $StartedAt -FinishedAt (Get-Date) -Revision $Revision -Configuration $Configuration -SessionId $SessionId
}

Push-Location $root
try {
    Update-Source
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw 'CMake 3.21+ must be installed and available on PATH.'
    }
    $cache = Join-Path $root 'cmake/windows-msvc-x64/CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) {
        Write-Host "Configuring MaaGE ($preset)..."
        Invoke-CheckedCommand -Name 'cmake' -Arguments @('--preset', $preset)
    }

    Write-Host "Building MaaGE Launcher and Client ($Configuration)..."
    Invoke-CheckedCommand -Name 'cmake' -Arguments @('--build', '--preset', $buildPreset, '--target', 'maage_launcher', 'maage_client_manifest')
    if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
        throw "Built Launcher not found: $launcher"
    }

    $packages = @(Get-ChildItem -LiteralPath $bin -Directory |
        Where-Object { $_.Name -like 'MaaGE-Client-*-Windows-*' } |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName 'maage-client.exe') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'maage-client.json') -PathType Leaf)
        })
    if ($packages.Count -eq 0) { throw "No complete Client package found under $bin." }
    $package = $packages |
        Sort-Object { (Get-Item -LiteralPath (Join-Path $_.FullName 'maage-client.json')).LastWriteTimeUtc } -Descending |
        Select-Object -First 1
    $clientExecutable = Join-Path $package.FullName 'maage-client.exe'
    Write-Host "Launching MaaGE with Client package: $clientExecutable"

    $env:MAAGE_PROFILE = '1'
    $env:MAAGE_PREFER_VULKAN = '1'
    $sessionId = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $env:MAAGE_SCREENSHOT_SESSION = $sessionId
    $revision = (& git rev-parse --short HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { $revision = 'unknown' }
    $startedAt = Get-Date
    $launcherProcess = Start-Process -FilePath $launcher -ArgumentList @('--dev-launch-client', ('"' + $clientExecutable + '"')) -WorkingDirectory (Split-Path $launcher -Parent) -PassThru
    Wait-And-Archive -LauncherProcess $launcherProcess -StartedAt $startedAt -Revision $revision -SessionId $sessionId
}
finally {
    Pop-Location
}
