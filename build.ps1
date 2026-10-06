param([switch]$Test, [switch]$Package)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Qt = Join-Path $Root '.tools\Qt\6.8.3\mingw_64'
$Compiler = Join-Path $Root '.tools\Qt\Tools\mingw1310_64\bin'
$CMake = Join-Path $Root '.tools\aqt-env\Scripts\cmake.exe'
$env:PATH = "$Compiler;$Qt\bin;$Root\.tools\aqt-env\Scripts;$env:PATH"
$BuildDir = Join-Path $Root 'build'
$Cache = Join-Path $BuildDir 'CMakeCache.txt'
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
        Copy-Item -LiteralPath (Join-Path $BuildDir 'RelinkStudio.exe') -Destination $Dest -Force
        & (Join-Path $Qt 'bin\windeployqt.exe') --release --no-translations --no-opengl-sw --no-system-d3d-compiler --compiler-runtime --skip-plugin-types generic,networkinformation,tls --dir $Dest (Join-Path $Dest 'RelinkStudio.exe')
        if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
        New-Item -ItemType Directory -Path (Join-Path $Dest 'platforms') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $Qt 'plugins\platforms\qoffscreen.dll') -Destination (Join-Path $Dest 'platforms') -Force
        # QSQLITE is a runtime-loaded plugin: deploy it explicitly, not only
        # whatever windeployqt discovers from import tables.
        New-Item -ItemType Directory -Path (Join-Path $Dest 'sqldrivers') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $Qt 'bin\Qt6Sql.dll') -Destination $Dest -Force
        Copy-Item -LiteralPath (Join-Path $Qt 'plugins\sqldrivers\qsqlite.dll') -Destination (Join-Path $Dest 'sqldrivers') -Force
        # Prevent a packaged app from silently finding plugins in the build SDK.
        [System.IO.File]::WriteAllText((Join-Path $Dest 'qt.conf'), "[Paths]`nPrefix=.`nPlugins=.`n", [System.Text.UTF8Encoding]::new($false))
        foreach ($dll in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
            Copy-Item -LiteralPath (Join-Path $Compiler $dll) -Destination $Dest -Force
        }
        $SavedPath = $env:PATH
        $SavedPluginPath = $env:QT_PLUGIN_PATH
        $SavedPlatformPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
        try {
            $env:PATH = "$Dest;$env:SystemRoot\System32;$env:SystemRoot"
            $env:QT_PLUGIN_PATH = $Dest
            $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $Dest 'platforms'
            & (Join-Path $Dest 'RelinkStudio.exe') --storage-self-test
            if ($LASTEXITCODE -ne 0) { throw 'Packaged QSQLITE persistence self-test failed.' }
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
        foreach ($name in @('STORE_UI.md','WIN11_DARK_UI.md','FLUENT_UI.md','TERMINAL_UI.md')) {
            $source = Join-Path (Join-Path $Root 'docs') $name
            if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $ReleaseDocs -Force }
        }
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
