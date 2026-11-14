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
$bin = Join-Path $root "build/windows-msvc-x64/bin/$Configuration"
$launcher = Join-Path $bin 'MaaGE Launcher.exe'

function Invoke-Git {
    param([string[]]$Arguments)
    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Git failed (exit $LASTEXITCODE): git $($Arguments -join ' ')"
    }
}

function Update-Source {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw 'Git must be installed and available on PATH to update MaaGE automatically.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root '.git'))) {
        throw "The source directory is not a Git checkout: $root"
    }

    $branch = (& git branch --show-current).Trim()
    if ($LASTEXITCODE -ne 0 -or $branch -ne 'main') {
        throw "The Ctrl+B workflow requires the downstream main branch. Current branch: $branch"
    }

    $changes = @(& git status --porcelain --untracked-files=no)
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect local changes.' }
    if ($changes.Count -gt 0) {
        throw 'Tracked local source modifications detected. Commit, stash or revert them before Ctrl+B; nothing was overwritten.'
    }

    Write-Host 'Fetching the latest downstream main...'
    Invoke-Git -Arguments @('fetch', 'origin', 'main')
    Write-Host 'Fast-forwarding main (never rebasing or discarding local commits)...'
    Invoke-Git -Arguments @('merge', '--ff-only', 'origin/main')
}


function Get-MaaGEClientProcess {
    param([datetime]$StartedAfter)
    $candidates = @(Get-CimInstance -ClassName Win32_Process -Filter "Name = 'maage-client.exe'" -ErrorAction Stop)
    foreach ($process in $candidates) {
        if (-not $process.CommandLine -or $process.CommandLine -notmatch '(?i)--instance=([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})') { continue }
        if ([datetime]$process.CreationDate -lt $StartedAfter.AddSeconds(-2)) { continue }
        return $process
    }
    return $null
}

function Get-MaaGEClientIdentity {
    param($Process)
    $line = [string]$Process.CommandLine
    $instance = [regex]::Match($line, '(?i)--instance=([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})')
    $root = [regex]::Match($line, '(?i)(?:^|\s)(?:"--data-directory=([^"]+)"|--data-directory=("[^"]+"|\S+))')
    if (-not $instance.Success -or -not $root.Success) {
        throw 'Could not identify the launched MaaGE instance and data root from the Client process arguments.'
    }
    $dataPath = if ($root.Groups[1].Success) { $root.Groups[1].Value } else { $root.Groups[2].Value.Trim('"') }
    $dataPath = [System.IO.Path]::GetFullPath($dataPath)
    return @{
        Instance = $instance.Groups[1].Value
        DataRoot = $dataPath
    }
}

function Export-MaaGEProfile {
    param(
        [hashtable]$Identity,
        [datetime]$StartedAt,
        [datetime]$FinishedAt,
        [string]$Revision,
        [string]$Configuration
    )
    $logs = Join-Path (Join-Path (Join-Path $Identity.DataRoot 'instances') $Identity.Instance) 'logs'
    if (-not (Test-Path -LiteralPath $logs -PathType Container)) {
        Write-Warning "No instance logs folder found: $logs"
        return
    }

    $out = Join-Path (Split-Path $root -Parent) 'profile-reports'
    New-Item -ItemType Directory -Path $out -Force | Out-Null
    $stamp = $StartedAt.ToUniversalTime().ToString('yyyyMMdd-HHmmss')
    $name = "MaaGE-profile-$stamp-$($Identity.Instance.Substring(0, 8))"
    $archive = Join-Path $out "$name.zip"
    $staging = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    try {
        $instanceFolder = Join-Path $staging 'instance-logs'
        New-Item -ItemType Directory -Path $instanceFolder | Out-Null
        $files = @(Get-ChildItem -LiteralPath $logs -File -ErrorAction Stop |
            Where-Object {
                $_.LastWriteTime -ge $StartedAt.AddSeconds(-5) -and (
                    $_.Name -eq 'client.log' -or
                    $_.Name -match '^profile-[0-9]+-[0-9]+\.csv
    param([string[]]$Arguments)
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake failed (exit $LASTEXITCODE): $($Arguments -join ' ')"
    }
}

Push-Location $root
try {
    Update-Source
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
    $env:MAAGE_PROFILE = '1'
    $revision = (& git rev-parse --short HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { $revision = 'unknown' }
    $startedAt = Get-Date
    $launcherProcess = Start-Process -FilePath $launcher -ArgumentList @('--dev-launch-client', ('"' + $client + '"')) -WorkingDirectory (Split-Path $launcher -Parent) -PassThru
    Watch-MaaGEClient -LauncherProcess $launcherProcess -StartedAt $startedAt -Revision $revision -Configuration $Configuration
}
finally {
    Pop-Location
}
 -or
                    $_.Name -match '^client.*\.log(\.gz)?
    param([string[]]$Arguments)
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake failed (exit $LASTEXITCODE): $($Arguments -join ' ')"
    }
}

Push-Location $root
try {
    Update-Source
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
    $env:MAAGE_PROFILE = '1'
    Start-Process -FilePath $launcher -ArgumentList @('--dev-launch-client', ('"' + $client + '"')) -WorkingDirectory (Split-Path $launcher -Parent)
}
finally {
    Pop-Location
}

                )
            })
        foreach ($file in $files) {
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $instanceFolder $file.Name) -ErrorAction Stop
        }
        $manifest = [ordered]@{
            description = 'MaaGE development run diagnostics'
            started_utc = $StartedAt.ToUniversalTime().ToString('o')
            ended_utc = $FinishedAt.ToUniversalTime().ToString('o')
            git_revision = $Revision
            build_configuration = $Configuration
            instance_id = $Identity.Instance
            logs_included = @($files | ForEach-Object { $_.Name })
            notes = 'Profiler timing is CPU wall time, not GPU execution time. Review logs for account identifiers before sharing.'
        }
        $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $staging 'run.json') -Encoding UTF8
        if (Test-Path -LiteralPath $archive) {
            $archive = Join-Path $out "$name-$([guid]::NewGuid().ToString('N').Substring(0, 8)).zip"
        }
        Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $archive -CompressionLevel Optimal -ErrorAction Stop
        Write-Host ''
        Write-Host "MaaGE diagnostic archive: $archive" -ForegroundColor Green
        Write-Host "Included $($files.Count) session log/profile file(s)."
        if ($files.Count -eq 0) { Write-Warning 'No current-session logs or CSV files were found. Archive contains only run.json.' }
        Write-Host 'Review the archive for private information before uploading it.'
    }
    finally {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Watch-MaaGEClient {
    param(
        [System.Diagnostics.Process]$LauncherProcess,
        [datetime]$StartedAt,
        [string]$Revision,
        [string]$Configuration
    )
    Write-Host 'Waiting for MaaGE Client to start; diagnostic ZIP will be produced when it exits.'
    $clientProcess = $null
    $identity = $null
    $warnedCim = $false
    while ($true) {
        try {
            $clientProcess = Get-MaaGEClientProcess -StartedAfter $StartedAt
        }
        catch {
            if (-not $warnedCim) {
                Write-Warning "Cannot inspect Client processes: $_"
                $warnedCim = $true
            }
            break
        }
        if ($clientProcess) {
            $identity = Get-MaaGEClientIdentity -Process $clientProcess
            Write-Host "MaaGE Client detected (PID $($clientProcess.ProcessId), instance $($identity.Instance))."
            break
        }
        $LauncherProcess.Refresh()
        if ($LauncherProcess.HasExited) {
            Write-Warning 'Launcher exited before a MaaGE Client was detected. No game profile archive was created.'
            return
        }
        Start-Sleep -Seconds 1
    }
    if (-not $identity) {
        Write-Warning 'Could not identify the game process; no automatic archive was produced.'
        return
    }
    while ($true) {
        Start-Sleep -Seconds 1
        $running = Get-Process -Id $clientProcess.ProcessId -ErrorAction SilentlyContinue
        if (-not $running) { break }
        if ($running.StartTime -and $running.StartTime -gt $StartedAt.AddYears(1)) { break }
    }
    Start-Sleep -Milliseconds 600
    Export-MaaGEProfile -Identity $identity -StartedAt $StartedAt -FinishedAt (Get-Date) -Revision $Revision -Configuration $Configuration
}

function Invoke-CMake {
    param([string[]]$Arguments)
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake failed (exit $LASTEXITCODE): $($Arguments -join ' ')"
    }
}

Push-Location $root
try {
    Update-Source
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
    $env:MAAGE_PROFILE = '1'
    Start-Process -FilePath $launcher -ArgumentList @('--dev-launch-client', ('"' + $client + '"')) -WorkingDirectory (Split-Path $launcher -Parent)
}
finally {
    Pop-Location
}
