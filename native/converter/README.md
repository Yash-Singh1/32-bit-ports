# Portal 2 Converter development

`make portal2-converter` builds a universal AppKit application with a small
drag-and-drop window. `App.swift` owns the UI and runs `Conversion.swift` on a
worker queue. Cancellation stops the current child process and removes only
the operation's private staging directory. Quitting waits for that cleanup.
There is no shell-interpolated command execution and no developer-tool
dependency in the shipped app. Apple's supplied macOS tools perform copying,
archive extraction and code signing; Swift handles SHA-256 and i386 thinning.

`Source.swift` shares app/Steam/depot discovery and copying with the Makefile
helper in `tools/portal2_source.swift`. `Conversion.swift` follows the existing `GAME=portal2 bundle` recipe and
`tools/prepare_portal2_runtime.py`. Keep the URL, archive/library digests and
bundle layout in sync with those paths. It intentionally uses the stock
loader and Rosetta configuration, including the existing performance fixes.

Run the independent regression checks with:

```sh
make test-portal2-converter
```

The built converter also exposes the same engine without opening any GUI:

```sh
build/Portal2-Converter.app/Contents/MacOS/Portal2Converter \
  --convert "/path/to/Portal 2.app or Steam game folder" "/path/to/output folder"
```

An optional `--cache /path/to/cache` isolates integration tests from the
user's normal cache. To exercise extraction without downloading another
4.72 GB, put the verified Lion download there as `InstallMacOSX-Lion.dmg`.
The code still checks its SHA-256 and extracts both libraries. SIGINT/SIGTERM
use the same cancellation mechanism as the window's Cancel button.

For the full test with a real game and the existing download (Python is only
needed to run this development test):

```sh
python3 tests/test_portal2_converter_integration.py \
  --source "/path/to/Portal 2.app" \
  --installer build/runtime-downloads/InstallMacOSX-Lion.dmg
```

This test builds in a temporary folder under `build/`, checks the produced
bundle and loader without launching the game, repeats with cached libraries,
cancels a copy, and checks that the source and previous output are untouched.
It removes its own test files afterward. It does not automate the UI. Manual
UI checks: window and Dock-icon drops, Choose Game, folder-picker dismissal,
Cancel during download/copy, Quit during conversion, error recovery, Open
Game and Show in Finder. A public release needs Developer ID signing and
notarization separately from the local ad-hoc build target.

For flat or split Steam depots, use the dedicated real-file integration test:

```sh
python3 tests/test_portal2_depots.py --source /path/to/portal2
```

It uses the prepared `build/guest-runtime` cache, verifies conversion and a
Makefile rebuild with both depots, and preserves the source and existing apps.
The source chooser accepts app bundles and folders; Dock drops declare both
UTIs. For a Steam installation, choose the **Portal 2** game folder inside
`steamapps/common`. The default Mac location is
`~/Library/Application Support/Steam/steamapps/common/Portal 2`; an installation
on another drive uses that drive's Steam library instead. The window and source
picker explain this, and the picker starts in the default game folder when it
contains `portal2_osx`. Selecting a Steam library or the parent of `depots/621` and `depots/623`
is sufficient. Multiple ambiguous versions produce an actionable error.
Steam copies require the installed Steam client to be running and signed in.
The bundled loader bridges Steam's i386 interfaces to that native client; see
`../PORTAL2.md` for tested behavior and remaining interface limitations.

## Portal Stories: Mel converter

`make mel-converter` builds `build/PortalStoriesMel-Converter.app` using the
same window and conversion engine, with Mel's own app identity and source picker.
Choose a complete Mac game folder (`steamapps/common/Portal Stories Mel`) or a
Mac app containing that tree. Separate depots must be assembled first.
`L4D2Source.swift` contains the shared complete-tree helper used by both L4D2
and Mel; their game directories, launcher names and retained settings are
selected by separate profiles.

The headless entry point is:

```sh
build/PortalStoriesMel-Converter.app/Contents/MacOS/PortalStoriesMelConverter \
  --convert "/path/to/Portal Stories Mel" "/path/to/output folder"
```

`--cache` works as described above. Each conversion creates a separate numbered
app and copies progress from the selected source. Make rebuilds of the same
Mel output retain its saves and local settings. Targeted packaging regressions:

```sh
make test-mel-source test-l4d2-source test-portal2-converter
```

See [Mel notes](../../docs/PORTAL-STORIES-MEL.md) for build commands and validation.
