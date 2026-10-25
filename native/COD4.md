# Call of Duty 4: Modern Warfare (Mac)

The converter supports Aspyr's 1.7.2 Steam and Mac App Store editions. The Mac
App Store version is reported working after the issue #13 startup fixes in
[COD4 Converter dev.5](releases/COD4-Converter-0.0.0-dev.5.md). Local gameplay
verification covers the Steam edition; local checks for the supplied App Store
executable cover its factory/retain, IOKit data, and request encryption paths.

Other 32-bit Intel Mac COD4 apps with their game data are accepted regardless
of bundle ID, version string, or executable hash. Untested executables use a
generic profile that skips patches tied to fixed Steam addresses. Accepting an
app does not establish that its menus or gameplay work. PowerPC-only and Windows
executables cannot run in this i386 loader. Activation and receipt checks remain
the game's responsibility.

## Mac App Store compatibility fixes

Guest autorelease scopes now cover superclass calls and calls before the guest
creates a pool. Native factory/getter results belonging to guest classes also
remain alive across the per-import native pool. A fallback scope preserves
pool-free startup objects for the rest of the guest run. Owned method-family
results still deallocate when their guest ownership ends.

IOKit CreateCFProperty, CreateCFProperties and SearchCFProperty results now
carry guest ownership. Previously, an autorelease could free the native value
while leaving its guest handle registered. The supplied executable reproduced
this crash at `getBytes:length:`; its own IOKit helper now passes with explicit
and fallback guest pools.

The App Store startup routine's legacy OpenSSL setup, memory BIO, PEM RSA public
key decoding and EVP encryption calls use macOS LibreSSL through guest handles.
OpenSSL 0.9.7's encryption signature calls `EVP_PKEY_encrypt_old`, rather than the
modern function with different arguments. The supplied routine reaches the
checkpoint before asynchronous networking. That local check sends no request
and does not verify every OpenSSL API or the complete App Store game flow.

The loader also handles fragile-ABI NSString literals in
`__OBJC,__cstring_object`, including layouts above `0x0c000000`. This fixes a
separately reproduced string-layout gap; the issue #13 executable uses CFString
literals and did not need that particular change.

## Diagnostics and regression checks

Finder launches write `~/Library/Logs/COD4Compat/last-run.log`, or
`~/Library/Logs/COD4MPCompat/last-run.log` for multiplayer. Each launch replaces
the previous log. Failed lookups include the receiver's image section, readable
object words, known guest class, and recent identity-map retirement.

The dev.5 release enables a bounded Objective-C recorder automatically. Native
crashes include the active selector, guest/native identity, method and type
encoding, recent message and lifetime events, native stack words, and a startup
snapshot of native image bases and UUIDs. The recorder copies scalar values and
names without retaining objects. Its crash reporting does not query Objective-C
or dyld or acquire a blocking recorder lock. Set `LP32_OBJC_DIAGNOSTICS=0` to
disable it. Local builds default to off unless built with
`OBJC_DIAGNOSTICS_DEFAULT=1` or launched with `LP32_OBJC_DIAGNOSTICS=1`.

Run the following standalone checks:

```sh
make -C native test-objc-lifetime test-objc-constant-string test-objc-iokit-ownership test-openssl-legacy
```

They compile original i386 fixtures and need no game files or Apple runtime archive.
The lifetime suite deliberately triggers a native crash and checks persistent
logs and bounded history wrap. Set `LP32_TEST_DIAGNOSTICS_DEFAULT=1` when testing
a binary with recording enabled by default to check that launch flags are not
needed.

The optional `tests/test_cod4_appstore_startup.py --image PATH_TO_COD4_IMAGE`
uses the supplied issue #13 image in a converted app with its private runtime.
It validates the image hash before using diagnostic addresses and stops before
starting a network request. The repository and converter contain no game files.

The Steam hardware-change prompt, "Set Optimal Settings?", uses
`RunStandardAlert` with no modal filter. The bridge now runs that native alert,
returns the chosen 16-bit button index, and retires the disposed dialog token.
`make -C native test-carbon-standard-alert` checks real native OK, Cancel and
Other clicks, result-buffer boundaries, token retirement and dynamic lookup.
This addresses a dev.5 report that trapped at `_RunStandardAlert` on macOS 27;
the native-dialog regression runs locally on macOS 26.5.2.

## Validation status

The Steam build is an experimental port with campaign and local multiplayer smoke tests.
On macOS 26.5.2
with an Apple M5 Pro, single-player initializes Steam, plays the intro movies,
renders the main menu, creates a profile, and starts or resumes the F.N.G.
training mission. Menu selection and pause/resume work. The mission advances
to the rifle pickup objective and writes a save in the test profile.

The training room now renders opaque world geometry, weapons, characters,
lighting, foliage and the HUD. The obsolete exclusive-fullscreen AGL request
previously failed and triggered a fallback with no depth or stencil buffer,
allowing the skybox to overwrite the world. The compatibility window now
retains the requested depth/stencil format (32/8 on the tested GPU).
No diagnostic shader replacements or draw overrides are included.

Both executables pass their dependency/initializer checks (45 single-player,
49 multiplayer). Multiplayer plays its intro, creates/selects a profile, opens
the server setup, loads a password-protected local Ambush map, and spawns after
team/class selection. The spawned view renders the buildings, terrain, weapon
and HUD correctly; firing produces impacts and reload input plays the reload
animation. The original PunkBuster modules now initialize without the startup
error; joining a protected server has not been verified. An Internet server
connection also completed its HTTP mod downloads and reached the active
Backlot map in spectator mode, followed the server's relocation redirect,
and reached team selection on Overgrown at the replacement server.
Voice chat, sustained online gameplay,
held movement/mouse-look, controller input, and audible output have not been
verified. Automated key taps did not establish continuous movement. Interactive probes
use isolated profiles and muted audio, leaving the Steam installation unchanged.

Intermittent bursts of gameplay static and camera-dependent mesh stretching
remain reported issues. The latest static report has timing logs but no audio
capture, so its cause has not been established.

## Converter app

The [dev.6 import audit](COD4-IMPORT-AUDIT.md) records additional network,
launcher and display bridges and the remaining unsupported calls found in
the supplied issue #13 and Steam executables. Import-name coverage alone
does not establish complete compatibility.

The COD4, MW2, and MW3 converters package GLMetal and its native shader
compiler. The [COD2 converter](COD2.md#converter-app) uses Apple OpenGL.
Build all four standalone apps with `make -C native cod-converters`, or use
`cod2-converter`, `cod4-converter`, `mw2-converter`, or `mw3-converter` for one title. The output
apps are `native/build/COD2-Converter.app`, `COD4-Converter.app`, `MW2-Converter.app`, and
`MW3-Converter.app`. Each supports campaign and multiplayer.

Each GLMetal converter build asks `native/glmetal` to rebuild its `lib` target, then copies and
signs the resulting driver and compiler. `GLMETAL_SOURCE` can select another
GLMetal checkout. The converters keep that build inside the app and copy it
into every converted game. They do not fetch updates at conversion time.
Rebuild the converter after changing GLMetal; previously converted games
keep their existing driver until updated or converted again.

`native/glmetal` is a submodule of
[Yash-Singh1/GLMetal](https://github.com/Yash-Singh1/GLMetal), pinned by the
parent repository. Run `git submodule update --init --recursive` after cloning
or pulling a change to the pin. The build initializes a missing submodule,
but leaves an existing checkout alone so it does not overwrite local work.
CI checkouts must initialize submodules too, for example with
`submodules: recursive` in `actions/checkout`.

When migrating a checkout that previously contained the vendored source,
move any leftover `native/glmetal/build` and `native/glmetal/third_party`
directories outside `native/glmetal` before its first initialization, then
move them back. This preserves compiled dependencies and lets Git clone
into an empty directory.

Make renderer changes in the GLMetal repository, test and push them there,
then update the parent repository's pin to that tested commit:

```sh
git -C native/glmetal fetch origin
git -C native/glmetal checkout <tested-commit>
git add native/glmetal
git commit -m "Update GLMetal to tested revision"
```

The converter build uses the checked-out revision; it does not automatically
follow GLMetal's latest `main` commit.

Generated apps use GLMetal both from Finder and when their executable is run
directly. `LP32_GL_BACKEND=apple` remains an explicit command-line override.
Driver build hashes are recorded in `Contents/Resources/GLMetal-build-info.json`.
The converter verifies its packaged files before copying game data.

The command-line bundle targets use the same driver packaging for
`GAME=cod4`, `cod4mp`, `mw2`, `mw2mp`, `mw3`, and `mw3mp`. Direct use of
`tools/bundle_cod4.py` or `tools/bundle_mw2.py` accepts `--glmetal` for a directory
containing an already built driver and compiler. The MW script accepts
`--game mw2` or `--game mw3`.

Packaging verification without launching games:

```sh
make -C native test-cod4-converter test-game-profile
python3 native/tests/test_glmetal_packaging.py
python3 native/tests/test_mw2_bundle.py
python3 native/tests/test_cod_converters_packaging.py --runtime /path/to/verified/compat-runtime
```

The last test converts small fixtures through all six campaign/multiplayer
paths using relocated converter apps, checks signatures and driver hashes,
and checks that the sources are unchanged. It uses an existing COD4 runtime
and does not download one or run graphics workloads.

Build the standalone converter with `make -C native cod4-converter` from the
repository root. Open `native/build/COD4-Converter.app` and select **Campaign**
or **Multiplayer**. Click **Find in Steam**, choose the game manually, or drag
the original Mac COD4 app or its game folder into the window. Discovery also
accepts Steam library/common folders and the nested multiplayer app.

Choose an output folder. Campaign creates `COD4-Compat.app`; Multiplayer
creates `COD4MP-Compat.app`. **Convert Another** returns to the selection flow
so both editions can be created. Existing copies receive numbered siblings
instead of being replaced, preserving their data and saves. Each app contains
its own roughly 7 GB game-data copy. Cancel removes the unfinished copy.

The converter is universal (Apple Silicon and Intel), includes the current
game loader, and uses only macOS system tools. Its users need no Python,
Make, Xcode, or repository checkout. Games require Rosetta 2 on Apple Silicon.
The tested Steam build requires Steam with the owning account before launch.

On the first conversion, it downloads Apple's 4.72 GB Lion archive, verifies
its checksum, and extracts the two private C++ libraries without running an
installer. Later conversions reuse the verified cache at
`~/Library/Caches/org.32bitgoofy.COD4Converter/`. The converter itself contains
no game assets or Apple runtime libraries. Its workflow is adapted from the
TFU and Portal 2 converters.

Validation includes the GUI's Steam detection and completed Campaign flow;
both real Steam modes converted from a relocated converter, with archive
extraction, signature checks, original data hashes, and all guest initializers
passing. Tests also cover cached reuse, existing-copy preservation, literal
paths, cancellation, staging cleanup, and an unchanged Steam installation.
The GUI converter's tests are `make -C native test-cod4-converter`; the real
installation integration test is
`python3 native/tests/test_cod4_converter_integration.py` (optionally pass
`--installer /path/to/InstallMacOSX-Lion.dmg` to exercise archive extraction).

## Command-line build

Requires macOS, the Xcode command-line tools, Python 3, Rosetta 2 on Apple
Silicon, and a 32-bit Intel Mac copy of the game. Start Steam with the account
that owns the game when using the Steam release.

```sh
cd native
make cod4-runtime
make GAME=cod4 bundle
make GAME=cod4mp bundle
```

Steam's default library and additional libraries from `libraryfolders.vdf`
are searched automatically. To select a particular installation, pass the
outer single-player app for either edition, or a standalone multiplayer app for
`GAME=cod4mp`:

```sh
make GAME=cod4 SOURCE_APP="/path/to/Call of Duty 4.app" bundle
make GAME=cod4mp SOURCE_APP="/path/to/Call of Duty 4.app" bundle
```

The output apps are `build/COD4-Compat.app` and `build/COD4MP-Compat.app`.
Each contains its own copy of `Call of Duty 4 Data`; allow space for a full
additional data copy per app. No game files, Steam SDK binaries, or Apple
runtime libraries are stored in this repository. The converter checks for a
32-bit Intel Mac executable and copies the selected edition without modifying
its original files. It rejects overlapping source/output paths and refuses to replace
an output that is not marked as a generated COD4 compatibility app.

`cod4-runtime` extracts two private i386 libraries from Apple's public Lion
archive, downloading approximately 4.72 GB if needed. It verifies the archive
and library checksums, mounts read-only, and never runs the installer or
changes system libraries. A previously prepared runtime can be selected with
`GUEST_RUNTIME_DIR=/path/to/guest-runtime` when bundling.

After runtime changes, update a previously built app without recopying data:

```sh
make GAME=cod4 promote-loader
make GAME=cod4mp promote-loader
```

## Installation layout and identity

```text
Call of Duty 4.app/Contents/
  MacOS/Call of Duty 4
  MacOS/libBinkMachOx86.dylib
  MacOS/libsteam_api.dylib
  Call of Duty 4 Data/
  Call of Duty 4 Multiplayer.app/Contents/
    MacOS/Call of Duty 4 Multiplayer
    MacOS/libBinkMachOx86.dylib
    MacOS/libsteam_api.dylib
```

The tested installation reports build 111539 (Steam build 2737681):

| Edition | Executable SHA-256 |
| --- | --- |
| Single-player | `43629c7f2f1b6f891e93c134f3101dbfb9fc4b46cabb1491e90437a450208870` |
| Multiplayer | `f90ec11a0822628aac0c2f788d980967ad333405b2fec5f3ecfbca1e3d8d1374` |

The original i386 Bink and private C++ libraries run as guest code. Steam
uses the publisher's x86_64 SDK slice and the existing Steam bridge. The
proof-of-purchase request goes to the real Steam client; the legacy game's
license-key log call is redacted. This does not emulate ownership or bypass
Steam licensing.

A signature-checked in-memory patch accepts a 32-bit depth-buffer capability
alongside the original 24-bit capability. Current Apple GPUs otherwise get
rejected before display creation. The AGL adapter removes only the obsolete `AGL_FULLSCREEN` flag, preserving
attribute/value pairs and the depth and stencil requirements for the Cocoa
fullscreen window. A native pixel-format regression verifies those buffers. The original executable on disk is unchanged. The
launcher's default `-g` argument is Aspyr's own direct-game option, which
skips its obsolete Game Guide web launcher.

Apple's legacy OpenGL compiler can route texture coordinates incorrectly when
an ARB shader declares named `OUTPUT` aliases, including unused outputs. On
Apple OpenGL, the COD4 bridge expands those aliases to their explicit
`result.*` bindings before compilation. This preserves the shader instructions
and fixes the intro movies' vertical streaking. A standalone GPU regression
test checks interpolated pixel values without loading game assets.
The preprocessing also normalizes classic Mac carriage-return line endings
and preserves leading comments, blank lines and `OPTION` declarations before
inserting math guards. A GPU test covers that ordering as well as the aliases.

Mapped OpenGL buffers use low-address staging storage. Explicit range flushes
and implicit unmaps submit their uploads so that a loading context which never
presents can publish data to the rendering context. The buffer regression
covers per-object flush settings, partial ranges, and shared-context publication.

Carbon mouse coordinates are converted from the physical fullscreen panel to
the game's logical render size. Event coordinates remain available to legacy
polling until physical pointer movement supersedes them. COD4 retains a painted
main window when it moves AGL rendering to a separate fullscreen display port.
Only the display port is presented; the backing window keeps its guest visibility
and input role without appearing as an extra grey window. Leaving fullscreen
restores the backing window if the guest still wants it shown. A native SP/MP
regression covers repeated fullscreen transitions, selection, dialogs and hiding.
Windowed startup translates the obsolete positioning-bounds API's guest display
handle to the native display-ID API and packs its CGRect into an i386 Rect.
The geometry regression checks the main display, explicit handles, invalid
handles and output canaries.
COD4 input follows Carbon's front-process state rather than AppKit's cached
`isActive`, which its legacy event loop does not reliably update. Activation
restores the fullscreen presenter and its GL drawable. Deactivation releases
cursor capture and hiding, suppresses keyboard/mouse events, and freezes mouse
polling so desktop movement cannot steer the game. Guest cursor requests are
remembered and reapplied when the game becomes active. The native window test
checks presenter restoration; SP/MP dispatch tests exercise repeated background
hide, capture, warp, and mouse-poll requests without moving the host pointer.
Fullscreen activation also completes AppKit activation and reapplies the
guest's menu-bar visibility through AppKit presentation options. Previously,
Cmd-Tab could raise the game while retaining the preceding app's menu bar.
Switching away, showing the menu bar, hiding the presenter, or leaving fullscreen
restores the preceding presentation options. The SP/MP window regression checks
these transitions, repeated activation, and background-test isolation.
The Carbon event pump also completes AppKit launch and dispatches pending
`NSEventTypeAppKitDefined` events before reconciling focus. Updating presentation
properties alone left Cocoa activation stale because COD4 never runs
`NSApplication.run`. The event-delivery regression uses the real AppKit queue,
fails without the pump, and verifies that keyboard/application events remain
queued for the guest and that reentrant pumping is safe. Focus transitions log
both requested and effective system presentation state for live diagnosis.

Audio render-notify
callbacks run on the existing callback worker, including safe self-removal and
teardown; legacy byte-count audio conversion adapts to the modern packet API.
An in-game freeze was captured in `AudioUnitSetProperty(SetRenderCallback)`:
the game thread held a sound mutex while waiting for the old bridge callback's
render lock, and that callback was waiting for the same sound mutex. Unit input
bindings are now tracked separately from render locks. Replacement/removal
retires the old context without waiting; its final host render reclaims it after
copying the result. A regression reproduces the guest mutex cycle for 100
replacements/removals and checks that retired contexts are reused safely.
A later crash log places the native trap in Core Audio's `caulk` free-list
allocator during `AudioConverterNew`, with invalid free-block metadata. The
bridge was treating pre-render notifications as filled audio: it copied and
sometimes zeroed their stale buffer pointers. Pre-render notifications now
forward flags/timestamps without sample data; skipped or muted notifications
never clear buffers belonging to the audio unit. A protected-memory regression
crashes with the previous behavior and passes with the fix, including held
callbacks and preservation of a valid post-render mix. This fixes a concrete
corruption path consistent with the crash; the exact gameplay crash has not
been reproduced. `LP32_TRACE_CONVERTERS=1` logs converter formats for further
diagnosis if needed.
COD4's i386 Bink backend supplies little-endian 16-bit PCM in extended Sound
Manager headers. The bridge preserves that byte order for both editions;
marking those buffers big-endian caused static in movies and mission briefings.
Explicit compressed-header format tags (`sowt`, `twos`, `NONE`) retain their
specified interpretation. The regression reads the actual native audio queue's
format and checks decoded sample polarity and amplitude for both byte orders.
Bink buffers also match independent movie decoding (correlation above
0.999999 for the IW and Aspyr logos). A live capture of `cargoship_load.bik`
confirmed repeated silent gaps between approximately 40 ms Sound Manager
buffers. The bridge now submits upcoming PCM before earlier buffers finish,
while retiring commands and invoking guest callbacks in order. Format changes
wait for queued audio to drain, and stop/flush invalidates pending completions.
A native offline-render regression checks sample continuity across four
buffers, copied-data ownership, callback order, and cancellation.
The radio hiss in the captured briefing matches the original soundtrack;
several uninterrupted segments correlated above 0.999 with independent decoding.
A campaign output probe also captured the final native mix with finite samples
and no clipping. Speaker playback across a full session remains unverified.

A separate gameplay capture investigated crackle beside the cargo-ship package,
just before the manifest instruction. After subtracting the matching HQ voice
and ship ambience, the remaining clicks matched the original mission's
`items/item_geiger_counter_lvl1.wav` and `lvl2.wav` PCM (approximately
0.96–0.99 correlation across selected click sequences). These loaded sounds
are embedded in `cargoship.ff`, rather than the loose dialogue WAV archives.
The mission's `package_radiation()` script explicitly plays these loops near
the container. The bundled mission file was byte-identical to Steam's copy.
Muted, isolated replays of the HQ WAV through the guest sound engine, both
resident and streamed, did not reproduce the extra clicks. No audio filtering
or runtime change was made for this captured Geiger-counter effect; this
finding does not establish the cause of other, uncaptured audio problems.

The BSD socket bridge converts resolver pointer arrays and 32-bit timeouts.
Its loopback regression includes a complete burst of COD4 setup fragments:
thirteen 1,310-byte datagrams and a shorter final fragment, with byte and
buffer-boundary checks.

An Internet connection stuck at “Setting up game” was traced to the active
Cloudflare WARP MASQUE tunnel (MTU 1,300). Only the final, shorter setup
fragment reached `recvfrom`; the thirteen full-size fragments never arrived.
The client kept receiving heartbeats and rejecting the incomplete setup.
After WARP was disabled by the user, the same unmodified socket/parser code
received the full fragments and proceeded to mod downloads. Keep WARP off
for this connection, or explicitly configure a server exclusion in WARP;
the app does not change VPN settings or bypass the tunnel itself.

Multiplayer bundles allow legacy HTTP downloads through
`NSAppTransportSecurity.NSAllowsArbitraryLoads`. COD4 uses `NSURLDownload`
with URLs supplied by game servers, so a fixed host exception cannot cover
the server browser. Without this setting, the modern host executable adds
ATS restrictions absent from the original game; macOS logs error `-1022`,
and the game reports a generic download failure. Both the Python bundler
and standalone converter apply this setting to MP only. A live test fetched
`zz_mf.iwd` and `mod.ff` from the server's HTTP host and reached the active
map after this change. The tested server then displayed its own relocation
notice and redirected to a replacement server, where Overgrown loaded and
the team-selection menu appeared. No unsupported imports were encountered.

A separate zero-byte stall had two causes. The public mod host at
`188.165.57.239` rejected Foundation's default `CFNetwork` user-agent with
HTTP 403, while the Carbon game loop could leave the download delegate's
progress and completion/error callbacks pending. MP now supplies
`COD4MPCompat/1.7.2` when a download request has no explicit user-agent, and
services ready default-run-loop callbacks when the original `DownloadDelegate`
is polled. Failed downloads also log their native error. The same host's
6,105,021-byte `~images_00.iwd` completed through the original i386 downloader,
with a SHA-256 matching a direct download. `make test-cod4-download` checks
intermediate progress, exact contents, redirects, HTTP 403, and truncated
responses using a loopback server and the shipped guest delegate.

The multiplayer server-name comparator calls Darwin's `__maskrune` when a
hostname contains a high-bit byte. A captured Join Game freeze stopped at that
missing import with byte `0xc3` (the start of a UTF-8 sequence). The bridge now
implements its signed-rune/32-bit-mask ABI. The server-browser regression runs
12,291 comparisons through the original MP function, including UTF-8, every
high-bit byte, reversed operands and equal names.

COD4's dynamic `dlopen`, `dlsym`, `dlclose` and `dlerror` imports now use the
same i386 loader as Bink and the private C++ runtime. Previously native
`dlopen` rejected the bundled 32-bit PunkBuster modules and the game displayed
“Unable to initialize Punkbuster.” The original `pbcl.mac`, `pbag.mac` and
`pbsv.mac` loaded and initialized with PunkBuster enabled in a separate test
profile; the game reached its main menu without that error. Native system
libraries still use typed ABI bridges. Modules remain pinned for callback
safety. This verifies startup, not acceptance by a PunkBuster-protected server.

## Tests and diagnostics

A later campaign freeze was captured with the game thread waiting on a sound's
mutex while its worker looped inside the host sample-rate converter. The guest
was providing valid looping PCM, but the default macOS SRC did not finish a
fill after the requested output changed from 470 to 471 frames. A standalone
native reproduction used the captured 13,544 Hz planar stereo int16 input and
44,100 Hz mono float output and failed on the second fill. PCM converters that
change sample rate now select the native minimum-phase SRC, preserving filter
history across fills. Equal-rate conversion is unchanged. The bridge regression
covers 16,384 alternating-size fills across four rates, reset, output samples,
and buffer guards; its bounded input callback makes the original infinite loop
fail promptly instead of hanging the test.

Both COD4 modes enable the bounded hitch recorder automatically. Reports go to
`~/Library/Logs/COD4Compat/hitches-*.log` or
`~/Library/Logs/COD4MPCompat/hitches-*.log`. The AGL swap path records CPU work
and buffer presentation separately, with exclusive import timings and nearby
worker events. The default threshold is 50 ms because some original loading
screens intentionally redraw at 30 Hz. `LP32_HITCH_LOG=0` disables recording;
`LP32_HITCH_LOG=/path/to/new.log` selects a file and `LP32_HITCH_MS` overrides
the threshold. Reports retain 32 preceding and eight following frames, with a
five-second cooldown and a limit of 128 reports per process. These are CPU wall
times, not GPU execution measurements.

Recorder v3 counts frequent memory/string operations, character conversion and
thread-ID queries in `counted_runtime` without timing every invocation. Their
time stays in total frame work and any enclosing timed import. Allocation,
locks, I/O, graphics and audio still receive individual timings. Set
`LP32_HITCH_FULL_IMPORTS=1` before launch to restore full import timing; this
adds measurable overhead in loading loops. Worker calls do not contribute to
the render thread's counted total.

A separate, muted 1024x768 campaign probe reproduced a `cargoship` loading gap
of 2.25 seconds as the asset counter jumped from 98/318 to 318/318. Presentation
took 2.3 ms; the delay was in CPU work between redraws. Import profiling found
millions of string operations and thread-ID queries repeating the slow bridge
dispatch. Those operations now use the existing memoized handler mechanism,
with the same guest ABI and libc behavior. The comparable loading gap fell to
0.92–0.94 seconds across two repeats on the M5 Pro test host (the original
path took 2.25–2.27 seconds across two runs). That optimization reduced loading
time but did not keep the screen animating during synchronous guest work. The i386
runtime fixtures exercise first and cached calls, signed comparisons, string
copy returns and padding, case conversion, and thread identity.

The next live session captured repeated 471–485 ms pauses with roughly 2.5
million runtime calls per frame. The live sample continued rendering rather
than waiting on the earlier audio lock. In paired `cargoship` probes, the v2
recorder extended the loading gap from 647–658 ms to 924–942 ms. Counting the
frequent pure runtime calls and memoizing zlib's streaming adapters brought
recorded loading gaps to 722–753 ms across two runs. Those optimizations alone
still left a visible pause in the badge animation.

An isolated copy of the user's `hunted-11` checkpoint also exposed over 716,000
small `inflate` calls in its restoration phase. Memoized `inflate`/`deflate`
handlers use the same stream adapter as ordinary dispatch, preserving input,
output, checksums, error returns and guest allocator callbacks. With detailed
profiling enabled in both runs, time in those inflate calls fell from 205 ms
to 26 ms and the associated frame from 326 ms to 149 ms. These figures describe
that restoration phase; they do not measure the badge redraw fix below. The zlib
regression exercises direct and chained calls, incremental output, reset,
invalid streams, ownership and buffer guards.

The SP loading-screen patch now keeps the original cinematic badge and progress
bar redrawing during synchronous mission initialization. A guest stack trace
placed the long no-present interval inside `SV_InitGameProgs`: script parsing
and compilation, entity creation, and the level's initial script execution.
Meanwhile, the database thread advanced from roughly one third to all of its
fastfile blocks without a single screen refresh.

Signature-checked i386 hooks yield to the existing `SCR_UpdateLoadScreen` at
`Scr_LoadScriptInternal` and `G_ParseSpawnVars` entry, and at the VM's existing
loop timer check. They only redraw on the main thread with `cls.state == 3`;
the original recursion guard, 33 ms throttle, cinematic clock and progress
counters remain in charge. The hooks run outside string/database critical
sections and preserve the original stack arguments and timer return value.
MP and the other titles have no loading-screen patch profile.

With default hitch recording enabled, two isolated `cargoship` loads had
65.1–67.8 ms maximum frame gaps after tracked mission loading began, versus the
previous roughly 0.8-second stall. `hunted` improved from 694.8 to 52.4 ms.
A separate frame capture verified that the SAS badge rotates and the bar
advances through the formerly frozen interval (capture overhead raised its
maximum gap to 83.8 ms). These are measurements of the animated mission-loading
phase, not a promise of hitch-free rendering: preparing the first badge frame
still took 0.17–0.31 seconds before the emblem appeared in these runs.

```sh
make test-game-profile test-macho-file test-macho-entry test-steam-bridge
make test-cod4-bundle test-cod4-runtime
make test-cod4-server-browser test-dlfcn-bridge
make test-arb-output-aliases test-sound-manager-bridge test-time-manager-bridge
make test-audio-converter-bridge test-audio-render-notify test-zlib-bridge
make test-socket-bridge test-agl-pixel-format test-carbon-fullscreen
make test-focus-policy
make GAME=cod4 test-carbon-input
make GAME=cod4mp test-carbon-input
make GAME=cod4 test-gl-buffer
make GAME=cod4 test-carbon-file
make GAME=cod4 test-cod4-initializers
make GAME=cod4mp test-cod4-initializers
```

The runtime test creates tiny original i386 fixtures, requiring neither game
data nor Steam. It exercises classic and compressed relocations, a universal
dylib, high preferred addresses, dependency constructors, repeated lookup,
malformed-image rollback, context jumps, signal masks, and libc return/write
ABI. Guest fixtures also open and call guest and host libraries through the
actual dl* imports and check error consumption. The server-browser test
requires a built MP bundle and does not open game UI or contact servers.
Initializer tests load the selected game's dependencies and initializers
without entering the game's main or opening Steam/game UI.

For development probes, set `LP32_TEST_HOME_DIR` and
`LP32_APPLICATION_SUPPORT_DIR` to a temporary home/support directory and
`LP32_COD4_PREFERENCES_ID` to a separate preferences domain. Set
`LP32_BACKGROUND_TEST=1 LP32_MUTE_AUDIO=1` for background probes.
`LP32_FAIL_ON_UNSUPPORTED_IMPORT=1` exits with status 78 on an unsupported
import, including imports reached from event callbacks. Without that option,
the normal controlled guest-escape diagnostic remains available.
