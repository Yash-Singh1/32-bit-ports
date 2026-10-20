# Gangstar: Miami Vindication (Mac)

Gameloft's 2011 Mac App Store release (`GangstarMV.app`, bundle identifier
`com.gameloft.gangstar2mac`, version 1.0.0) is a 32-bit Intel Cocoa app built on
Gameloft's Glitch/glf engine. It runs in the same i386-in-x86_64 loader as the
COD titles, with OpenGL through GLMetal by default.

## Validation status

Experimental. Muted, background probes on macOS 26.5.2 with an Apple M5 Pro
reach gameplay on both GLMetal and Apple's OpenGL:

- the intro movie (`intro.mov`, played through the QTKit stand-in), the title
  screen and the main menu with its animated 3D backdrops;
- New Game and Continue, the in-engine cutscenes (Skip works), and the open
  world. HUD, minimap and tutorial prompts render, and WASD and the arrow
  keys move the player;
- at 960×600, GLMetal and Apple GL both hold about 58–60 fps while walking.
  Captured frames match.

Not yet verified: long sessions, missions beyond the opening, controllers,
1920×1200 fullscreen frame rates, and the Mac App Store edition's receipt check.
The tested copy had none. Out of roughly 20 unattended launches, the game
quit itself twice on reaching the menu, both times while another copy of the
game was running. The game's `windowWillClose:` calls `[NSApp terminate:]`.
`LP32_TRACE_TERMINATE=1` prints the native stack when that happens.

## Building

The app, its folder, or the release's disk image can be the source. A disk
image is attached read-only during the copy and detached again. The i386 C++
runtime is shared with COD4:

```sh
cd native
make cod4-runtime
make GAME=gangstar SOURCE_APP=/path/to/Gangstar_Miami_Vindication.dmg bundle
make GAME=gangstar SOURCE_APP=/path/to/GangstarMV.app GANGSTAR_GL=apple bundle
```

The output is `build/Gangstar-Compat.app`. `tools/bundle_gangstar.py` does the
conversion and never modifies the source. GLMetal comes from `GLMETAL_SOURCE`
(the `glmetal` submodule by default, or for example
`GLMETAL_SOURCE=~/p/projects/glmetal`). After loader changes, run
`make GAME=gangstar promote-loader`.

## Converter app

`make gangstar-converter` builds `build/Gangstar-Converter.app`. Drop in
`GangstarMV.app`, its folder or the `.dmg`, choose GLMetal (default) or Apple
OpenGL, and choose an output folder. New conversions get numbered names, so an
existing converted app and its saves are never replaced. The converter is
built from the COD converter's shared code (`cod4-converter/Conversion.swift`):
the first conversion on a Mac downloads Apple's 4.72 GB Lion archive to extract
the two i386 runtime libraries, and later conversions, including COD ones,
reuse that cache. For testing without the GUI:

```sh
build/Gangstar-Converter.app/Contents/MacOS/GangstarConverter --convert SOURCE OUTPUT_FOLDER [--apple-gl] [--cache DIR]
```

## How it runs

- **Title profile:** `LP32_TITLE_GANGSTAR`, selected by the image name
  `Gangstar.image`. The 10.6 SDK crt1's `start` stub is recognised to find
  `main`.
- **C++:** every mangled import, plus the vtable and typeinfo references in the
  image's `LC_DYLD_INFO` bind stream, binds to Apple's i386 `libstdc++`
  (`macho_image32_bind_compressed`). The image's own weak definitions (such as
  `appleRootCA()`) stay in place.
- **SDL 1.3:** the bundled `SDL.framework` has guest Objective-C classes, which
  the guest library loader can't map. `src/sdl_bridge.m` implements the 23
  imported calls natively. Joysticks are GameController pads. `SDL_PumpEvents`
  drains AppKit on the main thread; this pump drives the `EAGLView` render timer
  and delayed performs while the game loop holds the main thread. It also turns
  key events into SDL 1.3 keyboard events, which the game reads with
  `SDL_PeepEvents` (types 0x300–0x303).
- **Cocoa:** the game's `GameWindow` replaces the loader's TransGaming
  stand-in of the same name (now registered as `LP32CiderGameWindow`). Guest
  `+initialize` runs for this title: `GameDelegate` registers its screen
  defaults there. `EAGLView`, a plain NSView, attaches its GL context in
  `-lockFocus`, which layer-backed AppKit never calls. The loader attaches the
  context when the game first uses it (`attach_owning_view`). QTKit's `QTMovie` and
  `QTMovieView` come from `src/qtkit_bridge.m` on AVFoundation.
- **Display:** the game switches modes with a display configuration
  transaction. Each step succeeds and the native mode stays in effect.
- **Other bridges:** object out-parameters (`NSError **`) get a host slot. When
  the game passes an object as its own `error:` slot (its IGP cache write), the
  error is dropped instead of AppKit writing into that object. IOKit matching
  and registry results follow the Create rule. `stringByAppendingFormat:`,
  `appendFormat:` and NSLog format their varargs. `getifaddrs`, `malloc_size`,
  wide-character memory functions, `CVDisplayLink` setup and
  `SCNetworkReachability` were added.

The game contacts Gameloft's in-game-ads server on start, as the original does.
Settings live in the `com.gameloft.gangstar2mac.compat` defaults domain. The
game's IGP cache is `~/Library/GangstarMV`.

## Testing

Automated runs must stay in the background and muted:

```sh
LP32_BACKGROUND_TEST=1 LP32_MUTE_AUDIO=1 LP32_HEADLESS_ERRORS=1 \
LP32_TEST_KEY_FIFO=/tmp/keys LP32_AGL_CAPTURE_FRAME=/tmp/frame LP32_AGL_CAPTURE_EVERY=60 \
  arch -x86_64 build/Gangstar-Compat.app/Contents/MacOS/GangstarCompat
```

Write `KEYCODE HOLD_SECONDS [MODIFIERS [X Y]]` lines (macOS virtual key codes;
1000–1002 are left/right/other clicks at window point X,Y) to the FIFO. Frames
are written as PPM. Set `LP32_GL_BACKEND=metal|apple` to choose the renderer when
launching the executable directly. Finder launches use the app's
`LSEnvironment`. `LP32_TRACE_SDL=1` logs every SDL call. `LP32_TRACE_QTKIT=1`
logs the movie stand-in. Saves live in `~/Library/<CFBundleExecutable>/`: give test
copies their own executable name and bundle identifier, and set
`LP32_NO_DIAGNOSTIC_LOG=1`, so they leave the player's saves and log alone.
