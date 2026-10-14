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

## First vertex-only gameplay cohorts

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

## Shared frame-cost follow-up

The next pass removes CPU work from the bridge and driver. It adds no game
binary patches. The largest measured change removes the bridge's custom
mutex polling loop. Repeated pause instructions, trylocks and clock reads
under Rosetta consumed CPU on waiting threads and delayed the lock owner.
The bridge now calls the system mutex implementation directly, without a
title-specific locking policy. The condition, semaphore and mutex lifecycle
selftest passed against the MW3 image.

Common Add32 and CAS32 imports now execute locked i386 instructions in the
copied guest helper page. Static stubs, nonlazy pointers, dynamic callbacks
and dlsym use the same implementation. Diagnostic flags restore the host
gateway. The explicit guest execution lock also restores gateway calls so
polling loops retain handoff opportunities. `make -C native test-pure-atomic`
checks seven configurations, signed and overflow returns, CAS booleans,
registers, live x87 values, FP control, mixed host/guest increments and
publication ordering. Actual imported Add32 calls measured about 2 ns
inline versus 65 ns through the gateway in one CPU fixture.

The default hitch recorder still counts every GL state call, but times one
in 32. It labels sampled time and sampled calls separately and does not
extrapolate them. Unmeasured work remains in frame work or an enclosing
scope. Audited scalar, TLS, atomic, clock and cached trylock calls retain
counts without per-call clocks. Cold dispatch and mutex allocation stay
visible. Draws, uploads, waits, allocation, shader work and audio keep their
timing. `LP32_HITCH_FULL_IMPORTS=1` retains exact per-import timing. Default
and full-mode recorder tests passed.

GLMetal caches successful texture storage and sampling validation decisions.
It skips identical inline Metal byte bindings while tracking transitions
between inline bytes and buffers. Its worker publishes progress at batch
completion or an exact pending request boundary instead of after every
command. Shared framebuffer image definitions and storage changes invalidate
open passes across ordered contexts. Redundant framebuffer setters can then
preserve passes safely. Audited local setters only advance the draw-state
serial after actual changes; pending query synchronization, shadow hooks
and display-list replay remain intact.

The texture, inline binding, framebuffer and generated local setter CPU
probes pass on both architectures under UBSan. The worker payload and request
boundary regression passes under ASan and UBSan on both. See
[the CPU probe procedures](glmetal/tests/probes/stream_cpu.md).

## Repeated MW3 checkpoint comparison

These runs used a private muted copy of `sp_dubai-1`, background SDL input,
1920 by 1200 rendering, uncapped presentation and `LP32_NO_CODE_PATCHES=1`.
The host was an M5 Pro with 64 GB RAM and 18 logical CPUs. Other user workloads
remained running. Each measured interval contained 60 frames. Initial 180
gameplay frames were excluded; the FPS figure combines equal frame-count
intervals with their harmonic mean.

| Build | Intervals | Average FPS | Largest frame in selected intervals |
| --- | ---: | ---: | ---: |
| Archived baseline, first run | 18 | 20.9 | 230.5 ms |
| Archived baseline, repeat | 18 | 20.5 | 239.6 ms |
| Updated bridge and driver with old mutex polling | 29 | 30.7 | 178.9 ms |
| Same build with polling disabled | 48 | 50.0 | 170.3 ms |
| Final default build with audited local setters | 47 | 49.6 | 149.7 ms |

The final measured checkpoint is about 2.4 times the repeated baseline.
This is a checkpoint comparison, not a claim about every mission, other
titles or performance relative to an old laptop. Large initial loading
frames remain. A private 12-second forward-input check followed by 23 seconds of observation
averaged 52.3 FPS after input, with a largest reported frame of 77.4 ms.
SDL accepted both key transitions and the draw workload changed. This
short movement check supplements the standing comparison; mission-wide
stutter elimination still requires longer playthrough evidence.

The first relevant rendering selection matched Apple GL in 646 comparisons
across arm64 and x86_64. After the local setter change, 540 of 542 selected
comparisons matched. The remaining case on each architecture is an existing
wide-line color discrepancy of one color level at three pixels. Archived
and updated drivers show the same discrepancy. The complete eight-variant
wide-line output also matches between old and new drivers on x86_64.
No tolerance was relaxed. Shared attachment replacement and inline
byte-to-buffer transitions have dedicated GPU cases that match Apple GL.

Logs, private loader/driver copies, CPU profiles and image comparisons are
in `native/build/frame-cost-20261014/`, ignored by Git. The normal local apps
and converters use the production loader; the copied diagnostic SDL helper
used for automated checkpoint input is not packaged.
