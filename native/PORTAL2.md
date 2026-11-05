# Portal 2 development

Portal 2 support is experimental. The supplied Mac application renders the
main menu, including its background video and text, and loads the opening
single-player relaxation room with its tutorial prompts. The game also exits
cleanly. Windowed and fullscreen startup render correctly; fullscreen has
also been checked after losing and regaining focus. The user reports that
the game launches and runs correctly. Sustained gameplay remains unverified.
The current work lives on the `portal2` branch in a separate checkout. No game
binaries or data are tracked by Git.

## Native Intel Macs

The source loader supports native x86_64 execution as well as Rosetta 2.
Native execution needs flat user data/stack selectors before entering i386
code, including when returning from a bridged import; Rosetta does not enforce
that requirement. `make` builds `build/mode32_probe`, which checks a real i386
round trip on either host.

On dual-GPU Macs, an enumerated renderer can reject the game's legacy pixel
format even though another GPU supports it. The Portal 2 Cocoa bridge first
tries the original attributes. If that fails with an explicit renderer ID, it
retries without that ID, preserving every other format requirement. Run
`make test-pixel-format` to exercise an unavailable renderer, a real usable GL
context, zero-valued attributes, and an unsatisfiable format that must still
fail. This test needs access to the macOS graphical session.

Validated on a MacBookPro16,1 with an Intel Core i7 and AMD Radeon Pro 5300M,
macOS 26.7.1 (25G241). A locally patched dev.6 app rendered the menu, loaded a
saved single-player level, and was played and then quit by its owner. This is
a short gameplay check, not a full playthrough or online co-op validation.
The published dev.6 converter predates these fixes; build from source to use
them until an updated converter is released.

## Building and running

Opening a converted app now checks Steam before starting the game. If Steam
is closed or not ready, the launcher opens Steam and waits up to 60 seconds
for initialization, then continues with the converted executable. This uses
the supplied Steam API's native slice in an isolated helper when available;
the game's own Steam initialization still checks authentication and ownership.
Older Steam libraries without a native slice use a process-readiness check
and leave API validation to the game.

Steam's standard AppID relaunch selects the original executable installed in
its library, so this launcher opens the client itself and keeps the converted
app selected. If startup still fails, a native dialog explains how to open
Steam and sign in, with Open Steam and Quit buttons. The known “Steam is not
running” engine error also shows this dialog and exits normally before the
engine's deliberate crash instruction. Other fatal errors retain crash reports.
`LP32_HEADLESS_ERRORS=1` disables startup UI and opening Steam for automated
tests. `make -C native test-steam-startup` checks readiness, automatic opening,
sign-in waits, failure, and bounded timeouts without changing a real Steam session.

From `native/`:

```sh
make portal2-runtime
make GAME=portal2 SOURCE_APP="/Volumes/Storage/games/mac/Portal 2.app" bundle
open build/Portal2-Compat.app
```

The same target also accepts a flat Steam installation or a parent containing
DepotDownloader's `depots/621/<version>` and `depots/623/<version>`:

```sh
make GAME=portal2 SOURCE_GAME="/Volumes/Storage/games/mac/portal2" bundle
```

`SOURCE_APP` remains supported. `SOURCE_GAME` can also name `Portal 2.app`,
`steamapps/common/Portal 2`, or the Steam library folder. Both Make and the
converter use `converter/Source.swift` to discover and combine the source
files. Depot 621 supplies content and `portal2_osx`; depot 623 supplies the Mac
libraries. `.DepotDownloader` staging files are excluded. No input files are
modified. Make stages a fresh game tree, retains the existing output's saves
and local options, and swaps directories so old libraries cannot survive a
release change. GUI conversion retains its numbered, separate output behavior.

The supplied **23973718** depot build passes conversion, signing, and real
launcher/dependency loading. With Steam signed in, the converted app loads
`sp_a1_intro1` and renders the opening relaxation room and LOOK UP tutorial
prompt. A 90-second background test completed without a crash, and the game's
framebuffer was inspected. This is a startup/rendering check; sustained
playthrough, co-op, Workshop, and a full controller playthrough remain unverified. Steam must be running and signed in to an account
that owns Portal 2. `steam_bridge.c` connects the original i386 Steam API library
to the installed x86_64 Steam client using guest interface proxies and libffi.
It preserves Steam's authentication and ownership requirements. Both the older
SteamClient017 initialization facade and SteamClient020 interfaces are needed.

The bridge metadata records virtual-method order from Valve's matching SDK
headers, including private slots that are absent from the SDK JSON. Regenerate
`src/steam_abi.inc` with `tools/generate_steam_abi.py` using SDK 1.53a's JSON and
headers, plus `isteamclient017.h` from SDK 1.32 and `isteaminput005.h` from SDK 1.52.
These inputs are needed only when regenerating metadata, not when building or
using the converter. Unsupported interface/argument layouts fail explicitly;
this is not a complete Steamworks implementation. In particular, callback
payloads containing native pointers and application-supplied C++ callback
objects need additional translation. Unregistered private Steam callbacks are
released without being delivered to the game. The loader retains its own crash
handler because Steam's native Breakpad cannot unwind the mixed-mode stacks.

Steam controller polling supports the packed digital, analog, and motion
results on SteamInput005, SteamInput006, and SteamController008. Analog and
motion results use i386 hidden output pointers with callee stack cleanup;
digital results use the two bytes in EAX. Input action event callbacks copy
their packed records into guest memory and can re-enter the bridge from a
native callback thread. Portal uses Steam Input for controllers; the absence
of a LEGO-style legacy HID profile is expected.

Controller glyph PNG decoding also needs `zlib_bridge.c`: the original
56-byte i386 `z_stream` cannot be passed to native zlib. The bridge retains
native stream state outside the guest record, translates counters/pointers,
and invokes the game's allocator callbacks in i386 mode. It implements all
nine zlib imports present in the supplied game libraries, including CRC,
inflate/deflate initialization, streaming, reset, and disposal.

The Steam ABI audit and remaining unsupported interfaces are recorded in
[STEAM_ABI.md](STEAM_ABI.md). `make test-steam-abi` checks real i386 calls,
packed results, callback re-entry, guarded pointer outputs, Workshop tags,
and zlib streaming with both native and guest allocators.

Other depot startup fixes include `bin/osx32` library search, matching duplicate
guest Cocoa class layouts, 8,192 dynamic import slots on separate i386 code
pages, the newer audio-device query, and the client's wide-string parsing
calls. `LP32_CAPTURE_FRAME` now also captures Source's own OpenGL presenter. If CoreGraphics reports no active
displays while AppKit still lists connected screens, Portal 2 can enumerate
those screens instead of dereferencing an empty adapter list.

`portal2-runtime` downloads Apple's 4.72 GB
[Lion archive](https://support.apple.com/en-us/106383), verifies its checksum,
and extracts only the i386 C++ runtime libraries. It never runs the installer
or package scripts. The archive is cached in `build/runtime-downloads` and
the resulting libraries in `build/guest-runtime`. For an existing download:

```sh
python3 tools/prepare_portal2_runtime.py --installer /path/to/InstallMacOSX.dmg
```

The bundle copies the game's approximately 11 GB of data to
`Contents/SharedSupport/Portal2`. The original application is only read.
Use `make GAME=portal2 promote-loader` to update the loader without copying
the game data again. A direct diagnostic launch accepts Source arguments:

```sh
arch -x86_64 build/Portal2-Compat.app/Contents/MacOS/Portal2Compat \
  -windowed -w 1280 -h 720 -novid
```

For fullscreen, choose **Options → Video → Display Mode → Full Screen**, then
**Apply**. Resolution is selected in the same menu. These are the game's
display controls; its fixed-size window does not expose macOS's green zoom
button. To override a saved windowed setting for one launch:

```sh
arch -x86_64 build/Portal2-Compat.app/Contents/MacOS/Portal2Compat -fullscreen -novid
```

The loader skips Portal 2's startup movies by supplying `-novid` on every
launch, including Finder and Dock launches. Bink videos opened by the game,
including the Community Test Chambers introduction, now bridge legacy
Sound Manager PCM playback and ordered completion callbacks to Core Audio.
Their asynchronous file loading uses native Time Manager tasks with converted
i386 records and callbacks. Previously the introduction exited on the
unimplemented `NewSndCallBackUPP` import, after Steam had initialized normally.
The supplied `intro_movie.bik` has passed two open/decode/play/close cycles
through the original Bink library, in addition to synthetic callback-order,
pause/resume, cancellation and timer tests. Workshop map downloads and play
remain unverified. The engine's other audio uses the existing AudioQueue bridge.

Launching normally uses the saved video settings. Avoid `-windowed` when
testing fullscreen. The development bundle is currently saved at fullscreen
1920×1200; that local preference is not part of the source or copied to other
installations.

Diagnostics are written to `~/Library/Logs/Portal2Compat/last-run.log`.
`LP32_TRACE_DYLD_INITIALIZERS=1` prints every guest library constructor.
`LP32_TRACE_DYLD_ERRORS=1` also prints failed library and symbol lookups,
including the engine's optional debug-library probes.
`LP32_TRACE_FONT=1` reports rejected ATSUI calls. `LP32_TRACE_DATES=1` traces
Cocoa date factories and guest autorelease pools.
`LP32_TRACE_EVENTS=1` traces Cocoa event delivery and keyboard callbacks.
`LP32_BACKGROUND_TEST=1` suppresses the compatibility layer's focus requests
during unattended tests and keeps its cursor released. Portal's inactive event
pump becomes nonblocking in this mode so background waiting does not distort
rendering measurements. `LP32_USE_SECONDARY_DISPLAY=1` selects the other monitor
when available; `LP32_DISPLAY_INDEX` selects a specific zero-based screen.

To play on the other display with a 120 FPS limit and an on-screen FPS counter,
quit the game first, then launch from `native/`:

```sh
LP32_USE_SECONDARY_DISPLAY=1 arch -x86_64 \
  build/Portal2-Compat.app/Contents/MacOS/Portal2Compat \
  -fullscreen +fps_max 120 +cl_showfps 1
```

The cap is an upper limit, independent of the display's refresh rate. This
selects the monitor without changing the saved rendering resolution. Add
`+load quick` to resume the latest quicksave. These launch options apply to
this session; a normal Finder launch continues to select the primary display.

`LP32_PORTAL_FRAME_STATS=1` reports presentation rate, mean/p50/p95/p99 frame
times, buffer-swap time and draw counts every 240 frames. `LP32_FRAME_STATS=1`
also measures imports, with more profiling overhead. For repeatable comparisons,
load the same quicksave, allow loading/shader compilation to finish, and keep
resolution and foreground/background mode the same. The diagnostic switches
`LP32_NO_BUFFER_CACHE=1` and `LP32_NO_CGL_CONTEXT_CACHE=1` restore the old lookup
paths for comparison. Unattended mode is for measurement; omit it when playing.

At the saved early chamber checkpoint, the September 5, 2026 comparison on
the secondary monitor measured about **15.6 FPS before and 42–43 FPS after**
the context/buffer caches, at unchanged 1920×1200 settings and about 710 draw
calls per frame. Typical 240-frame-window p95 times improved from about 66 ms
to 27 ms. Loading/shader-compilation windows were excluded and both runs used
the same unattended mode.

The second optimization pass retains a known buffer size across same-size
`glBufferData` orphaning. On success the size is unchanged; rejected parameters
leave the old store intact. A requested size change still invalidates the cache.
This removes the synchronous size query from streaming buffers each frame.
Two runs at the same checkpoint measured **54.9 and 52.7 FPS**, versus a new
baseline of **42.5 FPS**. Typical window p95 times fell from 27.3 ms to
21.6–23.5 ms. Rendering settings were unchanged, the cap was raised to 300
for measurement, and import profiling was off. Results and raw windows are
retained locally in `analysis/optimization2-results.json`.

Direct handlers for `memcmp`, `pthread_getspecific`, `pthread_setspecific`
and `pthread_self` were also tested. They reduced CPU dispatch work but
delivered about the same 54 FPS with worse p95 times (29.1–29.3 ms), so that
experiment was removed from the shipping loader. Its patch and TLS/memory
regression checks remain in `analysis/optimization2-direct-import-experiment.patch`.
These measurements describe this scene and machine, not every chamber. The
mean frame time at that stage was still above the 16.7 ms budget for 60 FPS.

### Mapped uploads and shader dispatch

The third pass replaces the `glBufferSubData` upload used by shadow mappings
with a native `glMapBuffer`/copy/explicit-flush/unmap sequence. The guest still
writes to its low-address staging allocation. The native mapping follows the
buffer's serialization and flush policies; it avoids the Metal-backed OpenGL
driver's repeated render-pass breaks and GPU copies for streaming writes.
Only validated mapped ranges use this path. Ordinary `glBufferSubData` calls
keep their existing behavior, and failed native mappings fall back to it.

Shader constant updates and program binding also use direct bridge handlers
for their core and ARB names, avoiding repeated import-name searches. Both
changes are Portal-only. `LP32_NO_MAPPED_UPLOAD=1` and `LP32_NO_FAST_GLSL=1`
disable them independently for same-binary comparisons.

At the same heavy checkpoint on September 5, the final comparison used
1920×1200 on the Retina display, a 300 FPS cap, 708–709 draws per frame, and
six complete 240-frame gameplay windows per run. The camera stayed still,
loading windows were excluded, import profiling was off, and background
waiting was disabled in every run. Roblox was temporarily suspended for these
three runs and resumed afterward. Desktop apps remained available.

| Configuration | FPS | Median of window p95 frame times |
| --- | ---: | ---: |
| Previous upload/dispatch paths, repaired x87 JIT | 51.7 | 31.5 ms |
| Mapped uploads/direct shader dispatch, repaired x87 JIT | 73.0 | 16.0 ms |
| Mapped uploads/direct shader dispatch, stock Rosetta | 72.3 | 15.3 ms |

The complete change improves this comparison by about **41%**, without changing
resolution or graphics quality. Earlier mapped-upload repeats measured
70.2–70.8 FPS while Roblox was running. Splitting uploads into 32/60 KiB chunks
reduced p95 times but stayed near 53 FPS; that experiment was removed. Disabling
the GL worker queue through Source's `-glmdisablemtgl -glmdisablemtgl2` options
regressed mapped uploads to 54.1 FPS, so normal threading is retained.

The GL buffer self-test covers partial and large uploads, preserved bytes,
explicit and implicit flushes, shared contexts, and buffer-size changes. An
offscreen pixel test checks queued draws during serialized overwrites,
nonoverlapping unsynchronized updates, and orphaning. Both the new upload path
and its fallback pass. The GLSL bridge's rendered-pixel test also passes.
Run the buffer test from `native/` with:

```sh
LP32_GL_BUFFER_SELFTEST=1 arch -x86_64 build/game_loader \
  build/Portal2-Compat.app/Contents/SharedSupport/Portal2.image
```

Raw windows and experiment notes are retained locally in
`analysis/upload-results.json` and `analysis/upload-*.log`. These measurements
apply to this checkpoint and machine; they do not imply a steady 120 FPS.

### Rosetta x87 investigation

The experimental [rosettax87_jit](https://github.com/Lifeisawful/rosettax87_jit)
hooks Rosetta's translator; its flags do not affect an ordinary Rosetta launch.
Revision `a44f1eff28591a90f4a8c7297134294811da06b5` was built under `analysis/`
on macOS 26.5.2, with Rosetta version `0x16f0240000000`.

The initial arithmetic failure was **register corruption during translation**,
not an incorrect implementation of addition. An x87 calculation stored directly
to memory produced `3.0`, but passing a double into a function translated for
the first time changed its bits to `0x0706050403020100`. Passing an ordinary
SSE constant reproduced it too. Preserving guest SIMD registers around the
translation hook fixed that failure. The decoder hook had a related issue,
which crashed AppKit's window-frame image conversion; it needs the same
protection. A further freestanding build issue made compiler-generated
`memset`/`bzero` implementations recurse indefinitely. Disabling those builtin
transformations removed the startup hang.

The complete fix is preserved in
[patches/rosettax87-preserve-simd.patch](patches/rosettax87-preserve-simd.patch),
with pinned build and launch instructions in [patches/README.md](patches/README.md).
The corrected runtime passes 11 upstream suites covering arithmetic, rounding,
control words, comparisons, conditional moves, constants and stack behavior.
Our four i386 dynamic-linker/ABI fixture combinations also pass through it.
A real 32-bit code-segment x87 probe and a cold-call SIMD preservation check
pass. [tests/test_rosettax87_cold.c](tests/test_rosettax87_cold.c) preserves the
minimal regression. Portal also loads and renders the heavy checkpoint with
the full decoder and JIT enabled.

On this M5 Pro, the upstream addition microbenchmarks improved by **29–61×**
with default optimization settings. That does not predict a game's overall
speedup: x87 is only part of the frame's work. Two stationary Portal runs at
the same heavy checkpoint measured **50.8 and 49.4 FPS**, versus **53.9 FPS**
for the initial stock run. A later stationary stock repeat measured **45.0 FPS**.
Settings stayed at 1920×1200 on the Retina display. Typical 240-frame-window p95
times were 30.0–31.8 ms with the JIT, 22.9 ms in the initial stock run and
30.5 ms in the later stock run. A comparison in which the user moved and fired
portals was excluded entirely. Roblox was also using about one CPU core during
the final stock repeat. Concurrent work was not controlled, so these measurements
**do not establish a net game speedup or regression**.
The game measurements use a 300 FPS cap, unattended mode, and no import profiling;
loading windows are excluded. CPU sampling happens after the measured windows.

Before the mapped-upload optimization, the JIT-run sample spent about 23% of
the main-thread samples waiting
for the OpenGL command queue during `glBufferSubData`, which identifies buffer
upload synchronization as a bottleneck. After addressing it, the controlled
comparison above measured 73.0 FPS with the repaired JIT and 72.3 FPS with stock
Rosetta. This small difference does not establish a useful additional x87 gain
for this scene. Normal app launches retain stock Rosetta; the patched JIT remains an
explicit experimental launch option. Precision/rounding tradeoff flags remain
disabled, and no system security settings were changed. Raw results and logs
are retained locally in `analysis/x87-results.json` and `analysis/x87-*.log`.

## Coexistence with LEGO

The engines can share the loader, memory allocator, mode-switch gateway and
OS bridges. Portal 2 adds a game profile and a guest dynamic linker; it does
not require removing the Pirates or Clone Wars profiles. Their executable
patches and controller layouts remain selected per title.

The important differences are:

- Portal 2's small universal i386 launcher opens Source libraries at runtime.
  `guest_dyld.c` maps these below the guest stacks, relocates and binds them,
  registers legacy Objective-C classes, and runs dependency constructors.
  Both classic relocations and compressed dyld information are supported.
- Guest library handles and symbols refer to i386 code. System calls cross
  the existing host bridge. Loaded libraries stay mapped because callbacks
  can retain code pointers after `dlclose`.
- Valve's Cocoa subclasses need guest instance storage and native callback
  adapters. The older Objective-C structure-return convention and Source's
  pthread argument convention are selected only for Portal 2.
- Cocoa windows expose stable guest handles for Source's remaining Carbon
  window calls. Showing, minimizing, restoring, focusing and invalidating the
  game window run on the AppKit thread. This prevents fullscreen focus changes
  from trapping on `ShowWindow`/`CollapseWindow`. `GetGlobalMouse` converts the
  native mouse position to Carbon's four-byte, top-left-origin `Point`.
- Portal 2's guest cursor visibility uses a balanced hide count, since the
  deprecated global query can disagree while another app has focus. Extra
  show requests cannot underflow that count. The bridge owns at most one
  native hide and applies capture/hiding only while Portal's game window is
  key and its application is active. AppKit focus loss releases the mouse
  immediately; background hide/capture requests only update guest intent,
  and background cursor warps are suppressed. Returning to Portal reapplies
  its current intent, so a menu stays uncaptured. Source retains control of
  pausing and menus. `LP32_TRACE_DISPLAY=1` reports focus, requested/effective
  cursor state, and suppressed background warps.
- Repeated `CGLGetCurrentContext` calls reuse the guest handle for the native
  context on that thread, avoiding temporary Cocoa objects and proxy-table
  searches on each call. Portal's buffer bridge tracks bindings per context,
  caches immutable share-group identity, and caches buffer sizes after querying
  them once. Buffer reallocation and
  deletion invalidate that state. This avoids draining Apple's threaded GL
  queue at every map, partial flush and unmap; uploads and rendering settings
  retain their existing behavior. This follows Apple's guidance to
  [keep copies of repeatedly queried GL state](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_designstrategies/opengl_designstrategies.html).
- The old C++ stream ABI needs more than the LEGO runtime's small string
  implementation. `LP32_GUEST_RUNTIME_DIR` can point to a private directory
  containing i386 `libstdc++.6.dylib` and `libc++abi.dylib`. The bundle also checks its own
  `compat-runtime` directory; `GUEST_RUNTIME_DIR` selects the directory copied
  by `make bundle` (default `build/guest-runtime`). Host `libSystem` and
  frameworks always remain bridged.
- Source's GLSL shader-object calls use native OpenGL shader IDs, with
  conversion of 32-bit string arrays. Legacy ATS/ATSUI font calls use CoreText
  for font selection, glyph metrics, shaping and bitmap drawing.
- AudioQueue output has low-memory buffer mirrors and native-to-guest
  callbacks. Bink's Sound Manager bridge supports queued 8/16-bit PCM,
  completion callbacks, pause/resume, volume and channel disposal; it is not
  a full implementation of the legacy Sound Manager or its compressed codecs.
  BSD sockets, page mappings, suspended threads and guest
  `setjmp`/`longjmp` support the engine's additional runtime requirements.

The loader currently targets the supplied `portal2_osx` image (entry `0x1cf0`,
image end `0x335c`), which is also used by the supplied flat depot; it does not
cover every historical Portal 2 release. Dynamic unloading,
full dyld search semantics, arbitrary legacy Objective-C signatures and a
complete C++/OpenGL compatibility surface are not implemented.
Both windowed and fullscreen presentation use Cocoa. Display-wide gamma
adjustment is unavailable. AudioQueue disposal waits for native callbacks to
finish, including when the caller permits asynchronous disposal. Guest
`longjmp` cannot unwind across a native-to-guest callback boundary. Font
rendering has been checked in the menu and the first room's tutorial prompts.
Automated console input previously duplicated characters; the video-menu
selection check passes with valid window handles, but console input still
needs rechecking. Controllers, co-op and community content remain unverified.
Temporary dates follow guest autorelease-pool lifetimes, and
CFString Create/Release calls reclaim their handles; long sessions may still
expose limits in other retained Cocoa-object and thread registries.

## Experimental cross-build co-op (issue #10)

These source changes are not in dev.6. They implement the table/class overrides
and diagnostics from [issue #10](https://github.com/Yash-Singh1/lego-mac-compat/issues/10).
They are opt-in and apply to the Mac host. The report tested Mac build 8408
hosting a Windows build 10090 client; this checkout has no new live co-op result.

Build the loader and update an existing local app, then launch it with:

```sh
make -C native GAME=portal2 promote-loader portal2-converter
LP32_SET_CONVARS="sv_sendtables=1" \
LP32_DROP_SERVER_CLASSES="CPointSurvey" \
LP32_TRACE_STEAM_METHODS="GameServer" \
LP32_TRACE_STEAM_CALLBACKS=1 \
"native/build/Portal2-Compat.app/Contents/MacOS/Portal2Compat" -condebug -console -dev
```

`LP32_SET_CONVARS` is a comma-separated `name=value` list for `engine.dylib`.
The loader locates private defined symbols and calls `ConVar::SetValue` after
the engine's constructors finish, before map spawn generates send tables.
This reaches the development-only `sv_sendtables` object that the retail console
does not register. The setter maintains the string, float and integer values.
Guest `dlsym` and cross-module imports retain their existing visibility rules.

`LP32_DROP_SERVER_CLASSES` is a comma-separated list for `server.dylib`.
Removing `CPointSurvey` prevents the old Mac server advertising `DT_PointSurvey`,
which the reporter's Windows client no longer has. The loader checks the
registration's network name and validates the active linked list before removing
the entry, including when it is the head. It does this before the engine builds
the class indices/table stream. Missing symbols, incompatible or unreadable
objects, cyclic lists and oversized settings produce diagnostics without the
corresponding mutation. Empty or unset variables leave normal behavior intact.
These controls use the i386 Source layouts and are not a general patch for
arbitrary game builds or an instruction to remove other classes.

`LP32_TRACE_STEAM_METHODS` filters interface versions by substring; `*` selects
all interfaces. It logs begin/end, argument words and return values, including
typed packed-record calls. Authentication ticket contents and pointed-to strings
are not dumped. `LP32_TRACE_STEAM_CALLBACKS=1` logs delivered callbacks, including
IDs/owners, approval, denial/kick reasons and modern ticket-validation responses.
Both traces default off; `LP32_TRACE_STEAM_CALLBACKS=0` disables callback tracing.
Logs contain player Steam IDs and can contain network addresses; redact these
before sharing them. Denial text is bounded by the actual callback length.

**The Steam authentication failure remains unresolved.** The report's 22-minute
session also suppressed authentication denials, including for an unapproved
player. That workaround is not implemented: `LP32_IGNORE_STEAM_DENY` and
`LP32_IGNORE_STEAM_DENY_UNAPPROVED` have no effect. Every Steam denial still
reaches the guest, so the reported roughly 100-second disconnect can still occur.

The proposed `BeginAuthSession` translation needs more evidence than an IP
replacement. Valve documents the legacy blob as coming from
`InitiateGameConnection`, while the modern API takes a `GetAuthSessionTicket`
ticket and a known Steam ID; it warns against mixing the two authentication
flows. A future translation must establish the ticket format and peer identity,
wait for validation, preserve failures/revocations, and end sessions correctly.
See [Valve's game-server authentication APIs](https://partner.steamgames.com/doc/api/ISteamGameServer#BeginAuthSession).

`make -C native test-portal2-coop` checks real i386 constructors/private symbols,
normal and downward relocation, repeated module opens, class removal and invalid
inputs, method filters and packed returns, short callback buffers at a protected
page boundary, and unchanged delivery of every legacy denial reason 0–14.
It uses a mock native Steam client; it does not validate real tickets or online
play. It also runs as part of `test-guest-dyld`.

## Checks

```sh
make test-macho-file test-guest-dyld
make test-portal2-converter
python3 tests/test_portal2_depots.py --source /path/to/depot-download
make test-gl-shader-bridge
make test-cursor-state
make GAME=portal2 test-portal2-loader
make test-arb-program-guard test-arb-sampler-usage
LP32_OBJC_PROXY_SELFTEST=1 arch -x86_64 build/game_loader
LP32_GL_BUFFER_SELFTEST=1 arch -x86_64 build/game_loader \
  build/Portal2-Compat.app/Contents/SharedSupport/Portal2.image
```

The standalone fixtures need no game data or historical SDK. They build tiny
original i386 libraries with Apple's linker and check both relocation
formats, function and data pointers, dependency initialization order,
one-time initialization, universal slices, missing symbols and rollback
after malformed-library rejection. They also exercise re-exports, weak
imports, the two Darwin `stat` layouts, 64-bit file offsets, both directory
entry layouts, reentrant directory reads, directory filter and sort callbacks,
deferred signal delivery and `iconv` pointer conversion. Wide-format checks
cover Unicode strings, 32-bit and 64-bit integers, floating-point values,
variadic widths, truncation, and the size of guest `%n` writes.
The legacy mouse-position check guards both sides of the four-byte output.
Cursor checks cover nested hide/show requests and excess show calls while
the fixture runs in the background. The cursor-state test uses a fake host
to check immediate release on focus loss, suppression of repeated background
capture/warp requests, restoration on return, menu transitions while inactive,
and retrying failed host releases without skipping cursor visibility cleanup.
Additional fixtures cover protected page allocation, time conversion, UDP
loopback, process pipes, CoreText glyph metrics and bitmap coverage, repeated
font-layout disposal, suspended threads, AudioQueue callbacks and reuse, and
guest stack recovery with signal masks. The Cocoa check covers point
conversion, repeated event delivery, and date lifetimes across nested pools.
The font fixture also checks thousands of CFString allocations and releases,
including overlapping references to the same native string.
The OpenGL test compiles and links
GLSL through the bridge and verifies a rendered pixel in an offscreen buffer.
The buffer test checks partial uploads, shared-context bindings and handles,
growth, shrinkage, same-size orphaning, rejected allocation parameters, and
binding invalidation on deletion.
The Portal 2 loader check maps the real
launcher and dependencies without executing game constructors or entering
the UI. Passing these checks is not evidence that the game is playable.

The depot integration test converts the real download into a disposable app,
checks both depot overlays and library hashes, signs and loads the result,
and rebuilds it with Make. It checks that saves/options survive, obsolete
libraries disappear, and the download remains unchanged. It opens no windows.
The dynamic-linker fixtures run with both `bin` and `bin/osx32` layouts.

The guest dylib fixtures also exercise a local mock x86_64 Steam client through
real i386 calls: interface ordinals (including private and legacy slots),
64-bit Steam IDs, strings, mixed float/double arguments, callbacks and API call
results. More than 2,200 imported symbols cover the expanded thunk region.
The mock tests require neither Steam nor an account.

Wide-text fixtures cover Unicode, field widths, assignment suppression,
floating-point and 64-bit integers, and guards around i386 long/pointer output
cells. Native Steam fixture binaries and generated loader stress inputs are
original test code; no game or Steam client binaries are tracked.

## Intermittent Core Audio crashes

A dev.2 user on an M2 Air running macOS 26.5.2 reported SIGILL in caulk's
allocator during `AudioQueueStart` after repeated map loads. Their equivalent
standalone native queue test did not reproduce it, and the game later stopped
crashing without an identified change. This does not establish a memory leak,
a device-specific cause, or a fix from restarting Core Audio.

The bridge now converts both AudioUnit input/render callback properties from
8-byte i386 structures to native callbacks, translates callback property reads,
and retires standalone units and callbacks on `CloseComponent`. OpenAL capture
uses device handles, including safe failed-open/close cleanup. Real microphone
capture and in-game voice chat still need testing. Wide-character libc support
includes integer conversions with i386 range limits, duplication/search/case
operations, character classification, and `vswscanf`; guest `errno` is writable
and thread-local through `__error`.

Dev.6 and later enable a bounded crash recorder automatically. There are no launch flags to
remember. It retains 65,536 allocation/free/reallocation events in native memory,
including allocation and free callers and the object's first word before free.
An independent 4,096-event buffer records animation, pending physics entities,
trigger callback entry/exit, and base-entity destruction. The physics hooks
require server UUID `0420DBF7-9CD0-31E2-84DD-61DDC692B704` and matching instruction
bytes; other server versions explicitly log that only general capture is enabled.
Hooks retain the original functions. The lifetime fix below invalidates references
to destroyed entities before those functions can consume them.

On SIGSEGV, SIGBUS, SIGILL or SIGABRT, `capture begin version=1` through
`capture end` in the run log contains the retained history, exact guest image
paths/slides/UUIDs, a 4 KiB stack snapshot, up to 64 frame-chain entries, and
bounded memory snapshots around register/stack object candidates (including
allocator headers and one pointer indirection). Reads use Mach's memory-copy API;
unreadable pointers are marked rather than dereferenced. The capture writer uses
fixed buffers and `write`, without heap allocation or allocator/stdio locks.
Guest execution threads receive alternate signal stacks, so a damaged guest
stack can still be reported. Normal play performs no per-event disk writes.

Analyze a new report with:

```sh
python3 native/tools/analyze_crash.py ~/Library/Logs/Portal2Compat/last-run.log
# Inspect a particular entity, including allocations containing this address:
python3 native/tools/analyze_crash.py /path/to/run.log --address 0x12a47180
```

The analyzer verifies each local i386 image's UUID before resolving function
names. `--image-dir /path/to/matching/dylibs` supports images moved since the run.
Keep the exact old binaries when upgrading if an unresolved crash still needs
symbolication. The history is a rolling window, not a complete record of every
memory write: an absent event does not prove an allocation/deletion never
happened. Queue observations cap each vector at 256 entries per checkpoint.
Capture may be incomplete after a forced kill or a machine/process failure that
does not deliver a catchable signal.

`make -C native test-crash-trace` checks unreadable memory, concurrent capture,
and ring wrap. `make -C native test-guest-dyld` additionally checks destroyed
main/worker guest stacks, freed-object history, verified symbolication, the five
physics hooks with nested real i386 calls, and default fatal-signal termination.

### Deferred physics entity lifetime fix

The September 8 report confirmed that a destroyed `CFuncBrush` remained in the
end-of-tick physics queue. Its memory was reused for a model filename before the
queue consumed it. Dev.6 and later invalidate every matching entry
in both deferred vectors on entry to the base-entity destructor, before memory
can be freed and reused. This covers entities destroyed during world teardown
as well as during physics callbacks.

Invalidation leaves an empty slot so an active guest loop's cached count and
indices remain valid. Before the next outer update, stable compaction removes
empty slots and preserves the full pusher record, including its old origin.
During an active update, trigger calls and both virtual-update loops skip empty
slots. The virtual-loop guards run before the vtable dereference. Live entities
continue through the original game code, and a new entity at a reused address
can be queued normally. No allocator quarantine or permanent address blacklist
is needed. New recorder events identify invalidations and compaction.

The patch checks the server UUID and all modified instruction sequences before
installing any hooks. The launch log reports `deferred physics lifetime fix
enabled` when installed. It is included in the dev.6 converter and rebuilt local
apps.

The regression executes the supported server's end-of-tick instruction sequence
with fixture dependencies. Bypassing the fixture destructor hook reproduces the
null-PC crash; the fixed path survives deletion/reuse, duplicate references,
self-deletion and deletion of a later object during a trigger, and reuse for a
new live entity. It checks that surviving entities get their trigger/shadow
updates and retain the correct old origin.

SIGILL writes register diagnostics before default signal termination, so
macOS can still produce a full crash report. If the crash recurs, collect:

- The `~/Library/Logs/Portal2Compat/` folder. Current builds keep ten recent
  `run-*.log` files, plus `last-run.log` and `previous-run.log` aliases. Older
  builds only have `last-run.log`, which must be copied **before launching again**.
- The matching `Portal2Compat*.ips` report from
  `~/Library/Logs/DiagnosticReports/` (include the whole file).
- The exact macOS version/build, selected audio output, and map transition.

For a reproduction with queue lifecycle details, run the converted executable
from Terminal with `LP32_TRACE_AUDIO=1`, redirecting stderr to a file. The trace
records queue format, native handle, total allocated buffers/bytes, entry into
start/stop/reset/dispose, and completion status. It does not log PCM data.
The missing callback translation is a confirmed defect; its connection to the
intermittent caulk allocator assertion remains unproven.

`make test-guest-dyld` checks real i386 callbacks through a native AudioUnit
without opening the microphone, repeated callback replacement, eight-byte
property sizes, guarded outputs, OpenAL failed-open cleanup, and SIGILL from
both host and guest instructions. It also repeatedly starts/stops queues with
the reported 128 buffers of 1024 silent bytes, stereo 44100 Hz/16-bit PCM.
These regression checks are not a substitute for repeated in-game map loads
on the affected machine.

### September 7 crash-log follow-up

[Issue #7](https://github.com/Yash-Singh1/lego-mac-compat/issues/7) says the
French reporter crashes at launch; it has no further reproduction steps.
The report's loader UUID `6745ccbf-1383-3ff7-9adc-1e66f7ab174b` matches the
published dev.2 converter's loader. It is from `MacBookAir10,1` on macOS 27.0
build `26A5425a`. This is a different immediate failure from the caulk report.

With the current Steam Mac engine and dev.2's module placement, guest RIP
`0x7131e77d` resolves to `engine.dylib+0x31d77d`, the `ud2` at the end of
`Sys_Error_Internal`. The saved EDI `0x7131e631` also matches that function's
PIC anchor. Immediately before the trap the engine prints
`##### Sys_Error: %s` and flushes stdout. The trap is deliberate; this does
not identify the earlier error or establish an unsupported CPU instruction.
The `.ips` contains neither that message nor the stack buffer holding it.

Finder logging now captures stdout as well as stderr, and this particular
fatal message is mirrored into the diagnostic log even for Terminal launches.
Every session records macOS version/build, hardware model, and loader UUID.
Each run gets its own file so another launch cannot truncate it. Retention
keeps ten recent runs and skips files whose producing process is still alive.
`LP32_LOG_DIR` redirects diagnostics into a supplied directory for isolated
tests. Ordinary Terminal streams remain attached to their existing pipes;
collect full Terminal output separately if detailed tracing is enabled.

The audio reporter's ZIP distinguishes a dev.2 `.ips`, a patched dev.2 crash
dump, an older patched crashing terminal run, and a successful patched dev.2
run. The successful run rejects the malformed voice-input callback. Treat
these as separate observations, not a controlled comparison of one change.
The older crashing log creates exactly one output queue, allocates 128 buffers
of 1024 bytes, and traps on its second start, after three consecutive immediate
stops. It does not show repeated queue creation leaking handles. The clean log
uses one queue, starts it ten times, and eventually frees its buffers and
disposes it successfully.

The caulk dump confirms the native instruction is an explicit `ud2`. With
the matching caulk UUID `346531c7-e1c9-3570-91c0-972ab4dc0f11`, the preceding
code reads the size from `rsi+24`; the supplied dump shows zero there. For the
`0x8040` request, subtracting the request and 32-byte header produces the
reported negative remainder `-0x8060`. This establishes invalid allocator
state, but the dump cannot establish who produced it. Zero bytes alone do not
prove memory was never written. Nor does re-enqueueing a buffer inside its
AudioQueue completion callback constitute misuse: Apple's AudioQueue header
explicitly permits it. The supplied silent/deferred callback experiments have
not been incorporated as a production fix.

The guest audio stress fixture now includes consecutive immediate stops and
restarting the emptied queue, as well as filled-queue restarts. The logging
fixture checks Finder output, fatal-error text, simultaneous launches, bounded
history, unrelated-file preservation, and unchanged Terminal redirection.

### Issue #7: dev.3 run

The September 7 follow-up ZIP contains one identifiable dev.3 run,
`run-20260907T180741-945255-12397.log`. Its loader UUID
`9dc2cffb-a125-3c2a-bdfd-a29e956c3a61` matches the published dev.3 converter.
`last-run.log` is byte-identical; `previous-run.log` has no dev.3 identity,
and the graphics log contains sessions from multiple PIDs. Do not attribute
the older Steam initialization failures or saved ARB program to this run.

The current run successfully initializes native Steam and reports
`CClientSteamContext logged on = 1`. It ends after loading `vaudio_speex`
with SIGSEGV, code 1, accessing `0xe3f1fe67` at host RIP
`0x7ff80c1c9ac0` (CS `0x2b`). It contains no `Sys_Error` message or SIGILL.
That establishes a different immediate failure from the original report;
the last codec warnings alone do not identify its cause. There is no native
stack or image list in this ZIP to directly symbolize the failing host
instruction, but its registers permit a narrower reconstruction.

An isolated i386 fixture using the published dev.3 loader reproduces the
reported `rax=0xe67`, `rcx=0x10`, `rdx=0x104`, `rsi=0xe3f1fe67`, and
`rdi=0x102b6120` by calling
`strncpy((char *)0x102b6120, (const char *)0xe3f1fe67, 260)`. Both crashes
have `rbp-rsp=0x758`, matching the published import dispatcher's frame plus
the return address of its direct native `strncpy` call. The native routine
faults on the initial source read: `rax` is its source page offset, and
`rcx` is the number of 16-byte chunks in the bounded copy. The destination
is inside the guest heap range, not above 4 GiB. This strongly identifies
the immediate failure as an invalid source-string pointer; it does not
identify the guest caller, prove a memory leak, or establish why the pointer
became invalid.

Tracing 260-byte string-copy callers exposed a confirmed Steam bridge bug:
[`GetUGCDetails`](https://partner.steamgames.com/doc/api/ISteamRemoteStorage#GetUGCDetails)
returns its borrowed filename through `char **`. Dev.3 classified that as
an ordinary pointer and let native Steam write eight bytes into a four-byte
guest pointer cell, overwriting the following cell and exposing a truncated
native address. The game's `CUGCFileRequest::Steam_OnUGCDownload` passes
that filename to `V_strncpy` with a limit of 260. This is a plausible failure
path, not a confirmed attribution of issue #7: the report lacks the guest
return address needed to identify that particular call.

The bridge now stages marked string outputs in native pointer cells, copies
the strings into guest memory, and writes back only four bytes. Failed calls
preserve the guest output. The ABI generator also rejects unconverted pointer
arrays instead of stripping away their pointer depth. The mock Steam fixture
returns a filename above 4 GiB and checks guard words, readable/stable copied
text, failure behavior, and null output arguments. The guard check fails
against the published dev.3 loader and passes with the fix; all four guest
dylib fixture variants pass as well.

Dev.3 preserved default SIGILL termination but still used `_exit` for
SIGSEGV/SIGBUS, suppressing normal macOS crash reporting for those signals.
The follow-up change preserves default termination after logging all fatal
signals. Regression checks exercise real host/guest invalid reads and UD2,
plus explicit SIGBUS, and require signal termination rather than an ordinary
exit status. This repairs a diagnostic gap, not the invalid access itself.

The completed follow-up also preserves the prior guest filename when the
bridge cannot allocate a string copy. Its regression grows beyond the old
1024-entry string cache,
checks native buffers reused for different filenames, handles successful null
outputs, and ensures invalid output pointers from failed native calls are
never read. `make -C native test-steam-abi` checks generation of marked string
outputs and rejection of pointer arrays, aliases, and pointer-bearing structs
using an original fixture; no Steam SDK download is needed for that test.

Fatal logs now identify the innermost active guest import, its guest return
address, and up to four argument words. The per-thread context covers both
normal and fast dispatch and restores the enclosing call after nested guest
callbacks. Argument words are read through Mach's checked memory-copy API;
the reporter does not dereference the string or object addresses they contain.
The real i386 fixture recreates the reported bad `strncpy` read and verifies
that its caller, `0xe3f1fe67` source, and 260-byte limit reach both stderr and
the persistent log. A nested Steam callback regression verifies restoration,
and direct guest faults verify that completed imports are not mislabeled as
active. These checks run with `make -C native test-guest-dyld`.

Direct loader invocation accepts the converted image name (`Portal2.image` or
`PortalStoriesMel.image`) as an identity hint. For a raw or renamed launcher,
set `LP32_GAME=portal2` or `LP32_GAME=PortalStoriesMel` explicitly: the two
launchers share a fingerprint.
