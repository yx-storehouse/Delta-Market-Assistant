# Vision transport implementation (PR12B)

Updated: 2026-10-07 (Asia/Shanghai)

## Scope and boundary

`relink_vision_transport` is an isolated Windows transport layer. It is deliberately not linked into `RelinkStudio`; the desktop product remains synthetic/replay-only. The layer now verifies two generic plumbing pieces:

- `SharedFrameMemory` / `ReadOnlyFrameMemory`: a Win32 paging-file-backed named mapping with bounded byte ranges and read-only descriptor validation.
- `WorkerProcess`: bounded NDJSON transport over a hidden `QProcess`, with explicit absolute executable and working-directory inputs, timeout/deadline handling, output limits, cancellation, release and exit/quarantine behavior.

The child used by the tests is `vision_worker_fixture.exe`. It is a deterministic synthetic protocol peer, not an OCR model, not a game helper, and not a production worker. It is built outside the desktop package and is explicitly rejected by package-manifest tests if copied into the release directory.

No WGC/DXGI game-window capture, OCR/OpenCV/ONNX provider, keyboard/mouse input, market request, purchase, or transaction is performed here. Pixel payloads are synthetic bytes held in memory; no image file or screenshot path is used.

## Module boundaries

| Target | Responsibility | Product status |
|---|---|---|
| `relink_runtime` | Observation demand, replay source, protocol/lease state models | Linked by desktop application |
| `relink_vision_transport` | Win32 mapping and hidden child process transport | Test-only static library, not linked to `RelinkStudio` |
| `shared_frame_memory_tests` | Mapping names, slot/session binding, descriptor and byte bounds, reader/owner lifecycle | 65 assertions, CTest target |
| `worker_process_tests` | Hidden child handshake, round-trip result/release, cancellation, deadlines, malformed/flooded output, stderr cap and quarantine | 206 assertions, CTest target |
| `vision_worker_fixture` | Synthetic protocol peer with normal/cancel/stall/malformed/EOF/flood modes | Test executable only; never shipped |

## Shared-frame lifecycle

The owner creates `Local\RelinkVision_<session>_<slot>` with slot 0 or 1 and an explicit capacity. `write()` accepts only a non-empty range fully inside that capacity. `descriptor()` publishes transport, mapping, lease ID, slot, generation, offset, length and capacity. The reader validates every descriptor against the negotiated two-slot pool before opening a mapping and copies only the declared byte range.

The implementation rejects invalid session names, cross-session or cross-slot descriptors, duplicate mappings, unsafe generations, fractional/string numeric fields, file transport, missing fields, capacity overflow and incomplete negotiated pools. Owner and reader reopen operations require explicit close. A live reader prevents mapping-name reuse until it closes.

`LeasePool` remains the lifecycle authority: a mapping is not considered free merely because a protocol message was acknowledged. Cancellation, terminal result, release, process exit confirmation and mapping close remain separate transitions.

## Hidden synthetic worker lifecycle

`WorkerProcess::start()` requires an absolute existing executable and absolute working directory. Tests supply only the synthetic fixture executable, with its explicit fixture marker and a mode argument; the transport never searches PATH for a worker. The process is created hidden on Windows. Standard output is incrementally NDJSON-framed, standard error is capped at 64 KiB, pending messages are bounded, and protocol/deadline errors are latched rather than reinterpreted.

Covered fixture modes:

- `normal`: hello/ready, recognize, result, release-frame, clean shutdown/bye.
- `cancel`: cancellation acknowledgement, deliberately late result rejection, then release.
- `request_stall`: deadline, quarantine, forced stop, confirmed process exit and mapping close.
- `session_error_stall`: session error while a request is active; the error remains latched and the child is explicitly stopped.
- `stall`, `malformed`, `partial`, `eof`, `stdout_flood`, `stderr`: bounded handshake/error behavior.

The test records the hash of the fixture child for evidence and verifies that no same-named child exists in the fixed desktop release directory. A passing child transport test proves only generic process/memory plumbing; it does not prove OCR quality or game-page recognition.

## Reproduction

From the project root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
build_relocated\shared_frame_memory_tests.exe
build_relocated\worker_process_tests.exe `
  (Resolve-Path .\build_relocated\vision_worker_fixture.exe).Path `
  (Resolve-Path .\docs\business_rebuild\implementation\runtime\fixtures\messages).Path
```

The release harness repeats both targets with package-only Qt paths and the absolute fixture path. It also runs the 41-case tiny-fixture negative package suite. Those negative cases exercise harness rejection paths and do not replace final package acceptance. The fixture directory and child are test inputs, not release assets. Final package hashes, offscreen results and rollback status are recorded separately in the transaction and delivery reports after source freeze.

## Next boundary

The next implementation boundary is the real visual business: selecting the game window, obtaining client/physical pixels, handling DPI/occlusion/minimize/recreation and viewport generations, defining product/listing/price/wear ROIs, and calibrating an OCR provider with real samples and rejection thresholds. Pause for user summary when that boundary requires a real game window or real game frames; continue all generic replay, storage, protocol and transport maintenance before then.
