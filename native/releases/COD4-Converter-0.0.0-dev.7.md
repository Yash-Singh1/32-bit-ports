Fixes the "Set Optimal Settings?" dialog appearing but not accepting clicks
in dev.6. The AppKit alert replacement is reported to get the Steam campaign
running on an M5 with macOS 27.0.1 in
[issue #13](https://github.com/Yash-Singh1/32-bit-ports/issues/13#issuecomment-6395678042).
This release also explicitly dismisses the alert before game loading resumes
to address the reported lingering popup.

Download and unzip **COD4-Converter-0.0.0-dev.7.zip**, then open
**COD4-Converter.app**. Reconvert your original Mac game to include the new
loader. Keep any previous converted app and its saves.

### Changes since dev.6

- Replaced the legacy Carbon standard-alert modal loop with an AppKit alert.
  Preserved the game's text, button labels, default/cancel choices, and 16-bit
  Carbon result values. AppKit retains its own button tags so selections map
  correctly back to the game.
- Prevented application activation from raising the game window over a modal
  alert and taking its keyboard focus.
- Explicitly hid the unused Carbon alert window and dismissed the AppKit
  alert before returning to game initialization. Window dismissal no longer
  depends on the next game event cycle.
- Added checks for modal focus and immediate dismissal, and updated the
  standard-alert regression to exercise the AppKit buttons.
- Added log messages when the AppKit alert opens and returns its result.

### Validation and limits

Local interaction checks cover mouse selection, Return and Escape with a
placeholder splash window on macOS 26.5.2. Automated checks cover all three
alert button results, packed output boundaries, dialog token retirement,
modal focus during activation, and window visibility immediately after the
modal loop returns. Packaged-loader regressions cover Objective-C strings and
ownership, IOKit, legacy OpenSSL, the supplied issue #13 executable's startup
paths, both Steam initializer suites, and standalone campaign/multiplayer
conversion.

The reporter's macOS 27.0.1 log shows the alert returning OK, rendering
starting, and a normal guest exit with no trapped imports. The report confirms
Steam campaign startup and the menu. It does not establish full mission or
multiplayer coverage. The additional immediate-dismissal change has been
checked locally; its effect on the reported macOS 27 delay needs confirmation.

The Mac App Store support and other changes from dev.5/dev.6 remain included.
The remaining gaps in the
[COD4 import audit](https://github.com/Yash-Singh1/32-bit-ports/blob/cod4-converter-v0.0.0-dev.7/native/COD4-IMPORT-AUDIT.md)
remain incomplete. Custom modal filters are unsupported. GLMetal is unchanged
from dev.6; this release does not claim to resolve the reported intermittent
audio static or mesh stretching.

Requires macOS 11 or later and your own original 32-bit Intel Mac game files.
Converted games require Rosetta 2 on Apple Silicon. Steam editions still
require Steam and an account that owns the game. The converter creates an
independent copy of the game files; keep the original or a backup for future
conversions. The first COD4 conversion downloads Apple's Lion archive to
extract two runtime libraries; later conversions reuse the cache. The
converter needs no Xcode, Python, or source checkout.

The release ZIP contains no game executable, game data, or Apple runtime
libraries. This is an ad hoc signed development prerelease without Developer
ID signing or notarization.
