# Fast Five for Mac

This port targets Gameloft's Mac App Store release of Fast Five: The Movie
Official Game, bundle identifier `com.gameloft.fastfivemac`. The tested
archive, `Fast_Five_1.0.zip`, contains `FastFive.app`, version 1.0.0, with an
i386 Mach-O executable and packed data in `Contents/Resources/res`.

## Build and convert

Download [Fast Five Converter 0.0.0-dev.1](https://github.com/Yash-Singh1/32-bit-ports/releases/tag/fastfive-converter-v0.0.0-dev.1),
unzip it and open `FastFive-Converter.app`. The download includes the loader
and GLMetal and does not require Xcode, Python or a source checkout. It
contains no game data or Apple runtime libraries. This development build
uses ad hoc signing and is not notarized.

Build the separate Fast Five converter:

```sh
make -C native fastfive-converter
```

Open `native/build/FastFive-Converter.app` and select the original Mac app,
a folder containing it, a ZIP archive, or a disk image. Choose an output
folder to create `FastFive-Compat.app`. The converter accepts Fast Five only;
Asphalt 6 and Gangstar keep their own converters and output apps.

The converter contains a universal native interface and the Intel
compatibility loader, with an optional GLMetal renderer. Apple OpenGL is the
default. The first
conversion prepares Apple's i386 C++ libraries using the same verified
download cache as the other converters. A fresh cache requires a 4.72 GB
download from Apple; later conversions reuse the libraries. Rosetta 2 is
required on Apple Silicon.

For conversion without the interface:

```sh
native/build/FastFive-Converter.app/Contents/MacOS/FastFiveConverter \
  --convert /path/to/Fast_Five_1.0.zip /path/to/output
```

Add `--metal` to use GLMetal, or `--cache DIR` to choose a
runtime download cache. New conversions use numbered names if an output
already exists. ZIP extraction uses a temporary directory, and disk images
mount read-only. Conversion leaves the original source unchanged.

The development bundle target accepts the same source types:

```sh
make -C native GAME=fastfive SOURCE_APP=/path/to/Fast_Five_1.0.zip bundle
```

After runtime changes, `make -C native GAME=fastfive promote-loader` updates
an existing app's loader.

## Runtime

`FastFive.image` selects a separate `LP32_TITLE_FASTFIVE` profile in the
Gameloft family. Fast Five shares Asphalt 6's Glitch engine and Cocoa layer
(`EAGLView`, `CustomWindow`, `VideoView`): the i386 libstdc++ binding, guest
setjmp/longjmp, Cocoa class initialization and EAGLView context attachment.
Its bundled SDL 1.2 framework is used only for joysticks, which the loader's
SDL bridge answers natively, so the converter does not copy it.

The engine's shader preamble defines the reserved `GL_ES` macro, as in
Asphalt 6. Current GLSL compilers reject it, and the engine then retries its
fallback shader recursively until the main thread's stack runs into a worker
thread's stack (the crash appears on the worker, returning to `0x9b`). Fast
Five therefore uses the same GLSL adapter as Asphalt 6.

Imports first needed by Fast Five: `nearbyint`/`nearbyintf`, the
`SCNetworkReachability` notifier (`SetCallback`, `ScheduleWithRunLoop`,
`UnscheduleFromRunLoop`), `pthread_testcancel` (a host cancellation point, as
`pthread_cancel` cancels the host thread) and `__cxa_call_unexpected`
(terminates, like GCC's default unexpected handler). The game's DNS thread
uses `pthread_testcancel` around `gethostbyname`.

Its `applicationDidBecomeActive:` calls `makeKeyAndOrderFront:` again.
Background test runs simulate activation, and now notify only when the
simulated focus changes, as AppKit does; repeating the notification looped.

The game keeps its saves in `~/Library/<CFBundleName>`, so the converter
keeps `CFBundleName` (`FastFive`). Settings use
`com.gameloft.fastfivemac.compat`, and the generated app logs to
`~/Library/Logs/FastFiveCompat/last-run.log`. At startup the original game
contacts Gameloft's in-game advertising server.

## Validation

Experimental. Muted background probes on macOS 26.5.2 and an Apple M5 Pro,
with Apple OpenGL and an isolated copy (own executable, bundle identifier and
`CFBundleName`), reach the intro movie, the title screen, the main menu,
track, race type and garage selection, and a Rio normal race at 1920x1200.
Holding the up arrow drove the car through the first lap. 108 shaders
compiled without errors.

GLMetal reaches the same Rio race. It needs GLMetal 8a9d42b or later, the
`native/glmetal` pin. Legacy shaders
may name a sampler `texture`, declare a varying once per preprocessor branch
or qualify it through a precision macro, and only uniforms a shader reads or
initializes are active, as on Apple's OpenGL. Without the last, the engine's shader
parameter table had a null entry (crash at `0x799793`). GLMetal's
`make test-legacy-reflection` covers these cases. A GLMetal app converted
with the bundled driver also reached the menus, garage and tutorial prompt.

Audio, controllers, multiplayer, full races, career mode
and long sessions remain unverified, as does the Mac App Store receipt path.

```sh
make -C native test-fastfive-converter test-game-profile test-asphalt6-glsl test-gl-core-bridge test-objc-proxy test-display-bridge
```

Automated game launches should use `LP32_BACKGROUND_TEST=1 LP32_MUTE_AUDIO=1
LP32_HEADLESS_ERRORS=1 LP32_NO_DIAGNOSTIC_LOG=1` and a probe copy with its own
executable name, bundle identifier and `CFBundleName`. With the default
fullscreen 1920x1200 window, scripted `LP32_TEST_KEY_FIFO` clicks (window
coordinates, origin bottom left) reach a race: `960 600` (continue),
`1128 960` (Quick Race), `1512 996` twice (Rio, Day; Normal Race),
`1776 115` (OK) and `816 576` (Skip Tutorial). Key `126` accelerates.
`LP32_AGL_CAPTURE_FRAME=/path/to/frame` captures GL frames.
