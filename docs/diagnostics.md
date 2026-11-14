# Diagnostics

Core Log flushes live records. Client: instance `logs/client.log`, including captured stdout/stderr. Launcher: application-root `logs/launcher.log`. Dedicated Server defaults to console. Close retains the plain log and timestamped GZip archive; startup preserves unarchived output.

DEBUG `maage.launcher.startup` reports monotonic durations for Core/log init, minimal Launcher init, native show/map, networking, account restoration/verification, catalog, resources and first Play readiness. Native show/map is not compositor/display latency. See [AGENTS.md](../AGENTS.md) for levels, filtering, traces and archive rules.

## Native Fault Records

`maage_log_init` installs supported crash hooks and a separate append handle. Emergency writing bypasses normal locks, streams, formatting, heap and filters. Bounded FATAL/Core/Stop records contain OS/architecture, image base, process, signal/exception, fault address and available PC/SP/FP/ARM64 LR; macOS may include image UUID, Windows thread ID. Symbolicate with matching binary/symbols, OS report and ASLR adjustment.

No saving, stack walking, cleanup or recovery in fault handlers. Capture is best effort: SIGKILL, forced termination, CRT fast-fail, replaced hooks or destroyed stacks can prevent it. Ordinary Status failures use causal traces and controlled shutdown. Platform references: [POSIX signals](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/sigaction.2.html), [Windows exception filters](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter).

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
