# Python-only release update: copy the collection runtime and docs into dist\RelinkStudio and
# rewrite file_manifest.json exactly like build.ps1 -Package, without rebuilding or replacing the EXE.
# Afterwards end the standby runner (python ... run_collection_hotkey.py --standby); the next F2 reloads it.
$ErrorActionPreference = 'Stop'
$Root = 'C:\Users\Administrator\Desktop\price'
$Dest = Join-Path $Root 'dist\RelinkStudio'
$ReleaseDocs = Join-Path $Dest 'docs'
Copy-Item -LiteralPath (Join-Path $Root 'docs\COLLECTION_HOTKEY.md') -Destination $ReleaseDocs -Force
Copy-Item -LiteralPath (Join-Path $Root 'docs\PURCHASE_READONLY.md') -Destination $ReleaseDocs -Force
$CollectionPython = Join-Path $Root '.tools\ocr-runtime\Scripts\python.exe'
& $CollectionPython -B -X utf8 (Join-Path $Root 'tests\release\package_collection_runtime.py') --root $Root --destination $Dest
if ($LASTEXITCODE -ne 0) { throw 'Collection runtime deployment failed.' }
$ManifestPath = Join-Path $Dest 'file_manifest.json'
$Files = [ordered]@{}
Get-ChildItem -LiteralPath $Dest -Recurse -File | Sort-Object FullName | ForEach-Object {
    if ($_.FullName -ne $ManifestPath) {
        $relative = $_.FullName.Substring($Dest.Length + 1).Replace('\','/')
        $Files[$relative] = [ordered]@{ bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
}
$Version = [regex]::Match((Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw), 'project\(RelinkStudio VERSION ([0-9.]+)').Groups[1].Value
$Manifest = [ordered]@{ version=$Version; build='Windows-x64-Qt6.8.3-MinGW13.1'; files=$Files } | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText($ManifestPath, $Manifest, (New-Object System.Text.UTF8Encoding($false)))
Write-Output "PACKAGE=PASS; path=$Dest"
