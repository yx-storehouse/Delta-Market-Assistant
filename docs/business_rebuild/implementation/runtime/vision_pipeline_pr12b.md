# PR12B synthetic vision pipeline integration

## Scope

`tests/runtime/vision_pipeline_tests.cpp` exercises the non-game boundary as a
single in-memory pipeline:

```text
InMemoryReplaySource
  -> ObservationAdapter
  -> LeasePool (write/publish/in-use/release)
  -> WorkerProtocol coordinator
  -> NDJSON framer
  -> WorkerProtocol worker
  -> result/cancel/release terminal flow
```

The test deliberately does not launch a process, create an OS shared-memory
mapping, access a window, capture a desktop surface, invoke OCR/OpenCV, send
input, or write image files. The shared-memory descriptor is protocol metadata;
the accepted BGRA8 bytes are copied into bounded `QByteArray` test slots.

## Scenarios

| Scenario | Assertions |
| --- | --- |
| successful one-shot | hello/ready NDJSON handshake, accepted frame, exact request/frame/context/lease correlation, result, release, adapter and pool drain |
| cancel-before-result | cancellation acknowledgement, late result rejected as `E_CANCELLED`, lease retained until release, source cancellation called once |
| stale frame | `E_STALE_OR_UNPROVEN_FRAME` before protocol dispatch; no request, lease, slot bytes, or worker history is created |
| oversized metadata | dimensions are checked during replay materialization before pixel allocation; capture returns `E_FRAME_SHAPE`/`E_SHM_BOUNDS`, no protocol or lease activity |
| bounded-watch final frame | second request waits for first release, slot generation/lease identity changes, close remains draining until terminal release, adapter/pool resources are drained |

Wrong viewport context, wrong frame identity, wrong slot generation, wrong lease
generation, and pre-terminal release are intentionally attempted and rejected.
The test also checks that a stale or malformed frame cannot increment worker
history. No ledger, business executor or transaction counter is instantiated by
this test; it makes no assertion about nonexistent counters.

## Running

The root CMake file registers `vision_pipeline_tests` with the message fixture
directory:

```powershell
cmake --build build_relocated --target vision_pipeline_tests -j 4
build_relocated\vision_pipeline_tests.exe docs\business_rebuild\implementation\runtime\fixtures\messages
```

Observed output for the current fixture set:

```text
VISION_PIPELINE_TESTS=PASS; assertions=279; failures=0
```

This is the final synthetic integration gate before a real WGC/DXGI capture or
OCR backend is considered. Any real-game validation remains a separate,
explicitly paused boundary.
