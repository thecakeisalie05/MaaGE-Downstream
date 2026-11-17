# One-time VS Code shortcut setup for the desktop MaaGE workspace.
# Modifies only the selected workspace's tasks.json and the user's keybindings.json.
[CmdletBinding()]
param(
    [string]$WorkspaceRoot,
    [string]$Key = 'ctrl+alt+shift+s',
    [string]$KeybindingsPath
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $WorkspaceRoot) {
    $parent = Split-Path -Parent $repo
    if ((Split-Path -Leaf $repo) -ieq 'source' -and
        (Test-Path -LiteralPath (Join-Path $parent '.vscode/tasks.json'))) {
        $WorkspaceRoot = $parent
    } else {
        $WorkspaceRoot = $repo
    }
}
$WorkspaceRoot = [IO.Path]::GetFullPath($WorkspaceRoot)
if (-not (Test-Path -LiteralPath $WorkspaceRoot -PathType Container)) {
    throw "Workspace directory does not exist: $WorkspaceRoot"
}
$batch = Join-Path $repo 'scripts/Invoke-MaaGEShaderCapture.ps1'
if (-not (Test-Path -LiteralPath $batch -PathType Leaf)) {
    throw "Capture script is missing: $batch"
}
if (-not $KeybindingsPath) {
    if (-not $env:APPDATA) { throw 'APPDATA is missing; supply -KeybindingsPath.' }
    $KeybindingsPath = Join-Path $env:APPDATA 'Code/User/keybindings.json'
}
$taskFile = Join-Path $WorkspaceRoot '.vscode/tasks.json'
$label = 'MaaGE: Capture shader suite'

# Find the closing bracket without being confused by arrays, strings or JSONC comments.
function Find-ArrayEnd {
    param([string]$Text, [int]$Open)
    $depth = 0
    $quoted = $false
    $escaped = $false
    $lineComment = $false
    $blockComment = $false
    for ($i = $Open; $i -lt $Text.Length; ++$i) {
        $c = $Text[$i]
        $next = if ($i + 1 -lt $Text.Length) { $Text[$i + 1] } else { [char]0 }
        if ($lineComment) {
            if ($c -eq [char]10 -or $c -eq [char]13) { $lineComment = $false }
            continue
        }
        if ($blockComment) {
            if ($c -eq '*' -and $next -eq '/') { $blockComment = $false; ++$i }
            continue
        }
        if ($quoted) {
            if ($escaped) { $escaped = $false }
            elseif ($c -eq '\') { $escaped = $true }
            elseif ($c -eq '"') { $quoted = $false }
            continue
        }
        if ($c -eq '/' -and $next -eq '/') { $lineComment = $true; ++$i; continue }
        if ($c -eq '/' -and $next -eq '*') { $blockComment = $true; ++$i; continue }
        if ($c -eq '"') { $quoted = $true; continue }
        if ($c -eq '[') { ++$depth }
        elseif ($c -eq ']') {
            --$depth
            if ($depth -eq 0) { return $i }
        }
    }
    throw 'Unclosed JSON array; no files were changed by this operation.'
}

function Add-ArrayEntry {
    param(
        [string]$Path,
        [string]$JsonEntry,
        [string]$ArrayProperty,
        [string]$ExistingIdentifier
    )
    $dir = Split-Path -Parent $Path
    $null = New-Item -Path $dir -ItemType Directory -Force
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $contents = [IO.File]::ReadAllText($Path)
    } elseif ($ArrayProperty -eq 'tasks') {
        $contents = '{"version":"2.0.0","tasks":[]}'
    } else {
        $contents = '[]'
    }
    if ($contents.Contains($ExistingIdentifier)) {
        Write-Host "Already configured: $Path"
        return
    }
    if ($ArrayProperty) {
        $match = [regex]::Match($contents, '"' + [regex]::Escape($ArrayProperty) + '"\s*:\s*\[')
    } else {
        $match = [regex]::Match($contents, '(?m)^\s*\[')
    }
    if (-not $match.Success) { throw "Could not locate the expected JSON array in $Path; file unchanged." }
    $open = $contents.IndexOf('[', $match.Index)
    $close = Find-ArrayEnd -Text $contents -Open $open
    $body = $contents.Substring($open + 1, $close - $open - 1)
    # Tasks and keybindings are arrays of objects. JSONC comments may occur before entries.
    $hasEntries = $body -match '\{'
    $separator = [Environment]::NewLine
    $addition = $separator + '    ' + $JsonEntry +
        $(if ($hasEntries) { ',' } else { '' }) + $separator
    $updated = $contents.Substring(0, $open + 1) + $addition + $contents.Substring($open + 1)
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $backup = $Path + '.backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
        Copy-Item -LiteralPath $Path -Destination $backup -ErrorAction Stop
        Write-Host "Backup: $backup"
    }
    [IO.File]::WriteAllText($Path, $updated, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Updated: $Path"
}

$task = [ordered]@{
    label = $label
    type = 'process'
    command = 'powershell.exe'
    args = @('-NoProfile','-ExecutionPolicy','Bypass','-File', $batch,
             '-RepoRoot', $repo, '-Update', '-Build', '-Suite', 'Full', '-IncludeLogs')
    options = @{ cwd = $repo }
    problemMatcher = @()
    presentation = @{ reveal = 'always'; panel = 'dedicated'; clear = $true }
}
$binding = [ordered]@{
    key = $Key
    command = 'workbench.action.tasks.runTask'
    args = $label
    when = "workbenchState != 'empty'"
}
# Refuse to take over a key already explicitly bound in the user's profile.
if (Test-Path -LiteralPath $KeybindingsPath -PathType Leaf) {
    $existing = [IO.File]::ReadAllText($KeybindingsPath)
    if (-not $existing.Contains('"' + $label + '"') -and
        [regex]::IsMatch($existing, '"key"\s*:\s*"' + [regex]::Escape($Key) + '"', 'IgnoreCase')) {
        throw "The key $Key already exists in VS Code keybindings.json. Choose -Key with another combination."
    }
}
Add-ArrayEntry -Path $taskFile -JsonEntry ($task | ConvertTo-Json -Depth 10 -Compress) -ArrayProperty 'tasks' -ExistingIdentifier ('"' + $label + '"')
Add-ArrayEntry -Path $KeybindingsPath -JsonEntry ($binding | ConvertTo-Json -Depth 10 -Compress) -ArrayProperty '' -ExistingIdentifier ('"' + $label + '"')
Write-Host "Installed shortcut: $Key -> $label"
Write-Host "Workspace: $WorkspaceRoot"
Write-Host 'Open that folder in VS Code. The task updates main (fast-forward only), builds and packages the capture suite.'
