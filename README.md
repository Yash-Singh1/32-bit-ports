# 32-bit LEGO games on modern macOS

A native loader that runs the original 32-bit Intel Mac LEGO games on
current macOS (Apple Silicon through Rosetta 2, or Intel). It maps the
game's i386 executable into a 64-bit process, switches into a 32-bit code
segment to run it, and bridges the OS, OpenGL, Cg, CoreAudio and controller
interfaces the game expects. No SIP changes, kernel extensions, VMs or Wine.
You need your own copy of the game.

This branch also adds experimental support for the Mac release of
[Call of Duty 4: Modern Warfare](native/COD4.md), with separate single-player
and multiplayer bundles. The Steam Mac 1.7.2 build is the only release tested
in gameplay. Other 32-bit Intel Mac releases can be converted for testing.
See that guide for build instructions and validation status.

The Steam Mac releases of Modern Warfare 2 and Modern Warfare 3 are also
experimental: `make -C native GAME=mw2 bundle` or `GAME=mw3 bundle` (and
`mw2mp` / `mw3mp` for multiplayer) find the Steam install and build a separate
compatibility app in `native/build/`. MW3 campaign reaches gameplay in short
probes; MW3 multiplayer logs in to Activision's servers, but their Mac lobby currently refuses the session, so online play is untested.

Support for the original 32-bit Mac releases:

| Game | Mac release | Status |
| --- | --- | --- |
| LEGO Star Wars: The Video Game | Aspyr, 2005; universal binary update in 2007 (initially PowerPC only) | - |
| LEGO Star Wars II: The Original Trilogy | Aspyr, 2006 | - |
| LEGO Indiana Jones: The Original Adventures | 2008 | - |
| LEGO Batman: The Videogame | Feral, 2009 | - |
| LEGO Star Wars: The Complete Saga | Feral, Steam 1.2.1 | WIP; launcher, menus and new-game cantina verified |
| LEGO Indiana Jones 2: The Adventure Continues | Feral, 2009 | - |
| LEGO Harry Potter: Years 1-4 | Feral, 2011 | - |
| LEGO Star Wars III: The Clone Wars | Feral, 2011 | ✓\* |
| LEGO Pirates of the Caribbean | TransGaming, 2011 | ✓\* |
| LEGO Batman 2: DC Super Heroes | Feral, 2012 | - |
| LEGO Harry Potter: Years 5-7 | Feral, 2012 | - |
| LEGO The Lord of the Rings | Feral, 2013 | - |
| LEGO Marvel Super Heroes | Feral, 2014 | Experimental |
| LEGO The Hobbit | Feral, 2014 | - |
| LEGO Batman 3: Beyond Gotham | Feral, 2014 | - |
| The LEGO Movie Videogame | Feral, 2014 | - |

\* Not verified with a 100% run yet, report any bugs or problems in the issues tab.

## Building

GLMetal is a Git submodule pinned to a tested revision. Initialize it after
cloning or pulling this branch:

```sh
git submodule update --init --recursive
```

The Call of Duty build targets also initialize it automatically when missing.
Prebuilt converters already contain GLMetal and need no repository access.

For a Finder-based COD4 conversion, build the standalone app once:

```sh
make -C native cod4-converter
```

Open `native/build/COD4-Converter.app`, choose **Campaign** or **Multiplayer**,
then click **Find in Steam** or drop in a Mac app/game folder.
Choose an output folder to create a separate compatibility app. The converter
includes the current loader and prepares its private runtime automatically;
using it requires no Python, Make, Xcode, or checkout. Existing converted apps
are preserved by giving new copies numbered names. See the
[COD4 guide](native/COD4.md#converter-app) for requirements and details.

Requirements: macOS 11 or later, Xcode command-line tools (`xcode-select
--install`), Rosetta 2 on Apple Silicon (`softwareupdate --install-rosetta`),
and the original game application. Pirates additionally needs Python 3 (the
one that comes with the command-line tools is fine) and network access the
first time, to fetch the `unicorn` package.

```sh
cd native
make GAME=pirates   SOURCE_APP="/path/to/LEGO Pirates of the Caribbean.app" bundle
make GAME=clonewars SOURCE_APP="/path/to/LEGO Star Wars III.app"           bundle
make GAME=marvel    SOURCE_APP="/path/to/LEGO Marvel Super Heroes.app"     bundle
make GAME=saga      SOURCE_APP="/path/to/LEGO Star Wars Saga.app"         bundle
```

Complete Saga's WIP build and current limitations are documented in
[native/SAGA.md](native/SAGA.md). Short probes reach the new-game cantina;
the full campaign and controllers remain unverified.
`GAME=saga` builds the Steam edition and requires an explicit `SOURCE_APP` path. Its output is
`native/build/LEGOCompleteSaga-Steam-Compat.app`; keep the copied
`LEGOStarWarsSagaData` directory beside it. The retail update is opt-in with
`SAGA_EDITION=retail` and uses a separate bundle.

This compiles the loader and assembles a self-contained app in
`native/build/` (`LEGOPirates-Compat.app`, `LEGOCloneWars-Compat.app`, or
`LEGOMarvel-Compat.app`) with
the game's data, resources and Cg framework copied in. The original app is
only read. Open the built app from Finder or the Dock.

To keep cutscenes and gameplay running when you switch to another app, build
with `CONTINUE_WHEN_INACTIVE=1` (works with `bundle` and `promote-loader`).
This setting is **off by default** and stored in the generated app, so Finder
launches respect it. For personal builds, put `CONTINUE_WHEN_INACTIVE = 1`
in the git-ignored `native/local.mk`; use `CONTINUE_WHEN_INACTIVE=0` on a make
command to override that preference. A process environment override,
`LP32_CONTINUE_WHEN_INACTIVE=0` or `1`, takes priority over the bundle setting.
This does not enable unattended test mode or mute audio. Global keyboard
polls and cursor warps are suppressed while the real app is inactive.
Gameplay continues too, so pause manually before leaving an active level.
Test with `make -C native test-focus-policy test-focus-bridge`.

Pirates ships with a SecuROM-packed executable. The build recovers the plain
Mach-O from it automatically: `native/tools/unpack_securom.py` emulates the
packer's stub with Unicorn and writes `native/build/LEGOPirates.unpacked.macbin`
(the activation code is left intact, nothing is bypassed). The first build
creates a Python virtual environment in `native/build/venv` and installs
`unicorn` into it; to use an interpreter that already has `unicorn`, pass
`PYTHON=/path/to/python3`. Clone Wars and Marvel use their shipped binaries directly.

Marvel supports the Feral 1.0.1 i386 build (`LEGOMarvel.macbin`). Its bundle
copies the source app's x86_64 Steam API and forwards Steam initialization.
Real Steam achievements require a genuine Steam library and Steam account
access to Marvel (app 249130). A replacement library can return successful
initialization without connecting to Steam: the earlier locally tested library
contains the identifying string `Steam Emulator Version`. Those earlier save
tests do **not** verify Steam Cloud. The genuine Steam build has now been
tested separately: initialization succeeds, its success callback reaches the
guest, and the game reads all 48 achievement entries. Uploading a newly earned
achievement and Steam Cloud save/reload remain unverified.
See [native/MARVEL-STEAM.md](native/MARVEL-STEAM.md) for the separate build
command, the startup fix, and verification details.
The same loader detects each
title, with Marvel's thread and structure-return conventions kept in its
own profile. Building Marvel leaves the other compatibility apps in place.
Verified so far: menus, the opening sequence, and keyboard movement and
Hulk/Bruce Banner transformation in Sand Central Station. Checkpoint files
persist on disk; a bundled storage-library enumeration defect hid later slots
after relaunch. The shared loader now repairs that directory walker, and fresh
processes discover and fully read the existing saves. In-game resume and the
full campaign remain unverified.
Marvel's achievement submitter now skips a missing Steam stats interface
instead of dereferencing NULL at `0x249374`. This is a crash guard, not Steam
integration or an achievement retry queue. An unlock attempted while Steam
is unavailable is not guaranteed to be uploaded later. Run
`make -C native test-steam-achievement-guard` to exercise the mapped guest
routine with NULL, disabled, successful, and failed mock interfaces without
unlocking achievements or loading saves.
The loader supplies the i386 character tables and Cg metadata queries needed
for Marvel's shader constants. On the first launch after this fix, it backs up
Marvel's `CachedShadersGL` folder alongside the original and rebuilds the cache;
this corrects black intro logos and missing brick meshes. Saves are unaffected.

The shared Cocoa bridge tracks owned and autoreleased string lifetimes instead
of keeping every returned string permanently. This fixes a handle-table leak
that can end in `persistent Objective-C proxy pool exhausted` followed by a
crash. `make -C native test-objc-proxy` checks 100,000 string lifetime cycles,
including retained values surviving pool drains, without launching a game.

The shared dispatcher also avoids searching Carbon's libraries for unrelated
imports, caches native Carbon exports, and uses the fast path for both GL
symbol spellings and the existing atomic-operation aliases. This removes
dispatch overhead introduced while expanding Complete Saga support, without
disabling that port. `make -C native GAME=marvel test-carbon-dispatch` checks
foreign-call rejection, native export caching, and real guest atomic calls.
Structure-return classification is also cached per import, preserving each
profile's stack convention and Steam's argument-dependent return handling.
`make -C native GAME=marvel test-import-return` checks static and dynamic imports.

The storage repair is selected by the SDK library's UUID and a SHA-256 match
of the entire defective routine, independently of the game profile. It keeps
the parent directory path intact during recursion; it does not invent slot
names or replace the SDK catalog. Only private process memory changes, and
unrecognized library builds are left intact. Run `make -C native
test-steam-storage-fix` for native filesystem regressions using temporary
fixtures (override `STEAM_STORAGE_LIBRARY` to select the affected dylib).
`LP32_STEAM_STORAGE_PROBE=1` on a compatibility app initializes its normal SDK,
enumerates and reads saves without running the game or issuing save writes,
and preserves the player's `last-run.log`.

Other targets: `make GAME=<game> promote-loader` replaces only the loader in
an existing bundle (no source app needed), `make icons` regenerates the Dock
icons from `native/icons/`, and `make` alone builds the loader and probes.

Saves and settings stay where the original games put them
(`~/Library/Application Support/...`). Controllers supported by the
GameController framework work through the games' Xbox 360 mapping, including
two-player co-op and controllers connected during play. Button prompts show PlayStation glyphs (✕ ○ □ △, L1/R1,
Select/Start) when the pad connected at launch is a DualShock/DualSense, and
Xbox glyphs otherwise; `LP32_BUTTON_GLYPHS=playstation|xbox` forces one.
Marvel currently uses its shipped Xbox controller mapping and prompts.

For silent testing, launch the bundle's executable with `LP32_MUTE_AUDIO=1`.
This mutes only that process and does not change game settings or system volume.

Marvel automatically records occasional frame hitches during normal play in
`~/Library/Logs/LEGOMarvelCompat/hitches-<pid>-<timestamp>.log`. Relaunch after
updating the loader to enable it. Each report contains 32 preceding frames,
the slow frame, and 8 following frames, with draw counts, CPU time in draws,
resource uploads, shader compilation, file I/O, waits, audio, GL state changes,
Objective-C calls and other runtime imports. Nested imports count only toward
their own categories; calls spanning presentation are omitted. It also
records the three slowest measured calls per frame (guest caller address,
vertex/fragment program IDs and draw count), plus recent slow worker calls.
Presentation time is split into work, drawable flush, and deliberate pacing.
These are CPU wall timings; they do not measure GPU execution or replay draws.
Log headers include the Mac model, chip, RAM, macOS version, Rosetta status,
loader build time and dispatch overrides. The session log also records the
native GL renderer and initial power/thermal state. Native crashes include a
bounded raw stack trace by default, with the loader UUID and load address in
the session header for matching a report to its build.
Steam storage calls are included in I/O attribution. Save requests, filenames,
byte counts, enumeration results and SDK return values also appear as
`compat32: save ...` lines in `last-run.log` for normal Finder/Dock launches
(stderr for terminal launches), capped at 1024 lines per session. These logs
do not contain save payloads and do not change storage behavior.

The shared bridge reuses handles for repeated native pointer queries, including
OpenGL contexts, instead of allocating a permanent handle on every query.
`make -C native test-pointer-proxy` checks concurrent queries, context switching,
and resource lifetime without launching a game.

Guest `pthread_once` now synchronizes each control independently and publishes
completion in its i386 control word. Completed calls avoid the old global
mutex and 256-entry scan. Run `make -C native test-guest-once test-guest-once-abi`
for concurrency, retry, storage reuse and real i386 import checks. See the
[shared runtime measurements](native/SHARED-RUNTIME-PERFORMANCE.md) for the
generic GLMetal vertex-cache changes and the limits of the gameplay comparison.

The recorder uses fixed memory and a background log writer, with no screenshot
capture or GPU readback. Its timestamps use the Mach uptime clock directly,
with nanosecond conversion checked against `CLOCK_UPTIME_RAW` in the tests.
Reports trigger above 25 ms (or 1.5 times an intentional
frame cap, whichever is larger), with a five-second cooldown and a limit of 128
reports per session. Inactive frames do not trigger reports. A report finishes
after its following frames arrive; abrupt termination can lose the pending
report. `LP32_HITCH_MS=35` changes the threshold; `LP32_HITCH_LOG=0` disables it,
or set `LP32_HITCH_LOG` to an unused absolute file path to redirect it. Other
titles leave it disabled unless explicitly enabled.
`make -C native test-hitch-recorder` runs synthetic timing tests without launching a game.
