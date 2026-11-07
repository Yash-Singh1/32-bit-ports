# 32-bit Mac games on modern macOS

A native loader for original 32-bit Intel Mac games on current macOS,
with native Intel execution and Rosetta 2 on Apple Silicon. It maps the
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
| Star Wars: The Force Unleashed | Aspyr, version 1.2 | Work in progress on `tfu`: executable initialization passes; platform bridges under development |

\* Not verified with a 100% run yet, report any bugs or problems in the issues tab.

The `tfu` branch builds on `portal2`. Its current status, input layout, and
build commands are in [native/TFU.md](native/TFU.md). A generated TFU bundle
is not yet a verified playable port.

## Portal 2 converter

The `portal2` branch includes a small native Mac app. Drop your original
**Portal 2.app** onto **Portal2-Converter.app** (or open the converter and click
**Choose Portal 2…**), choose an output folder, and wait for **Open Game**.
There is no separate validation step. You can cancel while it works.

The converter needs no Make, Python, Xcode, or Terminal to use. It includes the
compiled compatibility loader and automatically downloads the required Apple
runtime on its first conversion. It runs on macOS 11 or later; the converted
game needs Rosetta 2 on Apple Silicon. Compatibility is experimental, with the
same limitations as the command-line build; this does not guarantee support
for every future macOS release.

Your original app stays untouched. Each conversion creates a new
`Portal2-Compat.app`; if that name already exists, it uses a numbered name.
Saves and settings inside the source app are copied with it. Progress in a
previously converted copy stays in that copy; the converter never overwrites
or merges it. Portal 2 keeps its saves under the converted app's
`Contents/SharedSupport/Portal2/portal2/SAVE/`, so keep that app when rebuilding.

The first conversion downloads Apple's 4.72 GB Lion archive and extracts two
libraries without running the installer or changing system libraries. Files
are cached in `~/Library/Caches/org.32bitgoofy.Portal2Converter/`. Allow room
for the roughly 11 GB output and about 15 GB of download/extraction space on
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
and the original game application. Pirates additionally needs Python 3 (the
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
```

For Portal 2, `make portal2-runtime` downloads Apple's 4.72 GB Lion installer
and extracts the two required 32-bit C++ libraries automatically. It caches the
download in `native/build/runtime-downloads/` and places the libraries in
`native/build/guest-runtime/`. The bundle command then copies them inside
`Portal2-Compat.app`. Later runs verify and reuse the cached libraries. The
installer is never run, and your Mac's system libraries are not changed.

This compiles the loader and assembles a self-contained app in
`native/build/` (`LEGOPirates-Compat.app`, `LEGOCloneWars-Compat.app`) with
the game's data, resources and Cg framework copied in. The original app is
only read. Open the built app from Finder or the Dock. Portal 2 builds
`Portal2-Compat.app`; its current limitations and development checks are
documented in [native/PORTAL2.md](native/PORTAL2.md).

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
