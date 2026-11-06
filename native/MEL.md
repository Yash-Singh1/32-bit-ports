# Portal Stories: Mel

Portal Stories: Mel support was implemented by [@bretislavstepan](https://github.com/bretislavstepan).

Mel uses Steam AppID 317400, the `portal2_osx` launcher and the
`portal_stories` game directory. Its converted app, saves and logs are separate
from Portal 2.

## Build and convert

Building from source requires Xcode command-line tools. From `native/`:

```sh
make mel-converter
```

Open `build/PortalStoriesMel-Converter.app`, choose the original Mac game folder
or app, then choose an output folder. For Steam, select
`steamapps/common/Portal Stories Mel`.

The source must contain `portal2_osx`, `portal_stories/gameinfo.txt` and the Mac
`launcher.dylib`. Assemble separate depot downloads into one folder first.
Windows game files are not supported.

The converter creates `PortalStoriesMel-Compat.app`, adding a number if the name
already exists. It copies the source's saves and settings. Progress from other
converted apps is not merged; keep the app containing your progress.

The first conversion downloads Apple's 4.72 GB Lion archive to extract two
runtime libraries. Later conversions reuse the cache. The built converter needs no
developer tools to run. Open Steam and sign in before launching the converted game.

For command-line builds:

```sh
make GAME=mel SOURCE_GAME="/path/to/Portal Stories Mel" bundle
make GAME=mel SOURCE_GAME="/path/to/Portal Stories Mel" run-game
```

The default source is
`~/Library/Application Support/Steam/steamapps/common/Portal Stories Mel`.
`SOURCE_APP` is also accepted.

Rebuilding retains the output's `portal_stories/SAVE` (or `save`) and
`cfg/config.cfg`, `cfg/video.txt` and `cfg/videodefaults.txt`. Other game files
are replaced. `make GAME=mel promote-loader` updates only the loader in an
existing output and works without the source installation.

## Steam interfaces

Mel needs older Steam interfaces than Portal 2. SteamClient017 uses the Mac
vtable, without the PS3 accessor and with the ParentalSettings slot.
RemoteStorage014 and UserStats011 also omit PS3/server-only methods.
Unsupported argument layouts fail explicitly.

`native/src/steam_abi.inc` contains the generated table. Building and using the
converter does not require the SDK. To regenerate Mel's additions, use
`native/tools/generate_mel_steam_abi.py` after the regular generator.

Reference versions:

- [Steamworks SDK 1.40](https://github.com/rlabrecque/SteamworksSDK/tree/117b1282e83d)
- [Steamworks SDK 1.41](https://github.com/rlabrecque/SteamworksSDK/tree/ef207e7697c2)

The input directory needs SDK 1.40 `steam_api.json` and the legacy interface
headers. SDK 1.41 supplies `steam_api141.json`, `isteamclient017141.h` and
`isteamhtmlsurface004.h`. SDK inputs are not included in this repository.
Regeneration requires Python 3 and Xcode's Clang.

From the repository root, using the pre-Mel table as a baseline:

```sh
git show fb8e449ec9ae499bc56c29616af68fc8bfaa9628:native/src/steam_abi.inc > /tmp/mel-baseline.inc
python3 native/tools/generate_mel_steam_abi.py \
  /path/to/sdk-reference /tmp/mel-baseline.inc /tmp/mel-steam-abi.inc
cmp native/src/steam_abi.inc /tmp/mel-steam-abi.inc
```

## Callback stack fix

Nested callbacks could overwrite a suspended i386 stack frame when it exceeded
the fixed 64 KiB callback stride. The loader now places callbacks below the
suspended stack. A 96 KiB fixture with nested qsort/bsearch callbacks checks
sorting and preservation of the suspended frame:

```sh
make -C native test-callback-stack
```

Portal 2's UUID-specific deferred-physics patch does not apply to Mel's server
build.

## Verification

Gameplay and graphics settings have been checked on Intel macOS. Packaging
checks cover real-game conversion, repeated conversion, source preservation,
save retention during rebuilds, loader updates without the source installation,
code signatures and loading the game libraries. GUI checks cover folder pickers,
cancellation, retry, completion and Show in Finder.

The native loader and all three converters build. Relevant regression targets:

```sh
make -C native test-mel-source test-l4d2-source test-portal2-converter
make -C native test-guest-dyld test-source-steam-startup
```

On Apple Silicon, the optional legacy microphone input is rejected before
initialization to avoid a Core Audio allocator crash under Rosetta. Game audio
playback remains enabled. Intel microphone behavior is unchanged. Developers
can set `LP32_ENABLE_MEL_MICROPHONE=1` to retest the native capture path.

Full-campaign, controller and Apple Silicon gameplay coverage is not established.

Direct loader invocation accepts the converted image name (`Portal2.image` or
`PortalStoriesMel.image`) as an identity hint. For a raw or renamed launcher,
set `LP32_GAME=portal2` or `LP32_GAME=PortalStoriesMel` explicitly: the two
launchers share a fingerprint.
