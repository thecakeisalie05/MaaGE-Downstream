# Export one settled native scene through the actual Client renderer.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$')][string]$Instance,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$')][string]$World,
    [Parameter(Mandatory)][string]$Output,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateRange(64,8192)][int]$Width = 1280,
    [ValidateRange(64,8192)][int]$Height = 720,
    [ValidateRange(-30000000,30000000)][double]$Yaw = 0,
    [ValidateRange(-89,89)][double]$Pitch = 0,
    [double[]]$Eye,
    [ValidateRange(0,36000)][double]$Clock = 0,
    [ValidateRange(-1,23999)][int]$Daytime = -1,
    [ValidateRange(1,3600)][int]$Timeout = 180,
    [ValidateRange(0,64)][int]$RenderDistance = 0,
    [string]$DataDirectory,
    [switch]$IncludeUI,
    [switch]$Build
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ($Build) {
    Push-Location $root
    try {
        & cmake --build --preset "windows-msvc-x64-$($Configuration.ToLowerInvariant())" --target maage_client_manifest --parallel 4
        if ($LASTEXITCODE -ne 0) { throw 'Client build failed.' }
    } finally { Pop-Location }
}
$outputPath = [IO.Path]::GetFullPath($Output)
if ([IO.Path]::GetExtension($outputPath) -cne '.png') { throw 'Output must end in .png.' }
$null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outputPath)
$bin = Join-Path $root "build/windows-msvc-x64/bin/$Configuration"
$packages = @(Get-ChildItem -LiteralPath $bin -Directory -Filter 'MaaGE-Client-*' | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_.FullName 'maage-client.json')) -and
    (Test-Path -LiteralPath (Join-Path $_.FullName 'maage-client.exe'))
} | Sort-Object { (Get-Item -LiteralPath (Join-Path $_.FullName 'maage-client.json')).LastWriteTimeUtc } -Descending)
if (!$packages.Count) { throw 'No compiled Client package found. Configure/build the workspace first.' }
$client = Join-Path $packages[0].FullName 'maage-client.exe'
$culture = [Globalization.CultureInfo]::InvariantCulture
$arguments = @("--instance=$Instance", "--world=$World", '--renderer=vulkan',
    ('"--render-scene=' + $outputPath + '"'), "--scene-size=${Width}x${Height}",
    ('--scene-clock=' + $Clock.ToString('R',$culture)), "--scene-wait=$Timeout")
if ($PSBoundParameters.ContainsKey('Yaw') -or $PSBoundParameters.ContainsKey('Pitch')) { $arguments += '--scene-look=' + $Yaw.ToString('R',$culture) + ',' + $Pitch.ToString('R',$culture) }
if ($Daytime -ge 0) { $arguments += "--scene-daytime=$Daytime" }
if ($RenderDistance -gt 0) { $arguments += "--render-distance=$RenderDistance" }
if ($IncludeUI) { $arguments += '--scene-ui' }
if ($Eye) {
    if ($Eye.Count -ne 3) { throw 'Eye requires X,Y,Z in world coordinates.' }
    $arguments += '--scene-eye=' + (($Eye | ForEach-Object { $_.ToString('R',$culture) }) -join ',')
}
if ($DataDirectory) { $arguments += '"--data-directory=' + [IO.Path]::GetFullPath($DataDirectory) + '"' }
$started = [DateTime]::UtcNow
$process = Start-Process -FilePath $client -ArgumentList $arguments -WorkingDirectory $root -PassThru -Wait
if ($process.ExitCode -ne 0) { throw "Scene capture failed (exit $($process.ExitCode)); inspect the instance logs/client.log." }
if (!(Test-Path -LiteralPath $outputPath) -or (Get-Item -LiteralPath $outputPath).LastWriteTimeUtc -lt $started) { throw 'Client exited without producing a new scene image.' }
Write-Output $outputPath
Write-Output ($outputPath + '.txt')
