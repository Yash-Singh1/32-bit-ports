# 32-bit LEGO games on modern macOS

Run the original 32-bit Intel Mac LEGO releases on macOS 11 or later,
including Apple Silicon through Rosetta 2. You need your own copy of each game.

| Game | Mac release | Status |
| --- | --- | --- |
| LEGO Star Wars: The Video Game | Aspyr, 2005; universal binary update in 2007 (initially PowerPC only) | - |
| LEGO Star Wars II: The Original Trilogy | Aspyr, 2006 | - |
| LEGO Indiana Jones: The Original Adventures | 2008 | - |
| LEGO Batman: The Videogame | Feral, 2009 | - |
| LEGO Star Wars: The Complete Saga | Feral, Steam 1.2.1 | ✓\* |
| LEGO Indiana Jones 2: The Adventure Continues | Feral, 2009 | - |
| LEGO Harry Potter: Years 1-4 | Feral, 2011 | - |
| LEGO Star Wars III: The Clone Wars | Feral, 2011 | ✓\* |
| LEGO Pirates of the Caribbean | TransGaming, 2011 | ✓\* |
| LEGO Batman 2: DC Super Heroes | Feral, 2012 | - |
| LEGO Harry Potter: Years 5-7 | Feral, 2012 | - |
| LEGO The Lord of the Rings | Feral, 2013 | - |
| LEGO Marvel Super Heroes | Feral, 2014 | ✓\* |
| LEGO The Hobbit | Feral, Steam 1.0 | Experimental |
| LEGO Batman 3: Beyond Gotham | Feral, Steam 1.0.3 | Experimental |
| The LEGO Movie Videogame | Feral, Steam 1.0 | Experimental |

\* Not verified with a 100% run yet, report any bugs or problems in the issues tab.

## Building

Run `make converter` from `native/` to build `native/build/LEGO-Converter.app`.
Drop or select an original Mac app or Steam game folder, choose the game if
needed, then convert. Compiler tools are included. See the
[converter notes](native/converter/README.md) for supported releases and building.

Requirements:

- macOS 11 or later.
- Xcode command-line tools, installed with `xcode-select --install`.
- Rosetta 2 on Apple Silicon, installed with `softwareupdate --install-rosetta`.
- The original game application, fully installed.
- For Pirates, Python 3 and network access for the first build's `unicorn` dependency.

The build detects the game from the source app's bundle identifier:

```sh
cd native
make SOURCE_APP="/path/to/LEGO The Hobbit.app" bundle
```

You can also select the game explicitly:

```sh
make GAME=pirates   SOURCE_APP="/path/to/LEGO Pirates of the Caribbean.app" bundle
make GAME=clonewars SOURCE_APP="/path/to/LEGO Star Wars III.app"            bundle
make GAME=marvel    SOURCE_APP="/path/to/LEGO Marvel Super Heroes.app"      bundle
make GAME=saga      SOURCE_APP="/path/to/LEGO Star Wars Saga.app"          bundle
make GAME=batman3   SOURCE_APP="/path/to/LEGO Batman 3.app"                 bundle
make GAME=movie     SOURCE_APP="/path/to/The LEGO Movie.app"              bundle
make GAME=hobbit    SOURCE_APP="/path/to/LEGO The Hobbit.app"              bundle
```

The generated compatibility app appears in `native/build/`. The build copies
in the game's data and resources and leaves the original app unchanged.
Open the generated app from Finder or the Dock.

Fullscreen launch placement records the frontmost app's visible window before
game initialization, then falls back to the display under the pointer. Later
fullscreen frame changes keep the game on its current display. The Cocoa bridge queries
[`NSScreen.mainScreen`](https://developer.apple.com/documentation/appkit/nsscreen/main)
on each call. Quartz's primary display stays independent of the game window,
and emulated display modes retain the selected display's desktop origin.
`LP32_DISPLAY_INDEX`, `LP32_TEST_DISPLAY` and `LP32_USE_SECONDARY_DISPLAY`
remain explicit display overrides for test launches.

Batman 3, The LEGO Movie, and The Hobbit default to their standard Steam
library locations when `GAME` is specified without `SOURCE_APP`. For other
install locations, pass `SOURCE_APP` explicitly. To check game detection before
building, run `make SOURCE_APP="/path/to/Game.app" game-info`.

`GAME=saga` defaults to the Steam edition and requires `SOURCE_APP`. Its output
is `native/build/LEGOCompleteSaga-Steam-Compat.app`; keep the generated
`LEGOStarWarsSagaData` directory beside it. The retail edition uses
`SAGA_EDITION=retail`. See [native/SAGA.md](native/SAGA.md) for edition-specific
build instructions and [native/MARVEL-STEAM.md](native/MARVEL-STEAM.md) for
Marvel's Steam build instructions.

Pirates automatically unpacks its shipped executable during the build. The
first build creates `native/build/venv` and installs `unicorn`. To use an
existing Python environment with `unicorn`, pass `PYTHON=/path/to/python3`.

To update the loader in an existing compatibility app without copying the
game data again:

```sh
make GAME=hobbit promote-loader
```

Add `CONTINUE_WHEN_INACTIVE=1` to `bundle` or `promote-loader` to keep the game
running while another app is active. It defaults to `0`. To save a personal
build preference, put `CONTINUE_WHEN_INACTIVE = 1` in the git-ignored
`native/local.mk`.

Startup diagnostics are saved to `~/Library/Logs/LP32Compat/startup.log`
before the game image loads. After loading, the session log is in the game's
log directory, such as `~/Library/Logs/LEGOCloneWarsCompat/last-run.log`.
Include these logs when reporting a launch failure.

Display regression checks can run without loading a game or changing the
system display layout:

```sh
make -C native test-display-bridge
```

The check exercises changing Cocoa focus, Quartz primary-display queries,
launch placement, fullscreen window moves, and emulated mode isolation on
all attached displays.
