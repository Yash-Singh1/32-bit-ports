# Call of Duty 2 (Mac)

Aspyr's **1.3.1 Steam Mac release** (build 111540, app 2630) runs in the same
i386-in-x86_64 loader as COD4. Both COD games share Aspyr's Carbon engine layer:
`CMacGameEngine`, the Direct3D-on-OpenGL wrapper, AGL display ports, ARB
programs and the Game Guide launcher. The loader treats them as one family
(`lp32_title_is_cod_carbon()` in `src/game_profile.h`). Campaign and
multiplayer each get a separate compatibility app.

## Validation status

The Steam build is experimental. Short muted, background probes on macOS 26.5.2
with an Apple M5 Pro reached gameplay in both editions:

- **Campaign:** Steam initialization, the intro logos and in-engine intro
  movie, profile creation, the main menu, and the Moscow briefing. The first
  mission then started and played: world, characters, HUD, compass,
  objectives, an automatic checkpoint save, and forward movement.
- **Multiplayer:** the intro movie, profile creation, and the main menu. A
  local Toujane deathmatch server started from the console, followed by team
  and weapon selection and a spawn. Movement and firing worked. The original
  i386 PunkBuster modules load through the guest loader.

Not verified: full campaign progression, audible output, controllers, Internet
servers, PunkBuster-protected servers, mod downloads, and long sessions.

## Building

Requires the private i386 C++ runtime (`make cod4-runtime`, shared with COD4)
and the Steam installation, or an explicit app:

```sh
cd native
make cod4-runtime
make GAME=cod2 bundle
make GAME=cod2mp bundle
make GAME=cod2 SOURCE_APP="/path/to/Call of Duty 2.app" bundle
```

The output apps are `build/COD2-Compat.app` and `build/COD2MP-Compat.app`.
Multiplayer is copied from the `Call of Duty 2 Multiplayer.app` inside the
single-player app. Each output contains its own copy of
`Call of Duty 2 Data` (about 3.6 GB). `tools/bundle_cod4.py --game cod2` does the
conversion and never modifies the source. After loader changes, run
`make GAME=cod2 promote-loader` (or `cod2mp`).

The standalone converter apps do not offer COD2 yet.

## Apple OpenGL by default, GLMetal opt-in

COD2 apps use Apple's OpenGL, and the bundler does not package GLMetal for
them. GLMetal also runs both editions. Short muted probes on GLMetal reached
the same points as on Apple GL: the campaign menu, the Moscow intro and
movement, and in multiplayer a Toujane spawn, firing and movement. The menu
matched Apple's frame almost pixel for pixel. To try it on a COD2 app:

```sh
LP32_GL_BACKEND=metal LP32_GLMETAL_PATH=/path/to/glmetal/build/libGLMetal.dylib \
  build/COD2-Compat.app/Contents/MacOS/COD2Compat
```

This needs a GLMetal build with these changes, made in the GLMetal repo for
COD2:

- **Aux buffers.** Aspyr's Direct3D wrapper renders into the default
  framebuffer's auxiliary buffers. It requests `AGL_AUX_BUFFERS 2` with
  `AGL_AUX_DEPTH_STENCIL` and uses `GL_AUX0`/`GL_AUX1` for draws, reads,
  `glCopyTexSubImage2D` and clears. GLMetal now offers up to two aux buffers in
  legacy pixel formats, as Apple does. Aux depth/stencil buffers are given only
  with `kCGLPFANoRecovery`, as on Apple. Before this, every pixel-format
  attempt failed, and the game crashed on a NULL swap chain.
- **ARB generic attributes.** The ARB vertex programs read the D3D vertex colour
  from `vertex.attrib[5]`, which shares the fog coordinate's slot. GLMetal read
  it as the scalar fog coordinate, which left only the red channel. It now
  reads all four components. A location-assignment bug in GLMetal's shader
  compiler also moved generic ARB inputs off their bound locations; that is
  fixed too.

`GLMETAL_DEBUG=1` reports a pixel format GLMetal refuses, with its
attributes.

## Compatibility changes

- **Profiles.** Both executables are identified by SHA-256, entry point and
  image end. Other builds named `COD2.image`/`COD2MP.image` get a generic
  profile that keeps the family behavior but drops every fixed address.
  `LP32_COD2_GENERIC=1` forces it.
- **Depth capability.** `CDisplayInfo` accepts a renderer only if
  `kCGLRPDepthModes` contains the 24-bit bit. Current Apple GPUs report
  `0x1001` (0 and 32-bit). A signature-checked in-memory patch (SP `0x50b1`,
  MP `0x51f1`) also accepts 32-bit depth, as COD4's patch does. Without it, the
  game finds no display and exits from `main`.
- **Launch.** As for COD4, `-g` skips the obsolete Game Guide, Steam
  proof-of-purchase goes to the real client, and the license-key `puts` is
  redacted.
- **New imports.** The integer forwarder in `gl_core_bridge.c` now handles the
  NV register-combiner, `APPLE_vertex_array_range`/`vertex_array_object`/fence,
  `glPointParameterfvARB` and `glTexEnvi` entry points. `gluOrtho2D` is issued
  as `glOrtho(l, r, b, t, -1, 1)`. The Carbon bridge implements
  `StringToNum`, which is missing from 64-bit Carbon. The resource bridge
  implements `PtrAndHand`.

## Driving probes

`LP32_TEST_KEY_FIFO` now also feeds Carbon presenter windows (COD2 and COD4),
not only Cocoa/SDL ones. Keys arrive through the shared AppKit queue.
Development probes use an isolated home and preferences domain, as for COD4:
`LP32_TEST_HOME_DIR`, `LP32_APPLICATION_SUPPORT_DIR`,
`LP32_COD4_PREFERENCES_ID`, plus `LP32_BACKGROUND_TEST=1 LP32_MUTE_AUDIO=1`.
Useful guest arguments (`LP32_GUEST_EXTRA_ARGUMENT`):
`-g +set com_introPlayed 1` skips the intro movie, and in campaign
`+set monkeytoy 0` enables the console. A map loads only after a player profile
has been selected. On the profile screen, Tab and Right move focus to OK.

## Tests

```sh
make test-game-profile
python3 tests/test_cod4_bundle.py
python3 tests/test_glmetal_packaging.py
make GAME=cod2 test-cod4-initializers     # 11/11, needs build/COD2-Compat.app
make GAME=cod2mp test-cod4-initializers   # 13/13
```
