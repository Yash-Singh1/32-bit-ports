# Portal Stories: Mel compatibility experiment

This branch adds a separate Source profile for Portal Stories: Mel (Steam AppID
317400), a bundle plist, and legacy Steam interfaces needed by its Mac engine.
The launcher matches Portal 2's i386 executable. Game data, saves, app identity
and logs are independent of Portal 2. Steam login/ownership checks remain active.

SteamClient017 must use its Mac vtable, excluding the PS3-only accessor and
including the later ParentalSettings slot. RemoteStorage014 and UserStats011
also omit PS3/server-only methods. The legacy generator preprocesses headers
for POSIX/macOS before deriving method ordinals. Unsupported data layouts still
fail explicitly; this is an experiment, not complete support for every SDK call.

## ABI regeneration

`native/src/steam_abi.inc` is checked in, so building needs no SDK download.
`native/tools/generate_mel_steam_abi.py` extends a pre-Mel table using local
reference inputs. The current inputs are preserved in the local workspace's
`archive/portal-stories-mel/sdk-reference/` (not committed).

The inputs originate from Valve's SDK mirror:

- SDK 1.40: https://github.com/rlabrecque/SteamworksSDK/tree/117b1282e83d
- SDK 1.41: https://github.com/rlabrecque/SteamworksSDK/tree/ef207e7697c2

The local input directory contains SDK 1.40 `steam_api.json` and the legacy
interface headers; `steam_api141.json`, `isteamclient017141.h`, and
`isteamhtmlsurface004.h` come from SDK 1.41. The helper reuses the main
generator's type encoders and preserves all existing interface offsets except
the required SteamClient017 correction. It requires Python 3 and Xcode's Clang.

Run from the repository root:

```sh
git show fb8e449ec9ae499bc56c29616af68fc8bfaa9628:native/src/steam_abi.inc > /tmp/mel-baseline.inc
python3 native/tools/generate_mel_steam_abi.py \
  archive/portal-stories-mel/sdk-reference \
  /tmp/mel-baseline.inc /tmp/mel-steam-abi.inc
cmp native/src/steam_abi.inc /tmp/mel-steam-abi.inc
```

The regular generator alone does not yet append these legacy interfaces; use
this extra step when regenerating the table. SDK inputs are needed only to
regenerate this table, not to build the loader or converter. No SDK files are required from the user.

## Validation and limits

Observed on an Intel MacBook Pro: Steam initializes, the complete menu renders,
and the user confirmed graphics settings and actual gameplay work. Gameplay
also exposed recurring crashes in several rendering functions. A separate
local diagnostic build caught a nested qsort comparator entering above the
suspended guest stack, where it could overwrite live rendering locals. Callback
placement now follows the suspended stack when the fixed 64 KiB stride would
overlap it. A 96 KiB i386 stack-frame fixture with qsort -> bsearch nested
callbacks reproduces the old overlap and checks both sorting and intact locals.
After the correction, the user reported a further session without crashes.
The diagnostic session log records approximately 26 minutes of runtime and a
normal exit with no stack-overlap, heap-corruption or fatal-signal markers.
The full campaign, controllers, Apple Silicon and longer-term stability remain
untested; this is evidence for the fix, not complete compatibility coverage.
The Portal 2 UUID-specific deferred-physics patch does not apply to Mel's
server build and is intentionally skipped.

The guest-loader regression suite includes real i386-to-native calls covering
SteamClient017 slots 23/33/36, User019/Utils009, and RemoteStorage014 slot 43
with a 64-bit return value. `LP32_TRACE_FILES=3` enables pre-open path logging
for local diagnostics; ordinary runs do not enable it.

Consolidation checks (2026-11-01): `make -C native all`,
`native/build/mode32_probe`, `make -C native test-guest-dyld`, and
`make -C native test-source-steam-startup` passed. Legacy ABI regeneration
matched the checked-in table byte-for-byte; the bundle plist and
`git diff --check` passed. The installed game was not relaunched during
consolidation; the gameplay and platform limits above still apply.

Callback-stack correction checks (2026-11-01): the 96 KiB suspended-frame
fixture reproduced the overlap on the old loader. Full native builds and
`test-guest-dyld` passed after the correction, both with diagnostic redzones /
poisoning enabled and in the normal loader. The installed diagnostic app was
updated; the subsequent gameplay result is recorded above.

## Branch relationship

`fix/guest-callback-stack` contains only the generic callback-stack correction
and its i386 regression fixture, based on upstream `portal2`.
`feat/portal-stories-mel` builds on that commit and adds Mel's game profile,
bundle metadata and legacy Steam ABI support. No diagnostic instrumentation,
local archive, SDK input files or private logs are included in either branch.

The Mel branch can be submitted with the callback fix included; a separate
upstream merge of the generic fix is not a prerequisite. The reproducer explains
why Mel needs it, without claiming that other games or platforms reproduce it.

## Building and converting

Build the standalone converter from `native/`:

```sh
make mel-converter
```

Open `native/build/PortalStoriesMel-Converter.app`, drop the complete original
Mac game folder onto it (or choose it), and select an output folder. For Steam,
select `steamapps/common/Portal Stories Mel`. A Mac app containing the same game
tree is also accepted. Separate depot downloads must first be assembled into
one complete folder containing `portal2_osx`, `portal_stories/gameinfo.txt`, and
the Mac `launcher.dylib`. Windows-only installations cannot be converted.

The converter creates a separate `PortalStoriesMel-Compat.app`, using a numbered
name if one already exists. It copies saves and settings from the selected
source, never merges progress from another converted copy, and leaves the
original game untouched. Keep the converted app that contains your progress.
Runtime preparation uses the same verified Apple download and private cache as
the other converters; the shipped converter needs no developer tools.

For command-line builds:

```sh
cd native
make GAME=mel SOURCE_GAME="/path/to/Portal Stories Mel" bundle
make GAME=mel SOURCE_GAME="/path/to/Portal Stories Mel" run-game
```

The default source is the standard Steam installation under the user's Library.
`SOURCE_APP` is also accepted. Rebuilding the same output replaces the game tree
and retains its `portal_stories/SAVE` (or `save`) and local `config.cfg`,
`video.txt`, and `videodefaults.txt`. `make GAME=mel promote-loader` updates only
the loader in an existing output, including when the source disk is unavailable.

## Packaging validation — 2026-11-04

The native `all` target and universal Mel, Portal 2 and L4D2 converter builds
passed. Targeted `test-mel-source`, `test-l4d2-source` and
`test-portal2-converter` checks passed, covering source selection, output
isolation, save/config retention, stale-library cleanup and numbered publishing.

The shipped Mel converter converted the real Steam Mac installation twice on
Intel macOS, with developer tools disabled in its environment and an existing
runtime cache checked against the expected SHA-256 hashes. Both output signatures
verified, each app had Mel's identity and original launcher bytes, and source
metadata remained unchanged. The second conversion preserved the first output.
A SIGTERM during a separate copy test cancelled conversion and removed staging.

Rebuilding a converted real game through `make GAME=mel bundle` retained a
private test save. `promote-loader` also retained it with an unavailable source.
The converted bundle's `LP32_DYLD_SELFTEST=1` passed outside the command sandbox
with Mel selected automatically and seven game/runtime modules loaded. The
initial explicit-image test invocation needed a profile hint because Mel and
Portal 2 share a launcher; normal bundle invocation supplies the correct hint.
The sandboxed dependency check aborted; the same normal-bundle check passed
outside that sandbox. No loader code was changed for these checks.

Local output: `native/build/PortalStoriesMel-Converter.app`. A separate converted
real-game app remains under `native/build/mel-real-verification/converted/`.
Local logs and test scripts are under ignored `native/build/`; no game files,
SDK inputs or runtime libraries are committed. Existing installed apps were
preserved. A subsequent native GUI pass verified source-picker cancellation, source/output
selection, Cancel during copying, retry after cancellation, progress and disabled
controls, successful real-game conversion, and Show in Finder selecting the
finished app. The GUI used an existing verified runtime cache. Drag-and-drop,
Open Game, a new gameplay session, fresh runtime archive extraction and ARM
execution were not tested in this packaging pass.
Further campaign, controller and platform coverage are outside this task.
