# Single-request, hidden Windows OCR reader. Pixels enter via paging-file-backed
# read-only shared memory, not an image path. The script emits one UTF-8 JSON.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$requestId = ''
$bitmap = $null
$mapping = $null
$view = $null
$exitCode = 0
try {
    $line = [Console]::ReadLine()
    if (!$line -or $line.Length -gt 4096) { throw 'E_OCR_WORKER_INPUT' }
    $request = $line | ConvertFrom-Json
    $requestId = [string]$request.request_id
    $width = [int]$request.width
    $height = [int]$request.height
    $length = [long]$width * $height * 4
    if ($request.protocol -cne 'windows-ocr-once-v1' -or
        $requestId -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' -or
        $request.mapping_name -cne "Local\RelinkVision_${requestId}_0" -or
        $width -lt 1 -or $width -gt 8192 -or $height -lt 1 -or $height -gt 8192 -or
        $length -gt 134217728 -or $request.bytes -ne $length -or
        $request.language -cnotin @('zh-Hans-CN', 'en-US')) { throw 'E_OCR_WORKER_INPUT' }
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $null = [Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]
    $null = [Windows.Graphics.Imaging.SoftwareBitmap,Windows.Foundation,ContentType=WindowsRuntime]
    $null = [Windows.Globalization.Language,Windows.Globalization,ContentType=WindowsRuntime]
    if ($width -gt [Windows.Media.Ocr.OcrEngine]::MaxImageDimension -or
        $height -gt [Windows.Media.Ocr.OcrEngine]::MaxImageDimension) { throw 'E_OCR_IMAGE_SIZE' }
    $language = [Windows.Globalization.Language]::new([string]$request.language)
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($language)
    if (!$engine) { throw 'E_OCR_LANGUAGE' }
    $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
        [string]$request.mapping_name, [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
    $view = $mapping.CreateViewStream(0, $length, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
    $bytes = [byte[]]::new($length)
    $position = 0
    while ($position -lt $bytes.Length) {
        $read = $view.Read($bytes, $position, $bytes.Length - $position)
        if ($read -le 0) { throw 'E_OCR_WORKER_INPUT' }
        $position += $read
    }
    # DataWriter.DetachBuffer is an untyped COM object in Windows PowerShell 5;
    # the managed AsBuffer projection preserves the IBuffer type for WinRT.
    $buffer = [System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions]::AsBuffer($bytes)
    $bitmap = [Windows.Graphics.Imaging.SoftwareBitmap]::new(
        [Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8, $width, $height, [Windows.Graphics.Imaging.BitmapAlphaMode]::Ignore)
    $bitmap.CopyFromBuffer($buffer)
    $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    } | Select-Object -First 1
    $operation = $engine.RecognizeAsync($bitmap)
    $task = $asTask.MakeGenericMethod([Windows.Media.Ocr.OcrResult]).Invoke($null, @($operation))
    $result = $task.GetAwaiter().GetResult()
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
} finally {
    if ($view) { $view.Dispose() }
    if ($mapping) { $mapping.Dispose() }
    # WinRT wrappers expose IDisposable via .NET projection.
    if ($bitmap -is [IDisposable]) { $bitmap.Dispose() }
}
[Console]::WriteLine(($reply | ConvertTo-Json -Depth 6 -Compress))
exit $exitCode
