# Steam ABI audit

The September 7 controller failure was `_lp32_steam_method_517`, called from
`CInputSystem::PollSteamControllers` at `inputsystem.dylib+0x6df0`.
`GetDigitalActionData` was explicitly marked unsupported. After that fix,
controller glyph PNG decoding reached the previously missing `_inflateInit_`
import in `client.dylib`'s `png_create_read_struct_2`.

## Implemented corrections

- Packed digital (2-byte), analog (13-byte), and motion (40-byte) returns for
  Steam Input 005/006 and Steam Controller 008. Typed native calls preserve
  Valve's packing; analog/motion calls translate the i386 hidden result
  pointer and pop it on return.
- Steam Input event callbacks, including independent 005/006 registrations,
  replacement, removal, native threads, and nested guest imports.
- `GetUGCDetails` borrowed filename outputs staged through native pointer
  cells. Only four bytes are written to the guest; strings are retained in
  guest memory. Failed calls preserve the previous guest pointer. The string
  cache grows past its previous 1,024-entry limit.
- `SteamParamStringArray_t` and its nested pointer array converted for
  Workshop tag/group APIs; negative or excessive counts fail before invoking
  native Steam.
- Packed IP values passed into legacy networking and returned by game-server
  APIs, and the packed party beacon location argument.
- Scoped HTML enums and pointer-free networking status callback records.
- Generator validation of SDK interface versions, preservation of pointer
  depth, explicit recognition of enums, and legacy ordinal lookup by
  interface instead of hard-coded table offsets. Non-factory object returns
  can no longer interpret an integer argument as an interface-version string.
- All zlib imports used by the supplied game, including guest allocation
  callbacks needed by its PNG decoder. Native zlib's wider stream record and
  opaque state are never exposed to guest code.
- Public Steam Timeline 001 methods, appended without changing existing
  generated ordinals.
- Server-list requests, scalar controls and their three-method C++ response
  objects. Typed guest handles retain full native pointer identity, filters
  use widened pointer cells with retained inline records, and callback
  lifetimes distinguish cancellation from release. Release invalidates the
  guest identity immediately and defers native destruction until in-flight
  calls finish.

## Verification

`make test-steam-abi` requires no Steam account, game data, or SDK download.
It compiles an independent mock native Steam client and actual i386 callers
at `-O0` and `-O2`. It exercises all three controller interfaces through
thousands of calls, native-thread callbacks, nested struct returns, guarded
filename outputs above 4 GB, reused native strings, failed/null outputs,
more than 1,024 distinct names, Workshop tag arrays, IP and beacon records.
Server-list fixtures cover all six creation methods, null requests, full-width
native handles, filters, native-thread/nested callbacks, cancellation,
deferred release and guest buffers crossing the 4 GB boundary. These are ABI
checks against a mock client; they do not establish online browsing or play.
The zlib fixture checks guarded 56-byte streams, incremental inflation,
deflation, checksums, resets, invalid data/version/size, allocator failures,
callback re-entry, and balanced guest allocation/free counts.

The controller test reproduced method 517's trap against the old Compat 2
loader before the fix. `make test-guest-dyld` additionally checks the existing
Steam facade/private ordinals, callbacks, 64-bit IDs, and scalar arguments in
all four legacy loader configurations.

The real Portal 2 run with a Bluetooth DualShock 4 continued for several
minutes without a trapped import. Steam Input/Controller digital and analog
polling and PNG glyph requests were observed in the trace; the captured
video-options menu displayed PlayStation circle and square glyphs. The user
confirmed that buttons and sticks work. The diagnostic process was then
terminated deliberately and Compat 2 was reopened normally. Both existing
game bundles and the local converter were updated and their signatures
verified. This validates the reported controller path, not a full playthrough.

## Remaining unsupported contracts

The unreleased issue #10 changes add opt-in method/callback tracing and Source
send-table/class overrides; see [the co-op notes](PORTAL2.md#experimental-cross-build-co-op-issue-10).
Legacy `SendUserConnectAndAuthenticate_DEPRECATED` and disconnect calls still
use the native legacy API. No denials are suppressed or approvals synthesized.
The reported relayed-client authentication failure is not fixed by ABI tracing
or by the table overrides. `test-portal2-coop` checks callback payloads and
delivery against a mock client, including all denial reasons 0–14.

The audit covers the generated 987 method entries; it is not a claim that
every Steamworks feature is implemented. Some entries still have
explicitly unsupported types or handles. They fail with a named diagnostic
instead of passing incompatible pointers to native Steam:

| Area | Remaining contracts |
| --- | --- |
| Server browser | Server-item borrowed records and direct ping/player/rules response objects |
| Modern networking messages/sockets (16 methods) | Message ownership/release callbacks, configuration unions, relay tickets, custom signaling, and fake UDP objects |
| Networking utilities (4 methods) | Message allocation, debug callbacks, and typed configuration values |
| HTML (1 method) | File-dialog response pointer array; pointer-bearing HTML callback payloads also require conversion |
| Private/deprecated interfaces | Legacy PS3 and post-result callbacks; only separately verified private method signatures are enabled |

Generic callback delivery remains limited to records proven pointer-free by
the SDK metadata. Full online play, Workshop workflows, and a full controller
playthrough need separate end-to-end validation. The targeted controller and
PNG fixes do not establish support for these remaining contracts.
