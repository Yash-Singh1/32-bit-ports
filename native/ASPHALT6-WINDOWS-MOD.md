# A6 Windows mod comparison

Static comparison of the user's `A6.zip`, password `asphalt6`, on 2026-10-28.
No game or DLL was executed. The reference EXE was read from the original
[Windows archive](https://archive.org/details/asphalt-6-win-32) using HTTP
range requests. Texture data was compared against the Mac 1.0.9 track pack.

## Executable

Both executables are 653,824 bytes. Exactly seven bytes differ, in four runs:

- File `0x1dcd1`, Windows VA `0x3001e8d1`: conditional `jle` becomes `jmp`.
- File `0x1dcee`, VA `0x3001e8ee`: `cmovle ecx, ebp` becomes three NOPs.
- File `0x6cc74`: width changes from 1280 to 3840.
- File `0x6cc78`: height changes from 720 to 2160.

The two instruction edits remove the 16:9 fit calculation's dimension clamps.
These are host presentation changes, not new gameplay code. They should not
be applied at those addresses in the Mac image. Our Mac window sizing and
coordinate conversion already use their own Cocoa implementation.

## OpenGL proxies

PE imports, exports, and diagnostics identify this forwarding chain:

```text
Asphalt 6 (4k test).exe
  opengl32.dll
    a6core.dll
      a6core_original.dll
        Windows system OpenGL
```

`opengl32.dll` forwards most exports to `a6core`. Its additional handlers
repair invalid framebuffer/renderbuffer target enums. Configuration also
contains layout and mouse transforms, disabled in the supplied INI.

`a6core.dll` implements a scene-resolution and MSAA layer. Exported GL entry
points and diagnostics describe scaling selected null-data texture allocations
and renderbuffers, scaling viewport/scissor state, redirecting scene draws to
multisampled framebuffers, and resolving them before the game samples the
scene texture. `a6msaa.ini` requests 3840×2160 from a 1280×720 scene viewport,
8 samples, and base targets of 2048×1024 or 1280×720. Sample allocation can
fall back when the driver cannot create the requested target.

`a6core_original.dll` supplies the older proxy's system GL loading, pixel-format
MSAA selection, depth/stencil framebuffer repairs, and diagnostics. Its name
means the previous proxy in this package; it is not Microsoft's original DLL.

The INIs and logging strings establish the intended behavior. Static inspection
does not prove the resulting image quality or frame rate. The native implementation in `src/asphalt_scene.c` now supplies
scene-target tracking and resolves in our GL bridge. Setting a multisampled
window pixel format alone would leave the game's offscreen scene targets
single-sampled. None of these DLLs is installed by the Mac converter.

## Track textures

Both `file000017.dat` files contain 855 entries with the same names. These are
ZIP-like archives with `QM 06 08` local headers and ordinary ZIP central
directories. The mod changes 172 entries; the other 683 retain their CRC and
uncompressed size. All 172 changed entries were decompressed and their CRCs
checked during analysis.

All changed entries remain DXT3 DDS images, despite their `.tga` filenames:

- 102 double both dimensions.
- 68 quadruple both dimensions.
- Two retain their width and double their height.

For example, `Alps_Asphalt.PVRTC2.tga` changes from 1024×1024 with 11 mip
levels to 2048×2048 with 12. The pack grows from 161,334,552 to 480,357,048
bytes. Larger dimensions alone do not establish that the artwork has more
original detail; no claim about the upscaling method is made here.

The textures were analyzed as data. The converter does not import this ZIP
or these textures. It builds 4K scene rendering and MSAA into the Mac loader,
using the user's original Mac resources.

## SHA-256 identities

```text
Baseline Windows EXE
8b184e4bdc21ff9e51b72e58ca0ae0d618088ecd7f12699811989bf05ee87147
Modified Windows EXE
34d458df53518b36881898f222cd7fb9f685fb4b041884e246bf99aa97e83404
opengl32.dll
9d377ea5444f169921d4cd439d959e7cb46529e0fcaf6435b97cb10cf538f913
a6core.dll
7d138d73d4137948edaed315d282bbf4164e0a1ac9215330db9c80a2c55916a9
a6core_original.dll
39ab45d0cf3f0efc7e4a49e39ee8680a940d9967703ffa99a625b158b2e3513b
Original Mac track pack
f2350a6b3ba5e520490f88297c2496b80434a3fd34480293d78cd89062352efa
Modified track pack
90856df98e379aaba0234e7cf8f4cbb552369662ececcbb35687fa4d82112afc
```

Run converter checks with `make -C native test-asphalt6-converter`.
Native rendering checks are `test-asphalt6-scene`,
`test-asphalt6-scene-metal`, and `test-asphalt6-scene-bridge`.
