The Mac App Store version of Call of Duty 4 is now working with the converter, alongside the Steam edition. This development release includes the fixes from issue #13 and the COD4 runtime and GLMetal updates since dev.4.

Download and unzip **COD4-Converter-0.0.0-dev.5.zip**, then open **COD4-Converter.app**. Select Campaign or Multiplayer, choose the original Mac game app or use Find in Steam, and choose an output folder. Reconvert the original installation to get these fixes; keep any previous converted app and its saves.

### Changes since dev.4

- Added Mac App Store 1.7.2 support. Fixed premature Objective-C object deallocation during startup, including autorelease before a guest pool exists, superclass autorelease calls, and temporary guest objects returned by native code.
- Fixed IOKit data ownership that could leave a guest handle pointing to freed native data and crash during `getBytes:length:`.
- Implemented the legacy OpenSSL initialization, memory BIO, PEM public-key decoding, and EVP encryption calls used by the App Store startup request. The old encryption ABI now calls the matching native entry point.
- Added support for older Objective-C constant-string layouts.
- Improved `last-run.log` diagnostics with receiver/class details, bounded Objective-C message and lifetime histories, native stack records, and image identifiers for crash analysis. The release enables these records automatically.
- Included shared AudioQueue and AudioUnit callback lifetime fixes and checks that reject guest memory mappings outside the 32-bit address range.
- Included guest once/mutex synchronization fixes and reduced shared bridge and hitch-recorder overhead.
- Updated the bundled GLMetal driver and native shader compiler, including index streaming and buffer lifetime fixes, pipeline preparation and synchronization changes, faster vertex/sampler handling, and expanded legacy OpenGL compatibility. These changes do not establish that every reported rendering or audio issue is resolved.

### Validation

The supplied issue #13 executable passes its request factory/retain and IOKit autorelease/read checks with explicit and fallback guest pools. Its original startup routine completes request encryption and reaches the checkpoint immediately before asynchronous networking. The local test stops there without sending a request. The Mac App Store working status is reported; local verification does not cover its complete menu, mission, or network flow.

Regression checks cover actual RSA encryption/decryption, malformed PEM and oversized inputs, Objective-C ownership and string layouts, persistent native-crash logs, converter packaging, and the Steam campaign and multiplayer initializers. The converter packages GLMetal and its shader compiler and needs no Xcode, Python, or source checkout.

Requires macOS 11 or later and your own original 32-bit Intel Mac game files. Converted games require Rosetta 2 on Apple Silicon. Steam editions still require Steam and an account that owns the game. The first COD4 conversion downloads Apple's Lion archive to extract two runtime libraries; later conversions reuse the cache.

The release ZIP contains no game executable, game data, or Apple runtime libraries. This remains an ad hoc signed development prerelease without Developer ID signing or notarization.
