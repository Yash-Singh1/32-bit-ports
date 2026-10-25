Fixes COD4's "Set Optimal Settings?" prompt stopping at `_RunStandardAlert`,
as reported on macOS 27. This release also addresses additional missing calls
found while auditing the supplied issue #13 executable and the Steam campaign
and multiplayer executables.

Download and unzip **COD4-Converter-0.0.0-dev.6.zip**, then open
**COD4-Converter.app**. Reconvert your original game to include the new loader.
Keep any previous converted app and its saves.

### Changes since dev.5

- Implemented native standard-alert execution with actual button results,
  correct 16-bit output, and cleanup of disposed dialog references.
- Added HTTP run-loop sources with guest callback and ownership conversion.
  Added HTTP response inspection, authentication and credential handling,
  streamed uploads, stream availability/errors, and proxy configuration
  script evaluation used by ASIHTTPRequest.
- Added missing launcher control lookup, values, visibility and reference
  data; menu event targets and item enabling; mouse-event view lookup and
  theme cursors; display-width queries; and temporary-directory and form
  encoding calls.
- Added a repeatable import inventory tool and documented remaining gaps in
  [the COD4 import audit](https://github.com/Yash-Singh1/32-bit-ports/blob/cod4-converter-v0.0.0-dev.6/native/COD4-IMPORT-AUDIT.md).
  The scan covers 727 callable imports in the supplied image, 687 in Steam
  campaign and 737 in Steam multiplayer. It is not a complete compatibility
  certification.

### Validation and limits

Native tests exercise all three standard-alert buttons, launcher controls in
legacy and composited windows, display dimensions, run-loop source callbacks,
HTTP Basic authentication against a local fixture, invalid authentication,
proxy scripts, stream errors and packed guest outputs. Regression checks cover
the supplied image's factory/IOKit/encryption startup paths, Objective-C
ownership and strings, legacy OpenSSL, Steam campaign and multiplayer
initializers, and standalone campaign/multiplayer conversion.

Local validation ran on macOS 26.5.2 under Rosetta. macOS 27 has not been tested
locally. The Mac App Store working status from dev.5 remains reported; these
tests do not establish complete menu, mission, network or receipt-validation
coverage. Legacy movie recording, clipboard, some launcher drawing/focus and
multiplayer text paths, cache/MIME helpers, and other calls listed in the audit
remain incomplete. Custom modal filters and version-one run-loop sources are
not supported by the new handlers.

GLMetal remains pinned to the same revision as dev.5. This release does not
claim to resolve the reported intermittent audio static or mesh stretching.

Requires macOS 11 or later and your own original 32-bit Intel Mac game files.
Converted games require Rosetta 2 on Apple Silicon. Steam editions require
Steam and ownership of the game. The first COD4 conversion downloads Apple's
Lion archive to extract two runtime libraries; later conversions reuse the
cache. The converter needs no Xcode, Python, or source checkout.

The release ZIP contains no game executable, game data, or Apple runtime
libraries. This is an ad hoc signed development prerelease without Developer
ID signing or notarization.
