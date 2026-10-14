# GLMetal validation, October 9, 2026

The converter defaults to GLMetal again with the tested shader, command-stream,
and upload fixes. GLMetal contains generic renderer changes. Sound Manager
queue ownership and pause handling remain in the compatibility loader.

The [October 14 shared runtime measurements](SHARED-RUNTIME-PERFORMANCE.md)
cover per-control once initialization and generic vertex layout reuse. The
final vertex changes passed 150 Apple GL comparisons on each architecture.
The isolated alternating-format benchmark improved about 14%; gameplay FPS
and elimination of area-transition stutters remain unproven.

## MW3 steady-state index publication

The [render performance investigation](MW3_RENDER_PERFORMANCE.md) found
40.6% of the sampled render-thread time inside per-draw index `glBufferData`
publication, including 26.7% in new Metal buffer allocation. The bridge now
appends ranges to an orphaned index arena; GLMetal also caches immutable
sampler reflection. The compiler identity and shader results are unchanged.

Production-code offscreen probes passed queued draws and arena wraps with
zero bad pixels on both architectures. Rosetta index-workload submission
time decreased from 352.563 to 212.107 ms, 39.8%, for 131,072 draws. This is
an isolated workload measurement, not a gameplay FPS claim. Fifty focused
Apple GL rendering comparisons and native buffer checks also passed.

## MW3 upfront shader preparation

The [shader preparation experiment](MW3_SHADER_PRECOMPILE.md) extracted
bytecode inputs for 4,991 of 5,034 known recipes and captured 3,281 exact
program requests. The new generic serial CLI compiled every request and
6,562 Metal stage libraries without failure. A repeat hit every program
cache. All 7,328 generated cache files matched game-generated files exactly.

A muted normal-assets Berlin checkpoint run requested 317 unique programs.
316 matched the prepared set; one was new. The first isolated CPU replay
confirmed those 316 cache hits. Large program bursts took 23 ms of
application-thread compiler work each, while whole frames still took 377
and 474 ms. This does not establish complete coverage or stutter-free play.
The prototype warms libraries, not every render pipeline descriptor. The
original game's shader assets and all 243 original profile files stayed
unchanged. Game extraction remains outside the generic GLMetal submodule.

## MW3 first-use shader stalls

The live MW3 trace recorded frames of 1,145, 841, 514 and 1,279 ms. The
1,279 ms frame linked 89 programs and created 91 pipelines. Application-thread
program compilation took 576 ms, pipeline creation 559 ms, and native shader
waits 96 ms. These categories overlap, particularly native waits and program
linking, so their totals must not be added as independent wall time.

The worker's reported 2 ms of GLSL compilation was a memory-cache replay of
the application thread's result. It did not mean the cold compiler was cheap.
Matrix-array interface preparation searched an entire shader with a regular
expression whenever any identifier contained `mat`. Names such as `cinematic`
triggered that scan even without matrix declarations. The compiler now requires
an exact supported matrix-type token before scanning declarations.

A CPU-only comparison checked 20 generated pairs and 20 captured MW2 proxy
pairs, each compiled 20 times. Prepared GLSL and serialized results,
including MSL and reflection, matched byte-for-byte for all 40 pairs. On
arm64, total uncached compilation decreased from 5,231.32 to 4,146.74 ms,
20.7%, and preparation decreased from 1,848.03 to 693.662 ms, 62.5%.
These are compiler measurements, not an FPS improvement or proof that every
multi-second gameplay stall is eliminated. An additional x86_64 run confirmed
identical results; concurrent user gameplay makes its timings unsuitable for
a performance ranking. The proxy result does not establish that MW3's area
shaders are 20.7% faster.

The checkpoint run then captured 280 actual MW3 program pairs. CPU profiling
attributed most compilation time to glslang parsing and SPIRV-Cross MSL
generation. Texture rewrites also constructed full reflection for vertex
stages without images and stages without cube images. Two instruction-type
guards now skip those rewrites when their required image type is absent.
Malformed or truncated instructions retain the original path.

Alternating trials of all 280 pairs reduced the texture-rewrite pass by about
25 ms per pass through the workload. Total compilation improved about 2%,
with overlapping measurement variation. The complete baseline comparison
checked the 280 MW3 pairs and 27 generated fixtures, including matrix arrays,
double precision, boolean uniforms, 2D/cube/shadow/rectangle textures and
malformed scanner boundaries. All 307 prepared-source and serialized-result
pairs matched exactly. This is a modest first-use optimization.

The final driver also matched Apple GL exactly in 124 focused cases on each
of arm64 and x86_64, 248 comparisons total. These checks disabled the disk
shader cache and covered cube and rectangle sampling, varying-name linking
and legacy GLSL. No case failed or crashed.

Private muted checkpoint runs with disk shader caches disabled reproduced
two 89-program bursts. Baseline frames took 2,676 and 2,375 ms; a matrix-guard
build took 1,812 and 2,161 ms. The user was running a separate game during
these tests, and native Metal caches warmed between runs, so these observations
cannot isolate the compiler change or establish an FPS ranking. They confirm
that large uncached linking bursts remain. Experimental helper QoS changes
and a double-type shortcut did not show reliable improvements and are excluded.

The build retains the previous compiler cache identity only for the exact
verified source/dependency fingerprint of this equivalent optimization. A
further compiler or dependency edit receives a new identity. This preserves
existing `.prog` and `.ok` caches without accepting arbitrary older compiler
results. The exact alias and subsequent-edit invalidation were checked.

An experimental persistent Metal pipeline archive was also measured with
three fresh stable-entry cohorts of 24 captured pipeline pairs. Average cold
creation was 450.1 ms without harvesting and 470.4 ms with harvesting. Warm
archive hits averaged 8.3 ms; the same warm identities with archives disabled
also averaged 8.3 ms. Harvesting added 75 to 199 ms of background compilation.
The archive experiment is excluded from the driver because these observations
show no benefit over Apple's native warm cache.

The final build also ran the copied checkpoint with the normal disk cache
enabled. The two 89-program bursts took 680 and 552 ms, including 48 and
59 ms of application-thread compilation and 115 and 62 ms of pipeline
creation. The run reached the Berlin helicopter sequence and observed it
for 20 seconds. No frame exceeded one second and no crash occurred before
deliberate termination. Later hitches remained, including a 348 ms frame
with a 251 ms native compiler wait. Some hitches contained little measured
compiler work and need separate investigation. This cached run and the
uncached runs measure different conditions; their difference must not be
attributed to the approximately 2% compiler change.

All 243 original MW3 profile files retained their recorded sizes and SHA256
hashes after the private tests. The six local single-player/multiplayer game
bundles and three standalone converters now contain the final driver and
compiler helper. Their signatures and build-manifest hashes were verified.
Published release assets were not changed.

## Rendering

The complete offscreen suite ran sequentially on Apple GL and GLMetal with
100 ms between cases. GLMetal passed all 1,033 case assertions. Images matched
exactly in 1,027 cases. Four legacy wide-line cases differ by one green-channel
value at three pixels each. The older driver reproduces those same differences.
Two remaining cases test invalid floating-point texture swizzles: Apple accepts
the invalid settings and fails the assertions; GLMetal rejects them and passes.
These differences remain visible in the results.

The focused 150-case suite also passed and matched on arm64 and x86_64. Final
x86_64 checks covered 14 client-vertex cases, four pooled-upload cases, and four
ordered texture-query cases, with 22 exact matches. CPU shadow-border compiler
fixtures and all 20 generated offline Metal stages passed.

## Gameplay

An M5 Pro ran COD4's `airlift` scene at 1920 by 1200 with a private profile,
muted audio, and uncapped frames. Each observation covered 60 seconds after
gameplay began, using frame counters logged every 60 frames. There was one run
per condition, so the averages are approximate and do not establish results for
other missions or games.

| Provider | Average FPS | Recorded native shader waits |
| --- | ---: | ---: |
| Apple GL | 57.8 | Not instrumented |
| GLMetal, comment-salted sources | 77.7 | 615 ms during startup |
| GLMetal, warm cache and pooled uploads | 74.8 | 0 ms |

The latest run had no crash. A cutscene transition still took 335 ms, primarily
in guest event and movie handling. The comparable Apple transition took 424 ms.
This does not establish that all crashes or stutters are fixed. The previous
multi-second draw-time integer-input and alpha-test shader variants are removed.

Those observations used the background test path. Comment-only salting does
not establish cold downstream Metal pipelines, so the 77.7 FPS run must not
be treated as evidence that normal play is smooth with a cold cache.

## COD4 release and compiler scheduling investigation

The last published COD4 converter, `cod4-converter-v0.0.0-dev.3`, used Apple
OpenGL. Comparison with that tag found no removed COD4 profile patches, focus
policy or AGL presentation optimization. The pacing and append-ring changes
associated with MW2/MW3 were not active COD4 optimizations in that release.

The private COD4 `killhouse` reproduction used copied user settings, muted
audio, a real fullscreen window and one game process at a time. Fresh native
function entry names replaced comment-only salting. Native function compilation
used the same source and limits in both conditions; only scheduling priority
changed. Focus changed during these observations, so their FPS averages do
not establish a foreground performance ranking.

| Compiler priority | First gameplay frame | Pipeline builds in that frame | Native shader waits in that frame |
| --- | ---: | ---: | ---: |
| Utility | 11,304 ms | 2,723 ms | 7,690 ms |
| Utility, repeated | 11,054 ms | 2,835 ms | 7,408 ms |
| UserInitiated | 3,661 ms | 2,768 ms | 0 ms |
| UserInitiated, repeated | 3,926 ms | 2,996 ms | 0 ms |

Across recorded slow frames, native waits were 11,427 and 9,576 ms with Utility
priority, and 33 and 162 ms with UserInitiated. This supports raising the two
bounded prewarm queues and shader-to-pipeline notifications to UserInitiated.
The native shader queue still permits four jobs; the pipeline queue permits two.
It does not fix the remaining 2.7 to 3.0 seconds of first-use pipeline creation
or the separate cutscene and focus-handling stalls.

A bounded ready-pair retry experiment did not help this COD4 scene. ARB uploads
contained separate stages, so it produced no pipeline predictions. That
experiment is excluded from the final driver.

The final driver passed 147 fresh-identity rendering cases on each of arm64
and x86_64, with 294 exact Apple image matches. These include ARB lifetime and
alpha changes, integer vertex fetch, clip planes, depth clamp, sample shading
and shadow border comparison. Pipeline prediction CPU tests passed on both
architectures after removing the retry experiment.

## Audio converter lifetime

One Utility baseline run ended in native `AudioConverterNew`, where Caulk's
allocator trapped on zeroed free-block metadata. The earlier driver baseline
contains the same trap. The writer of that metadata is not established.

The converter bridge had a separate concrete race: it returned an unprotected
native converter while another thread could dispose it. Fill and Reset now
serialize per converter. Dispose retires the handle before waiting for active
work and rejects duplicate disposal. Callback self-disposal waits until the
outer Fill returns; recursive Fill or Reset rejects unsafe reentry. Independent
converters remain concurrent.

Deterministic PCM-only tests passed for blocked conversion callbacks, queued
Reset, concurrent disposal, independent converters and callback self-disposal,
alongside 16,384 variable-size resampling fills. They produce no device audio.
The lifetime fix alone does not prove the Caulk crash is eliminated.

With the final loader and normal shader caching, a 60-second `killhouse`
observation averaged 60.6 FPS with GLMetal and 54.8 FPS with Apple GL. Neither
run crashed. GLMetal's first gameplay frame was 825 ms, including 77 ms of
pipeline creation and no recorded native shader wait. The private profile,
resolution and loader were the same. Focus changed during the observations;
one run per provider does not establish overall superiority or stable frame
times. Audio playback stayed muted, so these observations do not establish
audible playback quality.

An offscreen CGL parameter probe also checked a suspected vsync mismatch.
Both Apple GL and GLMetal returned a default swap interval of 1 for legacy and
core contexts, with single and double buffering. Apple returned the same values
under arm64 and x86_64. Although the [archived Apple guide](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_contexts/opengl_contexts.html)
describes a default of 0, the current platform's measured default is 1. No
default-interval change was made.

## Audio and packaging

Muted native PCM tests cover decoding, ordered callbacks, gapless queueing,
completed-buffer reuse, teardown, and running playback followed by flush, pause,
and enqueue. Returned buffers remain owned by AudioQueue and are recycled.
Delayed completion jobs validate queue epochs before accessing buffer metadata.
Reset preserves the native queue's running state so a later pause reaches it.

All six installed compatibility apps passed signature checks after refresh.
The three converter apps were rebuilt. Packaging tests and relocated SP/MP
conversion checks passed for COD4, MW2, and MW3. Tests used copied profiles and
did not change the user's saves or audio settings.

Detailed local results are in `build/regression-20261009/`, including
`validation-summary.json`, full image comparisons, private gameplay logs, and
packaging results. These runtime artifacts are excluded from Git.

## First indexed draw after a flush

A direct element-buffer draw recorded its GPU read before `prepare_draw` opened
the new Metal command buffer. The read could inherit the previous submission
serial and look completed. An overlapping `glBufferSubData` or mapped write
then overwrote indices still needed by the queued draw. Read tracking now runs
after command-buffer creation, immediately before the indexed draw. Copied and
rewritten index streams retain their existing ownership.

The two new `core_buffer_index_read_after_flush` variants failed before the fix,
each losing a red triangle at 676 pixels. Both match Apple GL exactly after the
fix on arm64 and x86_64. The new ordinary and invariant multipass depth cases
also match on both architectures. A focused x86_64 run covered 26 buffer, depth,
client-index, index-type and primitive-restart cases with exact image matches.
These checks establish the buffer fix, not the cause of MW2 cave lighting.

Local evidence is under `build/mw2-flicker-20261009/`, including the before/after
index reports, `regression-*` reports and `multipass-final-arm64/`.

## Deferred pipeline preparation and shared uploads, October 10

GLMetal now retries recently skipped function pairs once their render recipe
becomes confident. Retention and retry work are bounded; descriptor matching
and compiler concurrency are unchanged. CPU mock tests pass on arm64 and
Rosetta, including stale pairs, wrong targets, final-function variants and
demand claiming queued work. A native compiler-only probe with twelve captured
MW3 pairs reduces first-draw waits from 216 to 231 ms to 8 to 14 ms across two trials
per architecture. This demonstrates preparation overlap, not gameplay FPS.

Immediately submitted shared-context texture uploads use payload-sized pooled
staging rather than a separate 4 MB transient reservation. Buffer lifetime and
submission ordering are unchanged. A new shared-context texture lifetime case,
four existing staging cases and fourteen depth-clamp/variant cases match Apple
GL on each architecture, 38 comparisons with no mismatches or crashes.

The compatibility bridge also feeds MW2/MW3 SDL flushes into the bounded hitch
recorder. The recorder CPU test and a muted private MW3 presentation check pass;
the latter generated a 41-frame report. The packaged native buffer check passes.
Details and limits are in `MW3_RENDER_PERFORMANCE.md`; local evidence is under
`build/mw3-area-20261010/`. Shader output and compiler cache identity are unchanged.

## CPU buffer readback and completed-query reuse, October 10

CPU-authored buffer reads still synchronize preceding command-worker uploads,
but skip GPU completion unless the buffer has a recorded GPU writer. Completed
32-bit query getters reuse the exact result established by a real worker getter.
Mutation, deletion, indexed queries and display-list state refresh invalidate it.
First polls and GPU-produced buffer reads retain necessary synchronization.

Thirty-eight focused Apple GL comparisons pass across arm64 and x86_64, including
CPU-upload/GPU-output readback, index publication and query lifecycle cases.
CPU probes verify the absence of the unnecessary wait, exact query saturation,
reuse invalidation and error handling. Query AddressSanitizer probes pass on both
architectures. The recorder regression covers presentation thread handoffs,
late timestamps and repeated sessions. Per-thread counters replace shared frame
counters that contaminated prior live category totals.

The packaged muted MW3 integration and native buffer check results are recorded
with the other local evidence in `build/mw3-followup-20261010/`. These checks do
not measure gameplay FPS or establish that every reported stall is fixed.
See `MW3_RENDER_PERFORMANCE.md` for the measured calls and remaining limits.
