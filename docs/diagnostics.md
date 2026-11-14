# Diagnostics

Core Log flushes live records. Client logs are instance-local `logs/client.log`, including captured stdout/stderr; Launcher logs use application-root `logs/launcher.log`. Dedicated Server output defaults to stdout/stderr unless file logging is selected. Normal close retains the plain log and produces a timestamped GZip archive; startup preserves otherwise unarchived output.

`maage.launcher.startup` emits DEBUG timings for Core application/log initialization, minimal Launcher initialization, native window construction/presentation, network service initialization, account restoration/verification, catalog loading, resource readiness/full verification and first Play availability. These are monotonic phase durations. Window presentation records the native show/map boundary; it is not a compositor or display-latency measurement. No startup measurements are implied by the presence of this instrumentation.

## Native Fault Records

`maage_log_init` installs supported native crash hooks and a separate append handle. The emergency writer bypasses the usual log mutex, streams, formatters, heap allocation and filters. It writes a bounded FATAL/Core/Stop record with OS/architecture, executable image base, process, signal/exception, fault address and available PC/SP/FP/ARM64 LR context. macOS can record image UUID and Windows a thread ID. Consult the matching binary/symbols and OS crash report for full symbolication, accounting for ASLR.

A fault handler does not attempt World saves, stack walking, cleanup or recovery. Capture is best effort: SIGKILL, forced OS termination, CRT fast-fail, supplanted hooks and destroyed stacks may prevent it. Ordinary Status failures use normal causal traces and controlled shutdown. Platform reference: [POSIX signals](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/sigaction.2.html) and [Windows exception filters](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter).

## Development frame profiler

The Client's built-in development frame capture is enabled with `MAAGE_PROFILE=1`.
The Windows Ctrl+B workflow enables it automatically. When launched by the MaaGE
Launcher, the Client inherits this environment variable and creates a uniquely
named `logs/profile-<epoch>-<index>.csv` inside the selected instance. The path
is logged as `maage.client.profiler`. F3 displays the most recent work time,
rolling p95 work time, historical maximum, input, scene, setup, renderer call,
and mesh upload timings. Five-second summaries are written to `client.log`.

Each CSV row contains the raw inter-frame interval, current-loop work duration,
input/UI, scene preparation, renderer setup, renderer API call, upload, residual
unattributed time, server tick, server chunk counts, worker count, mesh uploads,
upload budget, and presentation result. The first and last frame can include
startup/teardown effects. A pause, VSync, OS scheduling or frame cap can affect
the inter-frame interval even when CPU work is short. "Renderer" measures the
calling thread's elapsed time inside the renderer API; it is **not** a GPU
timestamp and does not distinguish GPU execution, driver waits, or presentation
queue blocking. The authoritative integrated/dedicated Server independently records 5-second mean tick breakdowns (gameplay, entity/container work, World tick, publication), peak tick cost and counts exceeding the 50 ms simulation budget in the Client/Server log via `maage.server.profiler`. It warns on the first and every twenty-fifth slow tick. Server sampling does not yet attribute background job durations.\n\nThe profiler is not a CPU sampling profiler and does not identify
individual worker-thread hot functions. The raw CSV is the primary artifact for
comparing regressions; compare identical world/seed, camera path, render distance,
renderer, build type, and capture duration. Capture overhead includes clock
sampling and buffered file output, so comparative runs should use the same mode.
Disk failure disables capture without stopping gameplay.
