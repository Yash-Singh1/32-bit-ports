# COD4 import audit

This audit preceded converter dev.6. It found missing handlers beyond the
`RunStandardAlert` call reported on macOS 27. The release adds native-backed
handlers for the network/run-loop and launcher/display gaps described below.
It does not implement every import in COD4.

## Inputs and method

| Executable | SHA-256 | Unique callable stub imports |
| --- | --- | ---: |
| Supplied issue #13 image | `5e943a76063d7e1e26bad289a879434dcebd5df2ac1607d58dc15113c42c71d4` | 727 |
| Steam campaign 1.7.2 | `43629c7f2f1b6f891e93c134f3101dbfb9fc4b46cabb1491e90437a450208870` | 687 |
| Steam multiplayer 1.7.2 | `f90ec11a0822628aac0c2f788d980967ad333405b2fec5f3ecfbca1e3d8d1374` | 737 |

`tools/audit_cod4_imports.py` reads the Mach-O indirect symbol table. It checks
for matching exports in the original guest C++ and Bink libraries and searches
the loader's compiled strings and bridge source for handler names. Optional
`llvm-objdump -d` output identifies direct callers. It never executes an import.

A matching string is only evidence to inspect, not proof of working dispatch,
correct ownership, complete argument support, or ABI compatibility. For example,
the Carbon dispatcher can reject a recognized name when its native export is
absent. This audit also checked those export dependencies on local macOS 26.5.2.
`FSpMakeFSRef` and `SetWindowContentColor` have replacement handlers;
`GetWindowResizeLimits` still depends on an absent native export. macOS 27 has
not been tested locally.

After the dev.6 additions, the tool flags 46 supplied-image imports, 27 Steam
campaign imports, and 37 Steam multiplayer imports with no handler-name or guest
export evidence. These are review candidates, not an exhaustive unsupported-call
count. Imported data, dynamic symbol lookups, Objective-C selectors, indirect
calls, and imports made by dependencies need separate analysis.

Run it against privately held files, for example:

```sh
python3 native/tools/audit_cod4_imports.py /path/to/COD4.image \
  --loader native/build/game_loader \
  --guest-library /path/to/compat-runtime/libstdc++.6.dylib \
  --guest-library /path/to/libBinkMachOx86.dylib \
  --disassembly /path/to/COD4-disassembly.txt \
  --output /tmp/cod4-import-audit.json
```

The supplied executable and private runtime libraries are not included here.

## Gaps addressed in dev.6

- **Modal alerts.** `RunStandardAlert` now enters the native modal loop, returns
  the actual button selection through a 16-bit guest output, and retires the
  disposed dialog token. COD4's settings prompt supplies no modal filter.
  Non-null guest modal filters remain unsupported.
- **HTTP worker loop.** `CFRunLoopSourceCreate` now converts a version-zero
  32-bit context and bridges retain/release, description, equal/hash,
  schedule/cancel, and perform callbacks. The supplied image calls it from
  `+[ASIHTTPRequest runRequests]`. Version-one Mach-port sources remain
  unsupported.
- **HTTP responses, authentication and uploads.** Added header-completion,
  response-version/status-line, stream availability/error, streamed-body request,
  authentication-method/realm/requirements, validity, and credential-dictionary
  calls. The authentication error output uses the guest's eight-byte
  `CFStreamError`, not the native structure. Added proxy auto-configuration
  script evaluation. These calls occur in ASIHTTPRequest's ordinary response,
  credential, upload and error paths.
- **Non-GC ownership.** `CFMakeCollectable` returns its input, as required in
  this retain/release runtime. It does not release the object or fabricate a
  new ownership count.
- **Launcher and display calls.** Added `GetControlByID`, signed 16-bit
  `GetControlValue`/`SetControlValue` and maximum values, control reference data,
  `ShowControl`/`HideControl`, `GetMenuEventTarget`, `EnableMenuItem`,
  `HIViewGetViewForMouseEvent`, `SetThemeCursor`, and `CGDisplayPixelsWide`.
  The supplied display code calls the width API when querying and changing
  screen modes. Added `NSTemporaryDirectory` and
  `CFStringConvertEncodingToIANACharSetName` for ASIHTTPRequest's temporary
  upload files and form encoding.

The native Carbon geometry test covers control IDs, packed pointer outputs,
signed-short values, visibility and reference data in legacy and composited
windows. It also exercises both display dimensions, encoding conversion, and
temporary-directory lookup. The modal test selects each of three actual native
buttons. The CFNetwork test covers source callback ownership, a real loopback
Basic-auth challenge, invalid authentication and eight-byte error canaries,
local PAC evaluation, upload stream creation, stream availability and errors.
It sends no request to an Aspyr or other external server.

## Remaining gaps

These calls still lack working bridges. A caller's presence establishes that
the code exists, not that a normal startup necessarily reaches it.

| Area | Remaining calls or families | Observed caller or consequence |
| --- | --- | --- |
| Legacy UI drawing | `CGRectUnion`, `OpenRgn`, `CloseRgn`, `FrameRect`, `DrawThemeFocusRect`, `DrawThemeMenuBarBackground`, `NormalizeThemeDrawingState`, `PlotIconRef` | Supplied `UserPaneDrawProc`; custom launcher painting is incomplete. |
| Focus and resize | `AdvanceKeyboardFocus`, `GetWindowResizeLimits` | Standard-event handling and window resize limits. The latter has a source handler but no native export on the test OS. |
| Clipboard | `GetCurrentScrap`, `GetScrapFlavorSize`, `GetScrapFlavorData` | Supplied `Sys_GetClipboardData`; pasting text can still trap. |
| Old fullscreen path | `SetOrigin` | Supplied `PseudoAGLSetFullscreen`; this path is not established by the working composited-window path. |
| Movie capture | `AddMovieResource`, `EndMediaEdits`, `InsertMediaIntoTrack` | Supplied `MacDisplay::StopCapture`; legacy movie recording is incomplete even though Bink playback uses the original guest library. |
| Application integration | `AECreateDesc`, `AECreateAppleEvent`, `AESend`, `LSCopyItemInfoForRef`, `LSOpenApplication` | Process messaging, application lookup, and a quit/relaunch path. |
| Network cache and MIME types | `CC_MD5`, `UTTypeCreatePreferredIdentifierForTag`, `UTTypeCopyPreferredTagWithClass` | ASIDownloadCache URL keys and ASIHTTPRequest file MIME lookup. |
| libc | `ctime`, `vfprintf`; multiplayer also imports `putenv$UNIX2003` | Supplied preprocessor date expansion and diagnostic printing; multiplayer environment updates. UNIX2003 normalization does not implement a missing base handler. |
| Multiplayer text and sound | `GetControlDataSize`, `HITextViewGetTXNObject`, `HandleControlKey`, `TXNClear`, `TXNSetData`, `TXNSetSelection`, `TXNSetTypeAttributes`, `TXNShowSelection`, `AlertSoundPlay` | Older multiplayer text-control and alert paths remain incomplete. |
| Publisher help | `ShowAspyrGameGuide` | Imported by the Steam images. |
| Supplied-image receipt/keychain code | `ASN1_get_object`, `OBJ_obj2nid`, `PKCS7_verify`, `PKCS7_free`, `d2i_PKCS7_fp`, `d2i_X509`, `X509_STORE_new/add_cert/free`, `X509_free`, `SHA1`, `SecCertificateGetData`, `SecKeychainOpen`, `SecKeychainSearchCreateFromAttributes/CopyNext`, `SecKeychainItemCopyContent/FreeContent`, `CFDataAppendBytes`, `CFEqual`, `CFStringCreateWithBytesNoCopy` | These imports are referenced in the supplied helper at `0x3396d0`. Its existing entry returns before that code. This audit did not change that entry and does not establish support for the complete receipt-validation path in another executable. |

The issue #13 startup regression still stops before `startAsynchronous`.
It proves the earlier object-lifetime and encryption fixes for that exact
executable. It does not prove full App Store networking, receipt validation,
menus, missions, or gameplay. A complete original app and a full run are needed
to establish those paths.
