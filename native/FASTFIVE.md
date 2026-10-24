# Fast Five for Mac

This port targets Gameloft's Mac App Store release of Fast Five: The Movie
Official Game, bundle identifier `com.gameloft.fastfivemac`. The tested
archive, `Fast_Five_1.0.zip`, contains `FastFive.app`, version 1.0.0, with an
i386 Mach-O executable and packed data in `Contents/Resources/res`. That copy
has no App Store receipt, and its receipt check is replaced with `return 1`
(the 6 bytes at `0x4d8e9d`). The App Store copy has the same executable
without that change, plus `Contents/_MASReceipt/receipt`.

## Build and convert

Download [Fast Five Converter 0.0.0-dev.2](https://github.com/Yash-Singh1/32-bit-ports/releases/tag/fastfive-converter-v0.0.0-dev.2),
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

Fullscreen uses a borderless window sized from the main screen's frame,
with the GL view sized to the chosen resolution. The resolution menu switches
the display mode, which stays native here. As with Gangstar:

- On a display with a camera housing, macOS moved the window down by the
  strip's height, which cut off the bottom of the picture. The window now
  covers the screen's safe area, and the game is given that area as the
  screen frame.
- A resolution other than the window's size left the view in a corner, or
  stretched it once the game reinstalled the view as the content view. The
  view is now fitted to the window at the resolution's aspect ratio, on
  black, both when it is sized and when it is reinstalled, and renders at the
  requested size through a CGL backing size.
- The game reads mouse positions from `-[NSEvent locationInWindow]` as points
  of its resolution, so with a smaller resolution the cursor stopped short of
  the right and bottom edges. Positions are now mapped through the fitted
  view to the resolution and clamped to it.

### App Store receipt

The App Store copy validates its receipt at launch and calls `exit(173)` on
any failure, which on a 2011 Mac asked the App Store to fetch a receipt. A
converted copy quit silently. It ran from an app without `_MASReceipt`, and
the check uses Security keychain and `libcrypto.0.9.8` functions that macOS
no longer has. The check:

1. changes to `<bundle>/Contents/_MASReceipt` and reads `receipt`;
2. looks up "Apple Root CA" in `SystemRootCertificates.keychain` for an
   OpenSSL certificate store;
3. decodes the receipt with `d2i_PKCS7_bio`, requires signed data with
   `id-data` content, and calls `PKCS7_verify`, but does not check its result;
4. parses the content with `ASN1_get_object`. The bundle identifier must be
   `com.gameloft.fastfivemac` and the version must equal the app's
   `CFBundleShortVersionString`. The hash must equal SHA-1 of the `en0` MAC
   address (`IOMACAddress` in the I/O Registry), the opaque value and the
   bundle identifier's DER.

The converter copies the receipt into the converted app's `_MASReceipt`. The
loader decodes PKCS7 messages, BER as receipts are encoded, with Security's
CMS decoder into OpenSSL 0.9.8's i386 `PKCS7` and `ASN1_STRING` layouts.
`ASN1_get_object` follows 0.9.8, and `PKCS7_verify` checks the signature and
trust. The keychain lookup reports the keychain as unavailable, which the
game handles. Its certificate store only feeds the unchecked
`PKCS7_verify`.

The hash also needed `-[NSData bytes]` and `-[NSMutableData mutableBytes]`.
The first returned a host address truncated to 32 bits, and the second a
pointer proxy, so SHA-1 read and wrote the wrong memory. `bytes` now returns
a guest copy of host-stored data. `NSMutableData`'s `data`, `new`,
`dataWithLength:` and `dataWithCapacity:` create data stored in guest
memory, so `mutableBytes` writes reach the object.

A receipt issued for another Mac fails the hash, as it did originally.
`exit` now logs the guest return address, and `0x004d9bff` is the hash
comparison.

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

Display probes on the built-in 1728x1117 panel (safe area 1728x1085) used
`LP32_TEST_DISPLAY=1` and `LP32_TRACE_DISPLAY=1`. A fresh profile renders
1728x1117 into a 1678x1085 view. After 1024x768 was saved, the view is
1447x1085 and pillarboxed. After 1920x1200 it is 1728x1080. In each case the
cursor reached the right and bottom edges, and menu clicks hit their targets.
On the external 1920x1200 display, a 1728x1117 resolution saved on the panel
is fitted to 1856x1200.

Without an App Store copy, the receipt path was tested on the tested
executable with its original 6 bytes restored and a receipt signed by a test
certificate, made for this Mac's `en0` address. With it, a converted copy and
an isolated probe passed the check and reached the title and the menu. A
receipt for another address exited with 173 at the hash comparison.
`make test-receipt-bridge` decodes DER and BER test messages.

Audio, controllers, multiplayer, full races, career mode and long sessions
remain unverified, as does a real App Store receipt.

```sh
make -C native test-fastfive-converter test-game-profile test-asphalt6-glsl test-gl-core-bridge test-objc-proxy test-display-bridge test-receipt-bridge
```

Automated game launches should use `LP32_BACKGROUND_TEST=1 LP32_MUTE_AUDIO=1
LP32_HEADLESS_ERRORS=1 LP32_NO_DIAGNOSTIC_LOG=1` and a probe copy with its own
executable name, bundle identifier and `CFBundleName`. With the default
fullscreen 1920x1200 window, scripted `LP32_TEST_KEY_FIFO` clicks (window
coordinates, origin bottom left) reach a race: `960 600` (continue),
`1128 960` (Quick Race), `1512 996` twice (Rio, Day; Normal Race),
`1776 115` (OK) and `816 576` (Skip Tutorial). Key `126` accelerates.
`LP32_AGL_CAPTURE_FRAME=/path/to/frame` captures GL frames.
