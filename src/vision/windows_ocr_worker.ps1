param([switch]$Session, [switch]$OverlappedStdin, [ValidateRange(100, 3600000)][int]$IdleTimeoutMs = 30000)
# Data-only OCR. One-shot remains the default protocol for existing callers.
# -Session explicitly opts into a bounded NDJSON request loop. All pixels stay
# in named paging-file-backed memory; the helper never writes image files.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$runtimeInitialized = $false
$engines = @{}
$asTask = $null
$sessionId = ''
$seenRequests = [Collections.Generic.HashSet[string]]::new()
$reader = $null
if ($Session) {
    # QProcess redirects stdin through an overlapped Windows pipe. Console's
    # standard-input wrapper assumes synchronous I/O and can stall after the
    # first line. The coordinator explicitly declares the handle mode; do not
    # guess it by FileStream construction (sync handles may fail on later read).
    if ($OverlappedStdin) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class RelinkOcrStdio {
    [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int id);
}
"@
    $inputHandle = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new([RelinkOcrStdio]::GetStdHandle(-10), $false)
    $inputStream = [IO.FileStream]::new($inputHandle, [IO.FileAccess]::Read, 4096, $true)
    } else { $inputStream = [Console]::OpenStandardInput() }
    $reader = [IO.StreamReader]::new($inputStream, [Text.UTF8Encoding]::new($false), $false, 4096)
}
try {
    while ($true) {
        if ($Session) {
            # Pipe ReadLineAsync keeps idle sessions finite without a polling
            # loop or a visible PowerShell window. EOF is the normal shutdown.
            $pendingRead = $reader.ReadLineAsync()
            if (!$pendingRead.Wait($IdleTimeoutMs)) { break }
            $inputLine = $pendingRead.GetAwaiter().GetResult()
            if ($null -eq $inputLine) { break }
        } else {
            $inputLine = [Console]::ReadLine()
        }
        $requestClock = [Diagnostics.Stopwatch]::StartNew()
        $requestId = ''
        $frameId = ''
        $mappingName = ''
        $bitmap = $null
        $mapping = $null
        $view = $null
        $exitCode = 0
        $fatalSessionError = $false
        $released = $false
        $timing = [ordered]@{runtime_init_ms=0; engine_init_ms=0; bitmap_copy_ms=0; recognize_ms=0;
            helper_request_ms=0; engine_cache_hit=$false; engine_created=$false; engine_create_count=$engines.Count}
        try {
            if (!$inputLine -or $inputLine.Length -gt 4096) { throw 'E_OCR_WORKER_INPUT' }
            $envelope = $inputLine | ConvertFrom-Json
            if ($Session) {
                if ($envelope.protocol -cne 'windows-ocr-session-v1' -or
                    $envelope.session_id -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' -or
                    $envelope.frame_id -isnot [string] -or !$envelope.frame_id -or $envelope.frame_id.Length -gt 256 -or
                    $envelope.frame_id.Contains([char]0) -or !$envelope.request) { throw 'E_OCR_WORKER_INPUT' }
                if ($sessionId -and $sessionId -cne $envelope.session_id) { throw 'E_OCR_WORKER_INPUT' }
                $sessionId = [string]$envelope.session_id
                $frameId = [string]$envelope.frame_id
                $request = $envelope.request
            } else { $request = $envelope }
            $requestId = [string]$request.request_id
            $width = [int]$request.width
            $height = [int]$request.height
            $length = [long]$width * $height * 4
            $mappingName = [string]$request.mapping_name
            if ($request.protocol -cne 'windows-ocr-once-v1' -or
                $requestId -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' -or
                $mappingName -cne "Local\RelinkVision_${requestId}_0" -or
                $width -lt 1 -or $width -gt 8192 -or $height -lt 1 -or $height -gt 8192 -or
                $length -gt 134217728 -or $request.bytes -ne $length -or
                $request.language -cnotin @('zh-Hans-CN', 'en-US')) { throw 'E_OCR_WORKER_INPUT' }
            if ($Session -and (!$seenRequests.Add($requestId) -or $seenRequests.Count -gt 4096)) { throw 'E_OCR_WORKER_INPUT' }
            $stage = [Diagnostics.Stopwatch]::StartNew()
            if (!$runtimeInitialized) {
                Add-Type -AssemblyName System.Runtime.WindowsRuntime
                $null = [Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]
                $null = [Windows.Graphics.Imaging.SoftwareBitmap,Windows.Foundation,ContentType=WindowsRuntime]
                $null = [Windows.Globalization.Language,Windows.Globalization,ContentType=WindowsRuntime]
                $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
                    $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
                    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
                } | Select-Object -First 1
                $asTask = $asTask.MakeGenericMethod([Windows.Media.Ocr.OcrResult])
                $runtimeInitialized = $true
            }
            $timing.runtime_init_ms = $stage.Elapsed.TotalMilliseconds
            if ($width -gt [Windows.Media.Ocr.OcrEngine]::MaxImageDimension -or
                $height -gt [Windows.Media.Ocr.OcrEngine]::MaxImageDimension) { throw 'E_OCR_IMAGE_SIZE' }
            $stage.Restart()
            $languageTag = [string]$request.language
            $timing.engine_cache_hit = $engines.ContainsKey($languageTag)
            if (!$timing.engine_cache_hit) {
                $language = [Windows.Globalization.Language]::new($languageTag)
                $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($language)
                if (!$engine) { throw 'E_OCR_LANGUAGE' }
                $engines[$languageTag] = $engine
                $timing.engine_created = $true
            }
            $engine = $engines[$languageTag]
            $timing.engine_init_ms = $stage.Elapsed.TotalMilliseconds
            $timing.engine_create_count = $engines.Count
            $stage.Restart()
            $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
                $mappingName, [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
            $view = $mapping.CreateViewStream(0, $length, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
            $bytes = [byte[]]::new($length)
            $position = 0
            while ($position -lt $bytes.Length) {
                $read = $view.Read($bytes, $position, $bytes.Length - $position)
                if ($read -le 0) { throw 'E_OCR_WORKER_INPUT' }
                $position += $read
            }
            $buffer = [System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions]::AsBuffer($bytes)
            $bitmap = [Windows.Graphics.Imaging.SoftwareBitmap]::new(
                [Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8, $width, $height, [Windows.Graphics.Imaging.BitmapAlphaMode]::Ignore)
            $bitmap.CopyFromBuffer($buffer)
            $timing.bitmap_copy_ms = $stage.Elapsed.TotalMilliseconds
            $stage.Restart()
            $operation = $engine.RecognizeAsync($bitmap)
            $task = $asTask.Invoke($null, @($operation))
            $result = $task.GetAwaiter().GetResult()
            $timing.recognize_ms = $stage.Elapsed.TotalMilliseconds
            $words = [Collections.Generic.List[object]]::new()
            foreach ($lineResult in $result.Lines) {
                foreach ($word in $lineResult.Words) {
                    if ($words.Count -ge 2000) { throw 'E_OCR_TOKEN_LIMIT' }
                    $bounds = $word.BoundingRect
                    $words.Add([ordered]@{text=$word.Text; x=$bounds.X; y=$bounds.Y; width=$bounds.Width; height=$bounds.Height})
                }
            }
            $angle = $null
            if ($null -ne $result.TextAngle) { $angle = [double]$result.TextAngle }
            $reply = [ordered]@{protocol='windows-ocr-once-v1'; request_id=$requestId; ok=$true;
                width=$width; height=$height; language=$engine.RecognizerLanguage.LanguageTag;
                coordinate_space='windows_ocr_rotated'; text_angle=$angle; words=@($words.ToArray())}
        } catch {
            $code = $_.Exception.Message
            if ($code -cnotin @('E_OCR_WORKER_INPUT','E_OCR_IMAGE_SIZE','E_OCR_LANGUAGE','E_OCR_TOKEN_LIMIT')) { $code = 'E_OCR_ENGINE' }
            $reply = [ordered]@{protocol='windows-ocr-once-v1'; request_id=$requestId; ok=$false; error=$code}
            $exitCode = 1
            $fatalSessionError = $code -ceq 'E_OCR_WORKER_INPUT'
        } finally {
            # The release acknowledgement is emitted only after all borrowed
            # memory and image handles have actually been disposed.
            try {
                if ($view) { $view.Dispose(); $view = $null }
                if ($mapping) { $mapping.Dispose(); $mapping = $null }
                if ($bitmap -is [IDisposable]) { $bitmap.Dispose(); $bitmap = $null }
                $buffer = $null
                $bytes = $null
                $released = $true
            } catch {
                $released = $false
                $fatalSessionError = $true
                $exitCode = 1
            }
        }
        $timing.helper_request_ms = $requestClock.Elapsed.TotalMilliseconds
        if ($Session) {
            $reply = [ordered]@{protocol='windows-ocr-session-v1'; session_id=$sessionId;
                request_id=$requestId; frame_id=$frameId; mapping_name=$mappingName;
                frame_released=$released; result=$reply; timing=$timing}
        }
        [Console]::WriteLine(($reply | ConvertTo-Json -Depth 8 -Compress))
        [Console]::Out.Flush()
        if (!$Session) { exit $exitCode }
        if ($fatalSessionError) { exit 1 }
    }
} finally {
    if ($reader) { $reader.Dispose() }
    foreach ($engine in $engines.Values) { if ($engine -is [IDisposable]) { $engine.Dispose() } }
}
exit 0
