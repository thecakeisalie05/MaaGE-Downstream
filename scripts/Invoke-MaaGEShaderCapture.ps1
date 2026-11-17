# MaaGE Vulkan shader comparison capture and ZIP packaging.
# Compatible with Windows PowerShell 5.1 and PowerShell 7.
# Put this file in <MaaGE source>\scripts\, or pass -RepoRoot explicitly.
# Uses the repository's existing scripts\render-scene.ps1 capture facility.
[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$DataDirectory,
    [string]$Instance,
    [string]$World,
    [string]$OutputDirectory,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('Quick', 'Full')][string]$Suite = 'Full',
    [ValidateRange(64, 8192)][int]$Width = 1920,
    [ValidateRange(64, 8192)][int]$Height = 1080,
    [ValidateRange(1, 3600)][int]$Timeout = 180,
    [ValidateRange(0, 64)][int]$RenderDistance = 0,
    [ValidateRange(-30000000, 30000000)][double]$Yaw = 0,
    [ValidateRange(-89, 89)][double]$Pitch = 0,
    [double[]]$Eye,
    [double[]]$UnderwaterEye,
    [switch]$Build,
    [switch]$Update,
    [switch]$IncludeLogs
)

$ErrorActionPreference = 'Stop'
$guidPattern = '^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$'

function Select-MaaGEId {
    param([string]$Directory, [string]$GivenId, [string]$Kind)
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        throw "Cannot find $Kind directory: $Directory"
    }
    if ($GivenId) {
        if ($GivenId -notmatch $guidPattern) { throw "$Kind must be a UUID: $GivenId" }
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $GivenId) -PathType Container)) {
            throw "$Kind UUID '$GivenId' does not exist under $Directory"
        }
        return $GivenId
    }
    $choices = @(Get-ChildItem -LiteralPath $Directory -Directory |
        Where-Object { $_.Name -match $guidPattern } | Sort-Object Name)
    if ($choices.Count -eq 0) { throw "No $Kind UUID directories found in $Directory" }
    if ($choices.Count -eq 1) {
        Write-Host "Auto-selected $Kind`: $($choices[0].Name)"
        return $choices[0].Name
    }
    Write-Host "Available $Kind identifiers:"
    for ($i = 0; $i -lt $choices.Count; ++$i) {
        Write-Host ("  {0}: {1}" -f ($i + 1), $choices[$i].Name)
    }
    $selection = Read-Host "Select $Kind number (or rerun using -$Kind UUID)"
    [int]$selected = 0
    if (-not [int]::TryParse($selection, [ref]$selected) -or $selected -lt 1 -or $selected -gt $choices.Count) {
        throw "Invalid $Kind selection."
    }
    return $choices[$selected - 1].Name
}

function New-Scenario {
    param([string]$Name, [int]$Daytime, [double]$Clock = 2,
          [bool]$Enabled = $true, [string[]]$Disable = @(), [bool]$Submerged = $false)
    return [pscustomobject]@{
        Name = $Name; Daytime = $Daytime; Clock = $Clock
        Enabled = $Enabled; Disable = @($Disable); Submerged = $Submerged
    }
}

# Resolve workspace and MaaGE data root.
if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if ($Update) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw 'Git is required for -Update.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot '.git'))) {
        throw "Cannot update a directory that is not a Git checkout: $RepoRoot"
    }
    Push-Location $RepoRoot
    try {
        $currentBranch = (& git branch --show-current).Trim()
        if ($LASTEXITCODE -ne 0 -or $currentBranch -ne 'main') {
            throw "Switch to main before updating (current: $currentBranch)."
        }
        $changes = @(& git status --porcelain --untracked-files=no)
        if ($LASTEXITCODE -ne 0) { throw 'Could not inspect tracked local changes.' }
        if ($changes.Count -gt 0) {
            throw 'Tracked local edits exist. Commit or stash them before capture; nothing was overwritten.'
        }
        Write-Host 'Fetching MaaGE downstream main...'
        & git fetch origin main
        if ($LASTEXITCODE -ne 0) { throw 'git fetch failed.' }
        & git merge --ff-only origin/main
        if ($LASTEXITCODE -ne 0) { throw 'Cannot fast-forward to origin/main.' }
    }
    finally {
        Pop-Location
    }
}
$helper = Join-Path $RepoRoot 'scripts\render-scene.ps1'
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    throw "Cannot find scripts\render-scene.ps1 in $RepoRoot. Place this script in the repository's scripts directory, or use -RepoRoot."
}
if (-not $DataDirectory) {
    if (-not $env:APPDATA) { throw 'APPDATA is unavailable; supply -DataDirectory.' }
    $DataDirectory = Join-Path $env:APPDATA 'MaaGE'
}
$DataDirectory = [IO.Path]::GetFullPath($DataDirectory)
$instancesPath = Join-Path $DataDirectory 'instances'
$Instance = Select-MaaGEId -Directory $instancesPath -GivenId $Instance -Kind 'Instance'
$instanceDir = Join-Path $instancesPath $Instance
$World = Select-MaaGEId -Directory (Join-Path $instanceDir 'worlds') -GivenId $World -Kind 'World'

# Do not race a live Client's options writes or its exclusive world/instance lease.
if (Get-Process -Name 'maage-client' -ErrorAction SilentlyContinue) {
    throw 'Close every running MaaGE Client before this batch; then rerun.'
}
if ($Eye -and $Eye.Count -ne 3) { throw '-Eye needs exactly X,Y,Z coordinates.' }
if ($UnderwaterEye -and $UnderwaterEye.Count -ne 3) { throw '-UnderwaterEye needs exactly X,Y,Z coordinates.' }
if ($UnderwaterEye -and ($UnderwaterEye[1] -lt 0 -or $UnderwaterEye[1] -gt 256)) {
    throw '-UnderwaterEye Y must be between 0 and 256.'
}

if (-not $OutputDirectory) { $OutputDirectory = Join-Path $RepoRoot 'captures' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6)
$runName = 'maage-shader-validation-' + $stamp
$runDir = Join-Path $OutputDirectory $runName
$bundleDir = Join-Path $runDir 'bundle'
$imagesDir = Join-Path $bundleDir 'images'
$logsDir = Join-Path $bundleDir 'logs'
$zip = Join-Path $OutputDirectory ($runName + '.zip')
$null = New-Item -ItemType Directory -Force -Path $imagesDir
if ($IncludeLogs) { $null = New-Item -ItemType Directory -Force -Path $logsDir }

$optionsDir = Join-Path $instanceDir 'config'
$optionsPath = Join-Path $optionsDir 'options.ini'
$originalExists = Test-Path -LiteralPath $optionsPath -PathType Leaf
$originalBytes = if ($originalExists) { [IO.File]::ReadAllBytes($optionsPath) } else { [byte[]]@() }
$backupPath = Join-Path $runDir 'original-options.ini.bak'
if ($originalExists) {
    [IO.File]::WriteAllBytes($backupPath, $originalBytes)
} else {
    [IO.File]::WriteAllText((Join-Path $runDir 'original-options-was-absent.txt'),
        'The original instance config/options.ini did not exist. Delete the temporary options.ini to restore that state.')
}
[IO.File]::WriteAllText((Join-Path $runDir 'RECOVERY.txt'), @"
If PowerShell or Windows is forcibly terminated before cleanup:
  Instance config: $optionsPath
  Original existed: $originalExists
  Backup (if one existed): $backupPath
Close MaaGE first. If Original existed is True, copy the backup over options.ini.
Otherwise remove options.ini. Normal script completion restores it automatically.
This backup deliberately remains outside the ZIP, since options.ini can contain private settings.
"@)

# Balanced-quality fixed settings, so captures are comparable even if the user's
# normal video settings differ. The rest of options.ini (including controls) is untouched.
$baseEffects = [ordered]@{
    shadows = 1; shadow_quality = 1; shadow_distance = 96
    dynamic_lights = 1; light_quality = 1
    waving = 1; wind = 40
    god_rays = 1; ray_quality = 1
    sky = 1; water = 1; water_quality = 1; underwater = 1
}

$scenarios = @(
    (New-Scenario '00-day-shaders-off'       6000 2 $false)
    (New-Scenario '01-day-all-on'            6000 2)
    (New-Scenario '02-day-no-shadows'        6000 2 $true @('shadows'))
    (New-Scenario '03-day-no-rays'           6000 2 $true @('god_rays'))
    (New-Scenario '04-day-no-sky'            6000 2 $true @('sky'))
    (New-Scenario '05-day-no-water'          6000 2 $true @('water'))
    (New-Scenario '06-day-no-wind'           6000 2 $true @('waving'))
    (New-Scenario '07-day-all-on-clock8'    6000 8)
    (New-Scenario '08-day-no-wind-clock8'   6000 8 $true @('waving'))
    (New-Scenario '09-sunset-all-on'       12000 2)
    (New-Scenario '10-night-shaders-off'   18000 2 $false)
    (New-Scenario '11-night-all-on'        18000 2)
    (New-Scenario '12-night-no-lights'     18000 2 $true @('dynamic_lights'))
    (New-Scenario '13-night-no-sky'        18000 2 $true @('sky'))
    (New-Scenario '14-sunrise-all-on'      23000 2)
)
if ($Suite -eq 'Quick') {
    $quickNames = @('00-day-shaders-off', '01-day-all-on', '02-day-no-shadows',
                    '03-day-no-rays', '05-day-no-water', '10-night-shaders-off',
                    '11-night-all-on', '12-night-no-lights')
    $scenarios = @($scenarios | Where-Object { $quickNames -contains $_.Name })
}
if ($UnderwaterEye) {
    $scenarios += New-Scenario '15-underwater-all-on' 6000 2 $true @() $true
    $scenarios += New-Scenario '16-underwater-no-effect' 6000 2 $true @('underwater') $true
}

$results = New-Object 'System.Collections.Generic.List[object]'
$failures = New-Object 'System.Collections.Generic.List[object]'
$settingsOverwritten = $false
$buildPending = [bool]$Build
$startTime = Get-Date

try {
    $null = New-Item -ItemType Directory -Force -Path $optionsDir
    foreach ($scenario in $scenarios) {
        $effectValues = [ordered]@{}
        foreach ($key in $baseEffects.Keys) { $effectValues[$key] = $baseEffects[$key] }
        foreach ($disabled in $scenario.Disable) {
            if (-not ($effectValues.Keys -contains $disabled)) { throw "Unknown shader control: $disabled" }
            $effectValues[$disabled] = 0
        }

        # Later INI sections override preceding keys. Append temporary overrides
        # without editing/reformatting the user's original configuration.
        $flag = if ($scenario.Enabled) { 'true' } else { 'false' }
        $lines = @('', '[video]', "shaders_enabled=$flag", 'shader_quality=3',
                   'shader_exposure=100', '', '[shaders]')
        foreach ($key in $effectValues.Keys) { $lines += ("{0}={1}" -f $key, $effectValues[$key]) }
        $suffix = [Text.Encoding]::ASCII.GetBytes(($lines -join "`r`n") + "`r`n")
        $combined = New-Object byte[] ($originalBytes.Length + $suffix.Length)
        if ($originalBytes.Length -gt 0) {
            [Array]::Copy($originalBytes, 0, $combined, 0, $originalBytes.Length)
        }
        [Array]::Copy($suffix, 0, $combined, $originalBytes.Length, $suffix.Length)
        $settingsOverwritten = $true
        [IO.File]::WriteAllBytes($optionsPath, $combined)

        $png = Join-Path $imagesDir ($scenario.Name + '.png')
        $arguments = @{
            Instance = $Instance; World = $World; Output = $png
            Configuration = $Configuration; Width = $Width; Height = $Height
            Clock = $scenario.Clock; Daytime = $scenario.Daytime
            Timeout = $Timeout; DataDirectory = $DataDirectory
        }
        if ($RenderDistance -gt 0) { $arguments.RenderDistance = $RenderDistance }
        if ($PSBoundParameters.ContainsKey('Yaw') -or $PSBoundParameters.ContainsKey('Pitch')) {
            $arguments.Yaw = $Yaw; $arguments.Pitch = $Pitch
        }
        if ($scenario.Submerged) { $arguments.Eye = $UnderwaterEye }
        elseif ($Eye) { $arguments.Eye = $Eye }
        if ($buildPending) { $arguments.Build = $true }

        Write-Host ("[{0}/{1}] Capturing {2} ..." -f ($results.Count + 1), $scenarios.Count, $scenario.Name)
        try {
            & $helper @arguments | Out-Host
            $buildPending = $false
            if (-not (Test-Path -LiteralPath $png -PathType Leaf) -or
                -not (Test-Path -LiteralPath ($png + '.txt') -PathType Leaf)) {
                throw 'Missing PNG or PNG.txt sidecar after capture.'
            }
            $sidecar = [IO.File]::ReadAllText($png + '.txt')
            if ($sidecar -notmatch '(?m)^renderer=vulkan$') {
                throw 'Sidecar does not confirm the Vulkan renderer.'
            }
            $expected = if ($scenario.Enabled) { '1' } else { '0' }
            if ($sidecar -notmatch "(?m)^shaders=$expected`r?$") {
                throw "Sidecar does not confirm shaders=$expected."
            }
            $results.Add([pscustomobject]@{
                name = $scenario.Name
                png = 'images/' + $scenario.Name + '.png'
                metadata = 'images/' + $scenario.Name + '.png.txt'
                daytime = $scenario.Daytime
                clock = $scenario.Clock
                shaders_enabled = $scenario.Enabled
                disabled = $scenario.Disable
                underwater_eye = $scenario.Submerged
            })
        } catch {
            $failures.Add([pscustomobject]@{name = $scenario.Name; error = $_.Exception.Message})
            Write-Warning ("Capture failed: " + $_.Exception.Message)
            # Stop rather than doing the same failed launch repeatedly.
            break
        } finally {
            if ($IncludeLogs) {
                $clientLog = Join-Path $instanceDir 'logs\client.log'
                if (Test-Path -LiteralPath $clientLog -PathType Leaf) {
                    Copy-Item -LiteralPath $clientLog -Destination (Join-Path $logsDir ($scenario.Name + '.log')) -Force
                }
            }
        }
    }
} finally {
    # Restore the original settings byte-for-byte, even after a capture error.
    if ($settingsOverwritten) {
        if ($originalExists) { [IO.File]::WriteAllBytes($optionsPath, $originalBytes) }
        elseif (Test-Path -LiteralPath $optionsPath) { Remove-Item -LiteralPath $optionsPath -Force }
    }
    Write-Host 'Original instance options.ini restored.'
}

$recordedYaw = $null
$recordedPitch = $null
if ($PSBoundParameters.ContainsKey('Yaw')) { $recordedYaw = $Yaw }
if ($PSBoundParameters.ContainsKey('Pitch')) { $recordedPitch = $Pitch }
$manifest = [ordered]@{
    project = 'thecakeisalie05/MaaGE-Downstream'
    suite = $Suite
    captured_at_local = $startTime.ToString('o')
    instance = $Instance
    world = $World
    width_requested = $Width
    height_requested = $Height
    configuration = $Configuration
    render_distance_override = $RenderDistance
    yaw_override = $recordedYaw
    pitch_override = $recordedPitch
    eye_override = $Eye
    underwater_eye_override = $UnderwaterEye
    fixed_shader_settings = $baseEffects
    note = 'Each PNG.txt sidecar contains the actual camera/settings/renderer. Camera view is scene-dependent; effect visibility is not guaranteed if the scene lacks appropriate geometry or lighting.'
    completed = @($results.ToArray())
    failures = @($failures.ToArray())
}
[IO.File]::WriteAllText((Join-Path $bundleDir 'manifest.json'),
    ($manifest | ConvertTo-Json -Depth 8), [Text.Encoding]::UTF8)
[IO.File]::WriteAllText((Join-Path $bundleDir 'README.txt'), @'
MaaGE actual Vulkan scene captures. These images were exported by the native Client,
not synthesized. Compare matched time/clock shots with individual toggles changed:
  01 vs 00: full shaders vs baseline (day)
  01 vs 02: directional shadows
  01 vs 03: god rays (requires sun ahead/occlusion)
  01 vs 04: analytic sky
  01 vs 05: water (requires visible water)
  01 vs 06: vegetation wind (requires visible plants/trees)
  07 vs 08: wind effects at animation clock 8
  11 vs 10: full shaders vs baseline (night)
  11 vs 12: colored dynamic lights (requires visible emitters)
  11 vs 13: sky at night
  15 vs 16: underwater effect (only if -UnderwaterEye was supplied and is submerged)
Capture limits: single settled frame, not animation or a GPU performance benchmark.
Use each .png.txt sidecar to confirm actual options, camera, and device.
'@, [Text.Encoding]::UTF8)
Compress-Archive -Path (Join-Path $bundleDir '*') -DestinationPath $zip -CompressionLevel Optimal -Force
Write-Host ("Finished: {0} captures, {1} failures" -f $results.Count, $failures.Count)
Write-Host "ZIP: $zip"
Write-Host "Unpacked artifacts: $bundleDir"
if ($failures.Count -gt 0) { throw "Some captures failed; inspect $zip and the latest instance logs/client.log." }