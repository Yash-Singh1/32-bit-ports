# Shared runtime and vertex setup, October 14, 2026

This change repairs overhead and synchronization in our compatibility code.
It adds no game binary patches. Tests of MW3 use `LP32_NO_CODE_PATCHES=1`,
private copies of the checkpoint and settings, muted audio and background
input. The player's apps, saves and system volume are untouched.

## Guest once initialization

The old `pthread_once` adapter ran every initializer while holding one
recursive global mutex. Unrelated controls blocked each other. It recorded
completion before running the callback, remembered only 256 addresses, and
kept those addresses completed after their storage was reset or reused.

The replacement publishes completion in the opaque word of Darwin's 8-byte
i386 control, preserving its signature. Each incomplete control has its own
mutex and condition variable. Completed calls use an acquire load without
locking. An initializer that traps, exits or is cancelled leaves the control
retryable. Stable per-address metadata remains allocated for the process
lifetime; resetting control storage requires no concurrent callers.
The import dispatcher also supplies a typed fast handler for `_pthread_once`.

`make -C native test-guest-once test-guest-once-abi` checks independent
initializers running concurrently, contended initialization, memory
publication, 4,096 controls, nesting, abort/exit/cancel retries, reset/reuse
and invalid arguments. The ABI fixture calls real i386 import thunks through
both fast and chained dispatch, and returns before graphics or game startup.
The host test also passed AddressSanitizer and UndefinedBehaviorSanitizer.

One Rosetta CPU benchmark measured completed calls as follows. These are
algorithm costs, excluding the guest import round trip.

| Workload | Previous | Current |
| --- | --- | --- |
| One thread, one control | 9.87 ns/call | 5.98 ns/call |
| One thread, 128 controls | 27.65 ns/call | 5.48 ns/call |
| Four threads, 128 controls | 70.23 ns/call | 1.41 ns/call |

The multithreaded number divides elapsed time by all completed calls. It is
aggregate throughput, not one thread's call latency. The benchmark retains
the previous algorithm alongside the production implementation.

## Generic GLMetal vertex setup

Vertex state version numbers now come from one sequence. Previously, each
source file's inline helper owned a separate counter, so changes in different
files could assign the same version and evade cache invalidation.
Repeated array setters no longer change the version when their values match.
At draw time, GLMetal can reuse a layout if the shader's used inputs were
restored to their previous fetch state. Changes to constants remain live.
Buffer/context destruction and input changes still invalidate the cache.

A bounded descriptor cache also avoids repeated NSData allocation, global
locking and dictionary lookup when draws alternate among known formats.
The isolated alternating-format workload improved about 14% in one Rosetta
cohort. Restored-state timing varied. All 150 vertex comparison cases matched
Apple GL on each architecture, 300 comparisons total. See
[the benchmark procedure](glmetal/tests/probes/vertex_layout_bench.md).

## Gameplay limits

The unpatched MW3 Dubai checkpoint profile still contains substantial
translated game and renderer work. The first two cohorts did not establish
an FPS improvement: a 300-frame interval measured 25.2 FPS before changes
and 17.9 FPS with the intermediate build. Separate CPU workloads were active
on the host, and the cohorts differed in loading and playback timing.
These observations require follow-up rather than an FPS claim.
The final descriptor-cache build measured 19.6 FPS in its 300-frame interval,
and a subsequent repeat of the original build measured 16.3 FPS. The original
build's own 25.2-to-16.3 FPS variation prevents ranking these cohorts.
The once bug did not dominate the sampled steady-state scene. Large loading
frames and first-use program/pipeline bursts remain, so this change is not
proof of stutter-free play or grounds to remove existing title patches.

Local test logs, private loader copies, screenshots and CPU profiles are in
`native/build/shared-overhead-20261014/`. That directory is ignored by Git.
The six local COD4/COD4MP, MW2/MW2MP and MW3/MW3MP compatibility apps and all
three standalone converters include the shared loader and driver changes.
Existing game code patches remain enabled for ordinary launches.
