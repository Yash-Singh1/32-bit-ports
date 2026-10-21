# Asphalt 6: Adrenaline for Mac

This port targets Gameloft's Mac App Store release, bundle identifier
`com.gameloft.asphalt6mac`. The tested archive contains `Asphalt 6.app`,
version 1.0.9, with an i386 Mach-O executable and packed data in
`Contents/Resources/data`.

## Build and convert

Download [Asphalt 6 Converter 0.0.0-dev.1](https://github.com/Yash-Singh1/32-bit-ports/releases/tag/asphalt6-converter-v0.0.0-dev.1),
unzip it and open `Asphalt6-Converter.app`. The download includes the loader
and does not require Xcode, Python or a source checkout. It contains no game
data or Apple runtime libraries. This development build uses ad hoc signing
and is not notarized.

Build the separate Asphalt converter:

```sh
make -C native asphalt6-converter
```

Open `native/build/Asphalt6-Converter.app` and select the original Mac app,
a folder containing it, a ZIP archive, or a disk image. Choose an output
folder to create `Asphalt6-Compat.app`. The converter accepts Asphalt 6 only.
Gangstar keeps its own converter and output app.

The converter contains a universal native interface, the Intel compatibility
loader and an optional GLMetal renderer. Apple OpenGL is the default.
The first conversion prepares Apple's i386 C++ libraries
using the same verified download cache as the other converters. A fresh cache
requires a 4.72 GB download from Apple. Later conversions reuse the libraries.
Rosetta 2 is required on Apple Silicon.

For conversion without the interface:

```sh
native/build/Asphalt6-Converter.app/Contents/MacOS/Asphalt6Converter \
  --convert /path/to/Asphalt.6.zip /path/to/output
```

Add `--metal` to test GLMetal, or `--cache DIR`
to choose a runtime download cache. New conversions use numbered names if an
output already exists. ZIP extraction uses a temporary directory, and disk
images mount read-only. Conversion leaves the original source unchanged.

The development bundle target also accepts these source types:

```sh
make -C native GAME=asphalt6 SOURCE_APP=/path/to/Asphalt.6.zip bundle
```

Use `ASPHALT6_GL=metal` to test GLMetal or `GLMETAL_SOURCE=/path/to/glmetal`
to use a separately built GLMetal checkout. After runtime changes,
`make -C native GAME=asphalt6 promote-loader` updates an existing app's loader.

## Runtime

`Asphalt6.image` selects a separate `LP32_TITLE_ASPHALT6` profile. Asphalt
shares the i386 libstdc++ binding, guest setjmp/longjmp, Cocoa class initialization
and EAGLView context attachment with Gangstar. Asphalt uses native Cocoa input
and GLSL shaders. It does not use Gangstar's SDL event pump or its fullscreen
coordinate fixes.

The legacy Objective-C bridge handles root classes without superclass names
and connects Asphalt's nib outlets to the delegate's guest object storage.
Its screen geometry record contains nine floats passed by value.
`AbsoluteToDuration` preserves both words of the guest's 64-bit time argument.
The retained CVDisplayLink remains stopped because the game renders from NSTimer.

The Asphalt-only GLSL adapter renames the reserved `GL_ES` macro while keeping
its desktop value and conditions. It also removes a stray bracket in the
shipped texture shader's `#endif]` directive. Shader failure previously caused
the engine to load its failing fallback recursively until its stack overwrote
a worker stack. The original game files stay unchanged. The shared GL bridge
now forwards active attribute/uniform queries and the float arguments of
`glVertexAttrib4f`. Darwin i386 `div` returns its quotient in EAX and remainder
in EDX, which the race clock uses.

Settings use `com.gameloft.asphalt6mac.compat`. The generated app logs to
`~/Library/Logs/Asphalt6Compat/last-run.log`.

## Validation

Experimental. Background, muted probes on macOS 26.5.2 and an Apple M5 Pro
reach the title screen, the animated garage menu, Free Race selection and
a Nassau normal race with Apple OpenGL. A four-minute engine probe exercised
keyboard acceleration, steering, braking and boost input, reaching 118 km/h.
The race continued after the clock's `div` call without unsupported imports
or shader compilation errors. Repeated engine probes used shortened movie
fixtures. A separate startup probe played both original intro movies through
to the title screen. The converted app retains those original movies.

GLMetal plays the movies but still rejects some legacy material shaders, so
it remains an opt-in experiment. Audio playback, controllers, multiplayer,
full races and long sessions remain unverified. Some rendering still needs
visual review, including dark car paint in races and a narrow corrupt strip
at the top of captured frames. The Mac App Store receipt
path is untested; the supplied 1.0.9 archive contains no receipt.

The Dev 1 release ZIP was extracted and matched the built converter, including
file permissions. Both converter architectures, version 0.0.0 build 1 and
strict signatures passed. With developer tools excluded from PATH, the
extracted converter converted the supplied ZIP using an existing runtime
cache. The output retained the original executable and all 5,192 resource
files, selected Apple OpenGL and passed signature verification.

```sh
make -C native test-asphalt6-converter test-asphalt6-glsl test-game-profile test-gl-core-bridge test-objc-proxy
```

Automated game launches should use `LP32_BACKGROUND_TEST=1 LP32_MUTE_AUDIO=1
LP32_HEADLESS_ERRORS=1`. Give probe copies a separate executable name and bundle
identifier, and set `LP32_NO_DIAGNOSTIC_LOG=1`, to isolate test saves and logs.
`LP32_TEST_KEY_FIFO` accepts the same scripted input as the other ports.
`LP32_AGL_CAPTURE_FRAME=/path/to/frame` captures GL frames after the movie views.
