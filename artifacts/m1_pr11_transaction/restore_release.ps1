param([Parameter(Mandatory=$true)][string]$Target)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$Source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'baseline\release'))
$Destination = [IO.Path]::GetFullPath($Target).TrimEnd('\')
$Allowed = @([IO.Path]::GetFullPath((Join-Path $Root 'dist\RelinkStudio')), [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'rollback_test')))
if ($Allowed -notcontains $Destination) { throw 'Target must be the fixed release directory or the isolated rollback_test directory.' }
if (!(Test-Path -LiteralPath $Destination -PathType Container)) { throw 'Target directory must already exist.' }
if ((Get-Item -LiteralPath $Destination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Target must not be a reparse point.' }
$ManifestPath = Join-Path $PSScriptRoot 'baseline_release_hashes.json'
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $Manifest.PSObject.Properties) {
    $relative = $entry.Name.Replace('/', '\')
    $sourcePath = [IO.Path]::GetFullPath((Join-Path $Source $relative))
    $destPath = [IO.Path]::GetFullPath((Join-Path $Destination $relative))
    if (!$sourcePath.StartsWith($Source + '\', [StringComparison]::OrdinalIgnoreCase) -or !$destPath.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escaped the release directory.' }
    if ((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw "Baseline hash mismatch: $relative" }
    $parent = [IO.Path]::GetDirectoryName($destPath)
    $cursor = $parent
    while ($cursor.Length -ge $Destination.Length) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Destination has a reparse point.' }
        if ($cursor -eq $Destination) { break }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    if (Test-Path -LiteralPath $destPath) {
        if ((Get-Item -LiteralPath $destPath).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Destination file must not be a reparse point.' }
    }
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destPath -Force
    if ((Get-FileHash -LiteralPath $destPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw "Restored hash mismatch: $relative" }
}
Write-Output 'ROLLBACK_RESTORED=PASS; new_databases_preserved=true'
