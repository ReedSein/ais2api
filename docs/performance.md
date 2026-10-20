# Offline replay performance tool

`ais2api_replay` is an optional developer executable. It connects a synthetic `GenerationBackend` to `serve_with_backend`, then drives the real listener, authentication, scheduler, event relay, JSON encoding and loopback HTTP stack. The production `ais2api` executable has no synthetic-generation switch. The replay executable is not installed or included in the production package.

Build with `-DAIS2API_BUILD_REPLAY=ON`, then build the `ais2api_replay` target. The option defaults to OFF. On Windows the resulting executable is normally `build/windows/ais2api_replay.exe`; the executable name has no `.exe` suffix on Linux/Termux.

```text
ais2api_replay --seconds 120
ais2api_replay --seconds 7200
```

The default duration is 120 seconds; 7200 selects a two-hour stability run. A run stops admitting new benchmark requests at its deadline and drains the outstanding requests. Reported duration includes that drain, so it can exceed the requested duration. A short successful run does not establish a two-hour stability result.

Ctrl+C requests the embedded server's orderly shutdown. Clients stop when that server exits, outstanding sockets drain or time out, and the report marks `server_stopped_early: true` with a nonzero exit code. This is an interrupted run, not a completed stability qualification.

## Fixed workload

Every run uses four clients, each opening a new loopback TCP connection for each request and waiting 100 ms after completion. This caps connection creation at forty per second independent of backend speed; observed throughput is workload throughput, not maximum server capacity. Clients submit authenticated Gemini SSE requests to a synthetic model. The backend emits eight content events with 1024 ASCII text bytes each, followed by one finish/usage event, without artificial upstream delay. Every successful response must decode to exactly those nine events and 8192 content bytes with a STOP finish reason.

The server uses a queue capacity of eight, a 4096-byte relay window, a 65536-byte object limit and ten-second queue/generation/idle limits. The client receive timeout is ten seconds. Fixture config, SQLite state and generated API keys exist only in a uniquely named temporary directory; keys remain private and no real Google cookies are imported. The fixture is stopped and removed after the run. No Chromium process, browser preparation, external media fetch or Google request is part of this workload.

This version uses only fixed Gemini SSE requests. It does not mix unary responses, OpenAI formatting, cancelled requests, large images, multi-account rotation or browser failure recovery. Those need separate workloads. Browser and upstream status are explicitly unverified in the report.

New connections also exercise the OS TCP port/TIME_WAIT policy; high-rate or long-run errors can reflect host socket-resource limits rather than generation-code capacity. Keep connection policy identical in comparisons.

## JSON report

The report is one JSON object on stdout with `scope: "offline_backend_replay"`. Server lifecycle diagnostics may appear on stderr. Fields include requested/observed duration, completed attempts/successes/errors, successful requests per second, p50/p95/p99 end-to-end request latency, backend/process CPU seconds, resident-memory first/last/sampled peak, and sampled generation-queue peak. `error_classes` reports bounded counters by system error category/code or application error code, without request bodies or exception messages.

Latency uses a fixed 512-bin logarithmic histogram. Percentiles are upper bucket boundaries with a 1.05 ratio (approximately five-percent quantization), calculated over successful requests. There is no unbounded per-request latency vector that could manufacture a memory slope during a long run. Null percentiles mean no successful samples.

`backend_cpu_seconds` measures CPU consumed by the generation worker thread while it constructs and pushes the synthetic events: GetThreadTimes on Windows, CLOCK_THREAD_CPUTIME_ID on Linux/Termux. It includes backend-side event-relay work but excludes time sleeping or blocked and excludes work on HTTP/client threads. `process_cpu_seconds` covers the entire colocated benchmark process, including HTTP clients and sampling, using GetProcessTimes on Windows and std::clock on Linux/Termux. Neither is a measure of browser CPU.

RSS is the current process working set from GetProcessMemoryInfo on Windows and resident pages from `/proc/self/statm` times the system page size on Linux/Termux. First and last samples are taken with the fixture still alive. A 100-ms sampler captures RSS and queries the real local control channel for queue depth. These are **sampled** peaks, not guaranteed instantaneous maxima. `sample_errors` counts failed samples and should be zero for a qualified run. Sampling and HTTP clients themselves consume CPU and memory, so the numbers are not isolated server resource use.

Exit code 0 means the run completed without counted request/sample errors or a failed server thread. It is not a universal performance pass threshold. Nonzero exit means an interruption, setup/runtime failure, request errors or failed sampling; keep the JSON report and investigate. Reports label `browser: null`, `browser_verified: false`, `google_verified: false`, and `original_project_comparison: null` to prevent conflating this synthetic pipeline with BrowserGateway performance or a comparison against the original project.

## Interpreting results

Record the commit, compiler/build type, OS/architecture, CPU, memory and tool command alongside each report. Compare identical workloads on the same machine, account for warmup and allocator caching, and inspect both error counts and latency before comparing throughput. A first-to-last RSS increase alone does not prove a leak; a bounded long-run plateau and repeated controlled runs are more informative. This tool has no built-in numerical release threshold and makes no claim about browser memory, Google generation latency, real cookie health or the original Python implementation.
