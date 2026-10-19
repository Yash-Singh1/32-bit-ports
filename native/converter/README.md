# LEGO Converter

Drop or select an original Mac `.app` or Steam game folder. Check the Game
menu, choose where to save, and click Convert. The menu allows manual game
selection. The converter leaves the original installation unchanged.

Supports Clone Wars, Pirates, Complete Saga Steam and retail, and Marvel.
Batman 3, The LEGO Movie, and The Hobbit support is experimental. Unsupported
releases are rejected. Complete Saga also needs its original data folder;
keep the output app and copied data together.

Conversion works offline on macOS 11 or later without developer tools.
Clone Wars startup has been checked on an Intel Mac running macOS 12.7.6
and on Apple silicon through Rosetta.
Compiler tools and an SDK are included for optional runtime rebuilds on
macOS 14 or later. Generated Intel apps need Rosetta on Apple silicon.

From `native/`, run `make converter` to build `build/LEGO-Converter.app`.
Building needs Xcode/CLT, Python 3.9+, and initial network access for pinned
Unicorn dependencies. The app is ad-hoc signed and not notarized.

Run `python3 tests/test_lego_converter.py` to check conversion using local
fixtures. Logs go to `~/Library/Logs/LEGOConverter`.

The executable also accepts these commands for troubleshooting:

```sh
build/LEGO-Converter.app/Contents/MacOS/LEGOConverter --inspect "/path/to/game folder"
build/LEGO-Converter.app/Contents/MacOS/LEGOConverter --convert "/path/to/Game.app" "/path/to/output" --game clonewars
build/LEGO-Converter.app/Contents/MacOS/LEGOConverter --rebuild-runtime "/path/to/runtime-output"
```
