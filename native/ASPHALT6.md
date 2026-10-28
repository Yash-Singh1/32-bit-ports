# Asphalt 6: Adrenaline for Mac

This port targets Gameloft's Mac App Store release, bundle identifier
`com.gameloft.asphalt6mac`. The tested archive contains `Asphalt 6.app`,
version 1.0.9, with an i386 Mach-O executable and packed data in
`Contents/Resources/data`.

## Build and convert

Download [Asphalt 6 Converter 0.0.0-dev.4](https://github.com/Yash-Singh1/32-bit-ports/releases/tag/asphalt6-converter-v0.0.0-dev.4),
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

## Optional iPad textures

Dev 4 accepts an optional Asphalt 6: Adrenaline HD **1.5.1 iPad IPA**
alongside the original Mac game. Conversion also works with the Mac game
alone. Click **Add iPad IPA…**, or drop the IPA into the converter, then
choose the original Mac app, ZIP or disk image and an output folder.
The IPA supplies track textures; the Mac release supplies the executable
and the rest of the game.

```sh
native/build/Asphalt6-Converter.app/Contents/MacOS/Asphalt6Converter \
  --convert /path/to/Asphalt.6.zip /path/to/output \
  --ipad-assets /path/to/Asphalt_6__Adrenaline_HD_1.5.1_ios_3.2.ipa
```

The importer selects existing track textures with matching base names and
aspect ratios, including the iPad's larger `_x2` variants. It decodes PVRTC
and encodes DXT3 DDS textures with the supplied mipmaps and alpha. It never
resizes images to manufacture larger textures. `_IPAD2` names remain distinct
because their atlas layouts have not been verified against the Mac version.
The Mac pack's entry names stay intact. The game code, meshes, shaders, UI,
resource index and saves continue to come from the Mac copy.

The supplied 1.5.1 IPA yields 118 larger Mac pack entries from 59 iPad
textures. Some entries are aliases for the same texture with different
compression or mipmap names. The supplied packs share 371 exact entry
names. SHA-256 hashes match for 57 entries; 311 pairs use DDS on Mac and
PVRTC on iPad, two use TGA on Mac and PVRTC on iPad, and one TGA pair differs.
Different hashes across these formats do not establish different artwork.

Every import saves `Contents/Resources/Asphalt6-iPad-textures.json` inside
the output app. It lists hashes for all 855 Mac and 613 iPad track pack
entries, exact-name matches, selected replacements, dimensions and hashes
of the converted DDS entries. Hashes cover each decompressed archive entry,
including the texture header and its compressed image data.

The packaged importer is native and universal; users need no additional
tools or downloads for texture conversion. Its PowerVR decoder and stb
color encoder retain their upstream licenses under `Resources/ThirdParty`.

```sh
make -C native test-asphalt6-ipad-assets test-asphalt6-converter
```

The importer has been tested with generated constant-color and alpha
textures and with the supplied IPA. Actual gameplay appearance remains
unverified; no game code was launched for the import checks.

The development bundle target also accepts these source types:

```sh
make -C native GAME=asphalt6 SOURCE_APP=/path/to/Asphalt.6.zip bundle
```

Use `ASPHALT6_GL=metal` to test GLMetal or `GLMETAL_SOURCE=/path/to/glmetal`
to use a separately built GLMetal checkout. After runtime changes,
`make -C native GAME=asphalt6 promote-loader` updates an existing app's loader.

## 4K scene rendering and MSAA

Dev 4 includes the scene layer. New conversions automatically render the
3D scene at 3840×2160 and request
8× MSAA. This is built into the Mac loader. No mod archive, Windows files,
or additional import step is needed.

The scene layer scales the game's offscreen textures and depth/stencil
buffers, redirects drawing to multisampled framebuffers, and resolves them
into the original scene textures before compositing. The original
2048×1024 padded allocations become 6144×3072, while their 1280×720 scene
viewport becomes 3840×2160. Normalized texture coordinates keep the same
ratios. Window size, HUD coordinates and mouse input retain their existing
coordinate system.

The driver limits the sample request to supported hardware. Logs report the
actual sample count allocated. If multisample allocation fails, the loader
keeps the high-resolution scene and reports that MSAA could not be enabled.
This mode uses more GPU memory and rendering time than the original scene.

For lower-cost settings, override `LP32_ASPHALT6_SCENE_SCALE=2` for a
2560×1440 scene and `LP32_ASPHALT6_MSAA=4` for four samples. Scale 1 and
MSAA 0 restore the original rendering path. Finder launches read the defaults
from the converted app's `LSEnvironment`; direct executable launches can use
ordinary environment overrides.

Offscreen GPU fixtures verify full-resolution pixels beyond the original
1280×720 area, eight actual samples, fractional edge coverage, color and
depth resolves, downsampled readback, scene-to-texture copies, logical
viewport/scissor queries, reallocation and deletion. They pass with legacy
and core Apple OpenGL contexts and GLMetal. A synthetic image also checks
both fast and chained loader import dispatch with each backend. No game
was launched for this implementation; gameplay appearance and performance
still need validation.

```sh
make -C native test-asphalt6-scene test-asphalt6-scene-metal test-asphalt6-scene-bridge
```

See [the Windows mod comparison](ASPHALT6-WINDOWS-MOD.md) for the static
analysis that informed this implementation.

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

Asphalt's Full setting uses a borderless window covering the selected display.
The bridge replaces its menu-bar-reduced `visibleFrame` and subsequent centering
with the display bounds, or the safe area below a camera housing. The renderer
and mouse mapping receive the same screen geometry. Window mode keeps the
game's normal sizing and title bar.
The hidden startup window is centered explicitly on the chosen display.
Calling AppKit's `center` here can move it back to the primary monitor before
the game changes it to borderless, even after its frame was placed correctly.
The title-to-menu artifact came from late attachment of the GL drawable.
The original game issues black color clears while initializing its renderer,
but the bridge previously attached its plain GL view only on a later
`makeCurrentContext` or `flushBuffer`, with retries throttled to every 30 calls.
Those first clears targeted `GL_NONE` and had no window color buffer.
The shared Cocoa bridge now attaches a plain view's context synchronously
after `setContentView:` installs it. The original initialization and clears
then target `GL_BACK`. No Asphalt-specific clear or rendering rewrite is used.

Launch placement uses the frontmost app's first visible normal window,
choosing the display with the largest overlap. If that window cannot be
identified, it uses the display under the pointer, then AppKit's main screen.
Explicit `LP32_DISPLAY_INDEX`, `LP32_TEST_DISPLAY` and
`LP32_USE_SECONDARY_DISPLAY` settings take precedence. After launch, windowed
resizes and Full keep the window on its current monitor, including monitors
with negative desktop coordinates.

This placement policy does not redefine the system's primary display.
Without an explicit display override, `NSScreen.mainScreen` calls AppKit each
time and `CGMainDisplayID` returns Quartz's primary display. Emulated mode
changes belong to the display ID passed by the game and preserve that
display's desktop origin.

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

## Controllers

Experimental controller support for the original Mac 1.0.9 executable is
included from the Dev 3 converter. Convert the game again, or use
`make -C native GAME=asphalt6 promote-loader` to update an existing
development app. The adapter uses the first connected
device with a GameController.framework extended gamepad profile.

- Left stick steers proportionally, with an 18% dead zone. D-pad Left/Right
  uses the game's digital steering.
- RT accelerates; LT brakes. Triggers are digital for now and retain the
  game's existing auto-acceleration setting.
- LB or RB holds the Space/nitro action.
- X cycles camera presets forward; Y cycles backward.
- Start/Menu, Back/Options, or B sends Escape for pause/back.
- Right stick moves an in-game cursor. A clicks at that cursor.
- Outside active racing, including pause, the left stick and D-pad also move
  the cursor. Coordinates follow the current view and window size.

Keyboard and controller holds combine, so releasing a trigger cannot release
an arrow key still held on the keyboard. Short presses and releases span
separate input commits. Camera/back actions also serialize with each other
because the game processes only one of their releases per update.
Disconnect clears controller input; focus loss clears buffered input and
releases held actions. Physical controller input requires an active app and
key game window, even when background rendering is enabled.

Analog steering retains the original digital routine's bookkeeping and the
shared physics setter. It applies only to the local keyboard-controlled car
in digital steering mode. Drift and reverse retain the original digital path.
Controller feel and these transitions still need an in-game hardware check.

`LP32_ASPHALT6_ANALOG_STEERING=0` selects digital stick steering.
`LP32_ASPHALT6_CONTROLLERS=0` disables this adapter and both hooks.
`LP32_TRACE_CONTROLLERS=1` logs device connections and delivered actions.
An unrecognized executable hash or changed call-site instructions disables
the hooks and keeps the original keyboard/mouse handling.

Run `make -C native test-asphalt6-controller` for windowless input and Cocoa
event tests, plus a synthetic i386 fixture that checks nested x87 returns,
stack alignment, and float-stack balance. These tests use no game code or
assets. See [the controller research](ASPHALT6-CONTROLLERS.md) for addresses
and the Windows implementation comparison.

## Validation

Experimental. Background, muted probes on macOS 26.5.2 and an Apple M5 Pro
reach the title screen, the animated garage menu, Free Race selection and
a Nassau normal race with Apple OpenGL. A four-minute engine probe exercised
keyboard acceleration, steering, braking and boost input, reaching 118 km/h.
The race continued after the clock's `div` call without unsupported imports
or shader compilation errors. Repeated engine probes used shortened movie
fixtures. A separate startup probe played both original intro movies through
to the title screen. The converted app retains those original movies.

The fullscreen correction was checked through the Visuals menu's Full to
Window to Full transition in a muted background probe. Full used a borderless
1920x1200 window at the display origin, with a matching GL view. Window mode
restored the title bar and smaller frame. Menu clicks worked after resizing.

Display regression tests checked changing AppKit focus independently of
Quartz's primary display, invalid override fallback, foreground-window
selection, emulated modes on each display, desktop origins and cross-monitor
window moves. Game probes also ran the Full to Window to Full menu sequence
on both physical displays. A test fixture simulated foreground-window focus
without activating the game or changing the primary monitor. On the secondary
display the launch ID was 1 while Quartz's primary remained 2; the window
covered its 1728x1085 safe area at x=-1728. The primary display used 1920x1200
at the origin. Captured menu frames and scripted mouse clicks passed.

A real Finder double-click on the secondary display exposed a startup case
the focus fixture missed. The launch selected display 1, but native centering
moved the hidden titled window to display 2. Repeating the same mouse launch
after the correction kept the window on display 1 through the borderless
transition and intro playback. The regression test now starts with a hidden
titled window sized for the primary display while targeting each monitor.
That test fails with the previous native-centering implementation and passes
with explicit placement.

Full-window captures verified black borders around the menu at 1728x1085
with a viewport of `{0,42,1728,999}` on the secondary display, produced by the
game's own drawing after the drawable-attachment correction. The original
title keeps its complete copyright. A regression test installs a plain GL
content view and verifies its first clear reaches actual window pixels before
any present call. It fails with the old attachment behavior. Capture
diagnostics can include the unused borders with
`LP32_AGL_CAPTURE_WHOLE_WINDOW=1`; `LP32_TRACE_GL_CLEARS=1` logs clear targets,
write masks and call sites from initialization onward.
Fresh launches passed on both displays: the secondary's menu had 44 black
rows above and 42 below, and the primary's 1920x1200 window had 51 above and
49 below. Both launches' first original color clear targeted `GL_BACK`.
The original full-length intro movies also led to a clean menu, and Full to
Window to Full passed after the attachment correction.

The display API rules follow [Apple's NSScreen reference](https://developer.apple.com/documentation/appkit/nsscreen/main)
and [Quartz display documentation](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/QuartzDisplayServicesConceptual/Articles/Overview.html).

GLMetal plays the movies but still rejects some legacy material shaders, so
it remains an opt-in experiment. Audio playback, physical-controller gameplay, multiplayer,
full races and long sessions remain unverified. Some rendering still needs
visual review, including dark car paint in races and a narrow corrupt strip
at the top of captured frames. The Mac App Store receipt
path is untested; the supplied 1.0.9 archive contains no receipt.

The Dev 2 release ZIP was extracted and matched the built converter, including
file permissions. Both converter architectures, version 0.0.0 build 2 and
strict signatures passed. With developer tools excluded from PATH, the
extracted converter converted the supplied ZIP using an existing runtime
cache. The output retained the original executable and all 5,192 resource
files, selected Apple OpenGL and passed signature verification.

The Dev 4 release ZIP also matched the built app's bytes and permissions.
Build 4, universal interface/importer architectures and strict signatures
passed. With developer tools excluded from PATH, the extracted converter
converted the Mac ZIP both alone and with the optional 1.5.1 IPA. Both
outputs retained the Mac executable and enabled 4K/8× MSAA defaults. The
Mac-only output retained the original texture pack. The IPA output replaced
118 entries and preserved the other 737 entries; all archive entries passed
CRC checks and replacement hashes matched the report. These checks did not
launch the game.

```sh
make -C native test-asphalt6-converter test-asphalt6-glsl test-game-profile test-gl-core-bridge test-objc-proxy test-display-bridge
```

Automated game launches should use `LP32_BACKGROUND_TEST=1 LP32_MUTE_AUDIO=1
LP32_HEADLESS_ERRORS=1`. Give probe copies a separate executable name and bundle
identifier, and set `LP32_NO_DIAGNOSTIC_LOG=1`, to isolate test saves and logs.
`LP32_TEST_KEY_FIFO` accepts the same scripted input as the other ports.
`LP32_AGL_CAPTURE_FRAME=/path/to/frame` captures GL frames after the movie views.
