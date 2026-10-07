param([Parameter(Mandatory=$true)][string]$Target)
$ErrorActionPreference = 'Stop'
$Source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'baseline\release')).TrimEnd('\')
$Destination = [IO.Path]::GetFullPath($Target).TrimEnd('\')
$Allowed = @('C:\Users\Administrator\Desktop\price\dist\RelinkStudio', [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'rollback_test')))
if ($Allowed -notcontains $Destination) { throw 'Target must be the recorded release or isolated rollback_test directory.' }
if (!(Test-Path -LiteralPath $Destination -PathType Container)) { throw 'Target directory must already exist.' }
function Assert-NoReparse([string]$Path) {
    $cursor = $Path
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse point rejected: $cursor" }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}
Assert-NoReparse $Source
Assert-NoReparse $Destination
$Manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'baseline_release_hashes.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$Entries = @()
foreach ($entry in $Manifest.PSObject.Properties) {
    $relative = $entry.Name.Replace('/', '\')
    $sourcePath = [IO.Path]::GetFullPath((Join-Path $Source $relative))
    $destPath = [IO.Path]::GetFullPath((Join-Path $Destination $relative))
    if (!$sourcePath.StartsWith($Source + '\', [StringComparison]::OrdinalIgnoreCase) -or !$destPath.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escaped release root.' }
    Assert-NoReparse $sourcePath
    Assert-NoReparse $destPath
    if ((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw "Baseline hash mismatch: $relative" }
    $Entries += [PSCustomObject]@{Source=$sourcePath; Destination=$destPath; Hash=$entry.Value}
}
foreach ($entry in $Entries) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($entry.Destination)) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Source -Destination $entry.Destination -Force
    if ((Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Hash) { throw "Restored hash mismatch: $($entry.Destination)" }
}
Write-Output 'ROLLBACK_RESTORED=PASS; new_databases_preserved=true'
