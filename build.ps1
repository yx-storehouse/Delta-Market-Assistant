param([switch]$Test, [switch]$Package)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Qt = Join-Path $Root '.tools\Qt\6.8.3\mingw_64'
$Compiler = Join-Path $Root '.tools\Qt\Tools\mingw1310_64\bin'
$CMake = Join-Path $Root '.tools\aqt-env\Scripts\cmake.exe'
$env:PATH = "$Compiler;$Qt\bin;$Root\.tools\aqt-env\Scripts;$env:PATH"
$BuildDir = Join-Path $Root 'build'
$Cache = Join-Path $BuildDir 'CMakeCache.txt'
# An already-open frontend may have unchanged Qt DLLs mapped. Rewriting
# byte-identical dependencies is unnecessary and can fail on Windows locks.
function Copy-ReleaseFile([string]$Source, [string]$Destination) {
    $Target = $Destination
    if (Test-Path -LiteralPath $Destination -PathType Container) { $Target = Join-Path $Destination ([IO.Path]::GetFileName($Source)) }
    if ((Test-Path -LiteralPath $Target -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash)) { return }
    Copy-Item -LiteralPath $Source -Destination $Target -Force
}
if (Test-Path -LiteralPath $Cache) {
    $cacheText = Get-Content -LiteralPath $Cache -Raw -ErrorAction SilentlyContinue
    if ($cacheText -notmatch [regex]::Escape($Root)) {
        $BuildDir = Join-Path $Root 'build_relocated'
    }
}
Push-Location $Root
try {
    & $CMake -S . -B $BuildDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$Qt" '-DCMAKE_CXX_COMPILER=g++'
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    & $CMake --build $BuildDir -j 6
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
    if ($Test) {
        & (Join-Path $Root '.tools\aqt-env\Scripts\ctest.exe') --test-dir $BuildDir --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    if ($Package) {
        $Dest = Join-Path $Root 'dist\RelinkStudio'
        New-Item -ItemType Directory -Path $Dest -Force | Out-Null
        Copy-ReleaseFile (Join-Path $BuildDir 'RelinkStudio.exe') $Dest
        New-Item -ItemType Directory -Path (Join-Path $Dest 'vision') -Force | Out-Null
        Copy-ReleaseFile (Join-Path $BuildDir 'vision\windows_ocr_worker.ps1') (Join-Path $Dest 'vision')
        & (Join-Path $Qt 'bin\windeployqt.exe') --release --no-translations --no-opengl-sw --no-system-d3d-compiler --compiler-runtime --skip-plugin-types generic,networkinformation,tls --dir $Dest (Join-Path $Dest 'RelinkStudio.exe')
        if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
        New-Item -ItemType Directory -Path (Join-Path $Dest 'platforms') -Force | Out-Null
        Copy-ReleaseFile (Join-Path $Qt 'plugins\platforms\qoffscreen.dll') (Join-Path $Dest 'platforms')
        # The collection runner reads the same skin catalogue this exe embeds.
        New-Item -ItemType Directory -Path (Join-Path $Dest 'catalog') -Force | Out-Null
        Copy-ReleaseFile (Join-Path $Root 'src\assets\catalog\skins.json') (Join-Path $Dest 'catalog')
        # QSQLITE is a runtime-loaded plugin: deploy it explicitly, not only
        # whatever windeployqt discovers from import tables.
        New-Item -ItemType Directory -Path (Join-Path $Dest 'sqldrivers') -Force | Out-Null
        Copy-ReleaseFile (Join-Path $Qt 'bin\Qt6Sql.dll') $Dest
        Copy-ReleaseFile (Join-Path $Qt 'plugins\sqldrivers\qsqlite.dll') (Join-Path $Dest 'sqldrivers')
        # Prevent a packaged app from silently finding plugins in the build SDK.
        [System.IO.File]::WriteAllText((Join-Path $Dest 'qt.conf'), "[Paths]`nPrefix=.`nPlugins=.`n", [System.Text.UTF8Encoding]::new($false))
        foreach ($dll in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
            Copy-ReleaseFile (Join-Path $Compiler $dll) $Dest
        }
        $SavedPath = $env:PATH
        $SavedPluginPath = $env:QT_PLUGIN_PATH
        $SavedPlatformPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
        try {
            $env:PATH = "$Dest;$env:SystemRoot\System32;$env:SystemRoot"
            $env:QT_PLUGIN_PATH = $Dest
            $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $Dest 'platforms'
            # A GUI-subsystem process may return before it exits in PowerShell.
            # Wait explicitly and check its own status rather than a stale LASTEXITCODE.
            $Smoke = Start-Process -FilePath (Join-Path $Dest 'RelinkStudio.exe') -ArgumentList '--storage-self-test' -Wait -PassThru -WindowStyle Hidden
            if ($Smoke.ExitCode -ne 0) { throw 'Packaged QSQLITE persistence self-test failed.' }
        } finally {
            $env:PATH = $SavedPath
            $env:QT_PLUGIN_PATH = $SavedPluginPath
            $env:QT_QPA_PLATFORM_PLUGIN_PATH = $SavedPlatformPath
        }
        if (Test-Path -LiteralPath (Join-Path $Root 'README.md')) {
            Copy-Item -LiteralPath (Join-Path $Root 'README.md') -Destination $Dest -Force
        }
        if (Test-Path -LiteralPath (Join-Path $Root 'docs\third_party')) {
            Copy-Item -LiteralPath (Join-Path $Root 'docs\third_party') -Destination $Dest -Recurse -Force
        }
        if (Test-Path -LiteralPath (Join-Path $Root 'docs\FRONTEND_DELIVERY.md')) {
            Copy-Item -LiteralPath (Join-Path $Root 'docs\FRONTEND_DELIVERY.md') -Destination $Dest -Force
        }
        $ReleaseDocs = Join-Path $Dest 'docs'
        New-Item -ItemType Directory -Path $ReleaseDocs -Force | Out-Null
        foreach ($name in @('STORE_UI.md','WIN11_DARK_UI.md','FLUENT_UI.md','TERMINAL_UI.md','REAL_SKIN_CATALOG.md','COLLECTION_TASK_IMPORT.md','COLLECTION_ONLY_TRIAL.md','COLLECTION_ALL_RULES.md','COLLECTION_FULL_CYCLE.md','COLLECTION_SPEED.md','COLLECTION_SCROLL_SPEED.md','COLLECTION_HOTKEY.md','PURCHASE_READONLY.md')) {
            $source = Join-Path (Join-Path $Root 'docs') $name
            if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $ReleaseDocs -Force }
        }
        # Deliver the actual collection CLI and its recursively discovered local
        # imports. The helper only parses Python AST and copies source files; it
        # never imports the application, captures the game, or duplicates models.
        $CollectionPython = Join-Path $Root '.tools\ocr-runtime\Scripts\python.exe'
        if (!(Test-Path -LiteralPath $CollectionPython -PathType Leaf)) { throw 'Collection Python runtime is missing.' }
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
    }
} finally { Pop-Location }
