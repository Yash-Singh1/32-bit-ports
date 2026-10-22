# 32-bit Mac games on modern macOS

A native loader that runs original 32-bit Intel Mac games on
current macOS (Apple Silicon through Rosetta 2, or Intel). It maps the
game's i386 executable into a 64-bit process, switches into a 32-bit code
segment to run it, and bridges the OS, OpenGL, Cg, CoreAudio and controller
interfaces the game expects. No SIP changes, kernel extensions, VMs or Wine.
You need your own copy of the game.

Supported and candidate games:

| Game | Mac release | Status |
| --- | --- | --- |
| LEGO Star Wars II: The Original Trilogy | Aspyr, 2006 | - |
| LEGO Batman: The Videogame | Feral, 2009 | - |
| LEGO Indiana Jones 2: The Adventure Continues | Feral, 2009 | - |
| LEGO Star Wars III: The Clone Wars | Feral, 2011 | ✓\* |
| LEGO Pirates of the Caribbean | TransGaming, 2011 | ✓\* |
| LEGO Batman 2: DC Super Heroes | Feral, 2012 | - |
| LEGO Harry Potter: Years 5-7 | Feral, 2012 | - |
| LEGO Batman 3: Beyond Gotham | Feral, 2014 | - |
| The LEGO Movie Videogame | Feral, 2014 | - |
| Portal 2 | Valve, 2011 | Experimental: main menu and first level render; gameplay checks ongoing |
| Left 4 Dead 2 | Valve, 2010 | Experimental: loader, bundle and movie playback; gameplay checks ongoing |

\* Not verified with a 100% run yet, report any bugs or problems in the issues tab.

## Left 4 Dead 2

L4D2 support uses the original **Mac** game files and the installed Steam client.
From `native/`, build a separate compatibility app with:

```sh
make GAME=l4d2 SOURCE_GAME="$HOME/Library/Application Support/Steam/steamapps/common/Left 4 Dead 2" bundle
```

The default `SOURCE_GAME` is that Steam installation, so `make GAME=l4d2 bundle`
also works there. Use a complete installed or assembled Mac game folder; the
L4D2 helper does not combine separate depot downloads. The Portal 2 GUI converter
continues to convert Portal 2 only. Open `native/build/L4D2-Compat.app` from Finder,
or use `make GAME=l4d2 run-game`. The original files are only read.

The initial validation goal is sustained offline campaign play with bots,
graphics, audio, keyboard/mouse input, map transitions, and settings retained
after relaunch and rebuild. Online multiplayer, voice chat, controllers, and
Workshop compatibility are not established by loader or bundle tests.
The QuickTime shim uses AVFoundation for the Mac build's supplied movies;
startup intro video and audio have been confirmed in the game window.
Build requirements, diagnostic launches, and test instructions are in
[native/L4D2.md](native/L4D2.md).

## Portal 2 converter

The `portal2` branch includes a small native Mac app. Drop your original
**Portal 2.app**, **Steam game folder**, or **depot download folder** onto **Portal2-Converter.app** (or open the converter and click
**Choose Portal 2…**), choose an output folder, and wait for **Open Game**.
There is no separate validation step. You can cancel while it works.

For a Steam installation, select `steamapps/common/Portal 2` (selecting the
Steam library folder also works). For separate DepotDownloader downloads,
select their parent folder, such as `/Volumes/Storage/games/mac/portal2`.
The converter combines the shared content in depot **621** with the Mac client
in depot **623**, including `bin/osx32`. You do not need to make an `.app`
bundle or combine the folders yourself. If several versions are present,
choose a specific version folder; ambiguous downloads are not mixed.

Steam copies require an account that owns Portal 2. The launcher opens Steam
when needed and waits for it before starting the converted game.
The loader bridges the game's 32-bit Steam interfaces to the installed
64-bit Steam client. It does not remove ownership or authentication checks.
Download
[Portal 2 Converter 0.0.0-dev.6](https://github.com/Yash-Singh1/lego-mac-compat/releases/tag/portal2-converter-v0.0.0-dev.6)
or build the converter from this branch. Folder support requires dev.2 or
newer; the older dev.1 release accepts app bundles only.
The supplied depot build has been checked through the opening single-player
room. Full playthrough and online features remain unverified.

Dev.6 fixes the confirmed deferred-physics crash: destroyed entities could
remain queued after their memory was reused. It also starts Steam automatically
and records allocation, physics, and memory evidence for future crashes.
The physics fix verifies the supported server UUID and instruction bytes before
patching; other server versions retain general crash capture only. Regression
tests reproduce the original crash without the fix and preserve live physics
updates with it. The original challenge/reload sequence still needs an in-game
retest. Dev.5's import diagnostics and native macOS crash reports remain included.
The reported co-op loading stall remains unresolved.
Unreleased source changes add opt-in send-table/class compatibility controls
and Steam tracing from [issue #10](https://github.com/Yash-Singh1/lego-mac-compat/issues/10).
See the [co-op setup and remaining authentication gap](native/PORTAL2.md#experimental-cross-build-co-op-issue-10);
these changes do not suppress Steam authentication failures.

Dev.4 fixes Steam controller data conversion and the missing PNG decompression
needed for controller button glyphs. DualShock 4 buttons and sticks have been
confirmed working in-game. It also fixes Workshop filename pointer corruption,
tag-array conversion, and other Steam ABI defects. The filename fix is a
candidate for issue #7's dev.3 crash; confirmation from the reporter is still
needed. Remaining Steam API gaps are listed in [native/STEAM_ABI.md](native/STEAM_ABI.md).
The earlier audio/libc fixes and logs in `~/Library/Logs/Portal2Compat/` remain
included. The intermittent Core Audio allocator crash remains under
investigation; real microphone capture and in-game voice chat need testing.

The converter needs no Make, Python, Xcode, or Terminal to use. It includes the
compiled compatibility loader and automatically downloads the required Apple
runtime on its first conversion. It runs on macOS 11 or later; the converted
game needs Rosetta 2 on Apple Silicon. Compatibility is experimental, with the
same limitations as the command-line build; this does not guarantee support
for every future macOS release.

Your original app or downloaded files stay untouched. Each conversion creates a new
`Portal2-Compat.app`; if that name already exists, it uses a numbered name.
Saves and settings inside the selected source are copied with it. Progress in a
previously converted copy stays in that copy; the converter never overwrites
or merges it. Portal 2 keeps its saves under the converted app's
`Contents/SharedSupport/Portal2/portal2/SAVE/`, so keep that app when rebuilding.

The first conversion downloads Apple's 4.72 GB Lion archive and extracts two
libraries without running the installer or changing system libraries. Files
are cached in `~/Library/Caches/org.32bitgoofy.Portal2Converter/`. Allow room
for the roughly 11–13 GB output and about 15 GB of download/extraction space on
the first run; later conversions reuse the cached libraries.

To build the converter once from this checkout (requires Xcode command-line
tools):

```sh
cd native
make portal2-converter
```

The result is `native/build/Portal2-Converter.app`. It contains no game data or
Apple runtime libraries and can be moved out of the checkout. Local builds
are ad-hoc signed; a public download still needs Developer ID signing and
notarization for normal Gatekeeper distribution.

## Building

Requirements: macOS 11 or later, Xcode command-line tools (`xcode-select
--install`), Rosetta 2 on Apple Silicon (`softwareupdate --install-rosetta`),
and the original game application (or Portal 2 Mac game files). Pirates additionally needs Python 3 (the
one that comes with the command-line tools is fine) and network access the
first time, to fetch the `unicorn` package. Portal 2 additionally needs Python 3
and network access for the automatic, one-time runtime download performed by
`make portal2-runtime`. You do not need to obtain or install the runtime manually.

```sh
cd native
make GAME=pirates   SOURCE_APP="/path/to/LEGO Pirates of the Caribbean.app" bundle
make GAME=clonewars SOURCE_APP="/path/to/LEGO Star Wars III.app"           bundle
make portal2-runtime
make GAME=portal2   SOURCE_APP="/path/to/Portal 2.app"   bundle
make GAME=portal2   SOURCE_GAME="/path/to/portal2"       bundle
```

For Portal 2, `make portal2-runtime` downloads Apple's 4.72 GB Lion installer
and extracts the two required 32-bit C++ libraries automatically. It caches the
download in `native/build/runtime-downloads/` and places the libraries in
`native/build/guest-runtime/`. The Portal 2 bundle target also prepares the runtime automatically, then copies it inside
`Portal2-Compat.app`. Later runs verify and reuse the cached libraries. The
installer is never run, and your Mac's system libraries are not changed.

This compiles the loader and assembles a self-contained app in
`native/build/` (`LEGOPirates-Compat.app`, `LEGOCloneWars-Compat.app`) with
the game's data, resources and Cg framework copied in. The original app is
only read. Open the built app from Finder or the Dock. Portal 2 builds
`Portal2-Compat.app`; its current limitations and development checks are
documented in [native/PORTAL2.md](native/PORTAL2.md). Portal 2 rebuilds assemble
a fresh game tree and preserve the output's existing `SAVE` folder and local
`config.cfg`, `video.txt`, and `videodefaults.txt`; stale libraries from another
release are removed. The GUI always produces a separate numbered app.

Pirates ships with a SecuROM-packed executable. The build recovers the plain
Mach-O from it automatically: `native/tools/unpack_securom.py` emulates the
packer's stub with Unicorn and writes `native/build/LEGOPirates.unpacked.macbin`
(the activation code is left intact, nothing is bypassed). The first build
creates a Python virtual environment in `native/build/venv` and installs
`unicorn` into it; to use an interpreter that already has `unicorn`, pass
`PYTHON=/path/to/python3`. Clone Wars uses its shipped binary directly.

Other targets: `make GAME=<game> promote-loader` replaces only the loader in
an existing bundle (no source app needed), `make icons` regenerates the Dock
icons from `native/icons/`, and `make` alone builds the loader and probes.

LEGO saves and settings stay where the original games put them
(`~/Library/Application Support/...`). For the LEGO profiles, controllers supported by the
GameController framework work through the games' Xbox 360 mapping, including
two-player co-op and controllers connected during play. Button prompts show PlayStation glyphs (✕ ○ □ △, L1/R1,
Select/Start) when the pad connected at launch is a DualShock/DualSense, and
Xbox glyphs otherwise; `LP32_BUTTON_GLYPHS=playstation|xbox` forces one.
