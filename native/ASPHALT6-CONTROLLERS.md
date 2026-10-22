# Asphalt 6 controller research

Static analysis of the Windows upload at
[Internet Archive](https://archive.org/details/asphalt-6-win-32), compared with
this repository's macOS port. Analysis date: 2026-10-22. Neither game executable
was launched, loaded into a process, or emulated during this analysis.
The integration proposal below predates the controller adapter now implemented
in this repository. See [the controller guide](ASPHALT6.md#controllers) for
the implementation and its validation limits.

The Windows host appears to contain a compatibility layer written specifically
for this game. Its controller support translates XInput into the original Mac
keyboard and mouse events. The original game has enough float-based steering
code to investigate proportional stick input without replacing its physics.
The implemented integration still needs in-game controller validation.

Addresses below are virtual addresses, not file offsets. Mac addresses refer
to the original i386 image. Windows addresses include the PE's preferred base
of `0x30000000`. Function names used for unnamed routines are descriptive
labels assigned during analysis. Objective-C selector names and named C++
symbols come from the executable's metadata and symbol table.

**Build identity and evidence.** The Windows archive contains the same Mac
1.0.9 executable as the local `Asphalt6.image`. Both are 8,032,256 bytes and
match byte for byte. The analyzed SHA-256 values are:

```text
Mac executable
8ce20892c7bf360a4bc71e29b8e32976c40081558db50788e29da9a59a1a6407

Windows host, 653824 bytes
8b184e4bdc21ff9e51b72e58ca0ae0d618088ecd7f12699811989bf05ee87147

code_relocs.bin, 654656 bytes
81532d2a0d14c9b97863f1f539a5af59dc64e9e78538ce10c637a5b24972cc43
```

Selected files were fetched using HTTP byte ranges and decompressed as data.
The game data archive was not needed. Python parsed Mach-O/PE records and
Capstone disassembled instruction bytes. `otool -ov` supplied Objective-C
method metadata; `nm -n` supplied the remaining symbols. Local evidence is in
`build/asphalt6-controller-research/`: `evidence.json`,
`mac-input-disassembly.txt`, `windows-controller-disassembly.txt`, and
`windows-runtime-disassembly.txt`. These generated files are git-ignored.

The verification checked the hashes, all 32 action-map records, singleton
indirections and the proposed steering call site's relative target. A Python
model also checked the decoded edge-buffer operations described below. That
model did not execute guest instructions.

**Compatibility-layer provenance.** There is no port source code alongside
the host executable. Its eleven DLL dependencies are Windows platform
libraries: Kernel32, User32, GDI32, OpenGL32, GDI+, Ole32, Winsock,
IP Helper, WinINet and the two Media Foundation libraries. XInput is loaded
dynamically. This does not exclude statically linked third-party code.

The host implements a small Objective-C runtime directly:

- `0x3000ce80` is its message-send entry. It asks `0x3000dbf0` to resolve an
  implementation and jumps to it. A nil receiver returns zero.
- `0x3000d0d0` looks up classes using FNV-1a and a 512-slot open-addressed
  registry. `0x3000d510` handles selector registration.
- `0x3000cff0` builds class/metaclass records and small method arrays.
  `0x3000cfc0` allocates an instance and writes its class pointer.
- The loader reads the old Mac `__OBJC` records, registers the game's classes,
  and installs hand-written implementations for the base classes and the
  Foundation/AppKit methods the game uses.
- Diagnostics include the internal names `a6_objc_load`,
  `a6_foundation_install` and `a6_reg_foundation_defaults`, plus game-specific
  initialization and direct guest addresses.

These are positive signs of a game-specific implementation. No identifying
Darling, GNUstep, Cocotron or WinObjC strings were found. Their absence alone
cannot prove authorship or exclude adapted snippets.
[Darling](https://github.com/darlinghq/darling) describes a broader Linux
runtime with a userspace kernel server and framework implementations.
[GNUstep libobjc2](https://github.com/gnustep/libobjc2) is a general Objective-C
runtime supporting several compiler ABIs.
[WinObjC](https://github.com/microsoft/WinObjC) is a source-based Windows bridge
for iOS projects. The observed host is not packaged like any of these. The
defensible conclusion is "apparently bespoke, upstream provenance unproven."

The host also supplies libc/POSIX/C++ replacements. It uses Windows OpenGL,
feeds the game's AudioUnit callback into WASAPI, and maps sockets to Winsock.
`code_relocs.bin` starts with `A6RL` and a count of 163,662 locations. The
loader's diagnostics identify several relocation passes, including one called
`ghidra`. This indicates Ghidra-assisted relocation analysis; it does not
identify the source of the compatibility layer.

**What the Windows controller code does.** The poller starts at `0x3001f060`.
It runs from the host's main loop at `0x300102ce`. It resolves
`XInputGetState` from `xinput1_4.dll`, `xinput1_3.dll`, then
`xinput9_1_0.dll`. It scans four XInput indices, selects one controller and
retries discovery at roughly two-second intervals.

The mapping decoded from the instructions is:

```text
RT                  Up arrow, held while RT >= 31/255
LT                  Down arrow, held while LT >= 31/255
Left stick X        Left/Right arrow beyond -7849/+7849
D-pad Left/Right    Left/Right arrow during races
LB or RB            Tap Space on a new shoulder-button press
X                   Advance camera index modulo 3; tap '1', '2' or '3'
Y                   Move camera index backward modulo 3; tap the digit
Start, Back or B     Tap Escape on a new button press
A                   Mouse-button down/up at the virtual cursor
Right stick         Move the virtual cursor
Left stick/D-pad    Also move the cursor outside GS_Race
```

The XInput packet layout and masks agree with Microsoft's
[XINPUT_GAMEPAD definition](https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_gamepad).
The camera-back mapping cycles backward through camera modes. It does not
implement a hold-to-look-behind action.

Steering has three output states: left, neutral and right. Triggers have two.
The host does not send proportional steering, throttle or brake values into
the game. Cursor movement is proportional outside its dead zone, approximately
`trunc((abs(axis) - 7849) * 25 / 24918)` pixels per poll. It is tied to poll
frequency, without a time delta. D-pad cursor movement uses 25-pixel steps.

`0x3001f550` passes synthetic messages to the host's event converter at
`0x3000f220`. This creates its replacement NSEvent objects and sends the
game's `keyDown:`, `keyUp:` and mouse selectors. It does not require a virtual
HID driver. The inspected controller routines contain no rumble command.

`0x3001f740` finds the top state and compares its name with `GS_Race`.
`0x3001efa0` changes the game's cursor-hidden byte at guest `0x00741045`.
It considers recent real mouse activity and right-stick movement, with an
idle timeout of 2,500 ms while racing.

Two implementation details should not be copied literally. On disconnect,
the poller resets its selected-controller and initialization state without
emitting releases for the keys or mouse button it owns. The first sample
also seeds prior trigger states without emitting a down event, so a trigger
held during connection can require release and repress. These are static
control-flow findings; their visible symptoms were not tested.

**The original Mac keyboard path.** The metadata identifies these EAGLView
methods:

```text
0x000b0db0  keyDown:
0x000b0170  keyUp:
0x000b0100  ApplicationPostEvent:
```

Both key handlers reject repeat events and require a nonempty `characters`
string before they examine the key code. Arrow events therefore need a proper
function-key character, even though the handler uses the Mac virtual key code
to identify their direction. Sending an arrow event with empty characters
will not work. The relevant Mac codes are 123/124/125/126 for
left/right/down/up, 49 for Space, 53 for Escape, and 18/19/20 for `1`/`2`/`3`.

The handlers create a `SEvent` with type 2, engine key at `+0x0c`, and a
pressed byte at `+0x10`. `ApplicationPostEvent:` calls `0x00424600` with
immediate dispatch. The event receiver at `0x00125750` translates engine keys
using the 32-entry table at `0x006d2da0`.

Relevant entries are:

```text
Engine key   Action mask    Use
0x26         0x00000001     Up / acceleration
0x28         0x00000002     Down / brake
0x25         0x00000004     Left
0x27         0x00000008     Right
0x20         0x00000200     Space / nitro input
0x1b         0x00008000     Escape / pause or back
0x31         0x08000000     Camera 1
0x32         0x10000000     Camera 2
0x33         0x20000000     Camera 3
```

Engine codes are not macOS virtual key codes. The arrow codes resemble
Windows virtual keys because the original game translates Cocoa events into
its own cross-platform event representation.

The application's singleton pointer lives at `0x00741220`, reached through
the image pointer slot `0x0070570c`. Its action-state object begins at `+0xc0`.
`0x000b8ca0` queues press/release bits. `0x000b8bf0` commits them during the
application update, called at `0x000bbb88`. `0x000b8b90` queries a mask in
the pressed, held or released field.

The first bank's offsets relative to that action-state object are:

```text
+0x04  pressed this update
+0x08  held
+0x0c  released this update
+0x10  pending presses
+0x14  pending releases
```

The important decoded operations are:

```c
// Simplified from 0x000b8ca0, for a nonzero action mask.
if (down) {
    if (!(held & mask)) pending_presses |= mask;
} else {
    if (held & mask) pending_releases |= mask;
}

// Simplified from 0x000b8bf0.
held = (held | pending_presses) & ~pending_releases;
pressed = pending_presses;
released = pending_releases;
pending_presses = pending_releases = 0;
```

This explains a problem with the Windows tap helper at `0x3001f690`. It
delivers down and up synchronously in the same poll. If the action was not
held, the down queues a press but the up sees the old held value and can drop
the release. The next update leaves it held. A later tap can release it.
The host has no literal references to these guest queue/commit routines that
would indicate an explicit patch to this behavior. Runtime confirmation is
still needed, but our port should serialize quick edges across actual input
updates instead of relying on same-tick taps.

This matters particularly because `0x0017c7d0` reads both Space edges, and
the camera and Escape handlers query released bits. Preserve a down through
one input commit before delivering its up. For shoulders, mapping physical
press and release to Space is preferable to manufacturing a tap. Buttons
pressed and released between polls still need an edge queue.

**Steering inside the game.** The per-car control pointer is at car `+0xef0`.
The car update calls `0x001e83e0` through that pointer at `0x00198743`.
The control object stores its car pointer at `+4`, current driving bits at
`+8`, steering mode at `+0xa4`, and throttle mode at `+0xa8`.

The control update calls `0x001e6bc0` to build the driving bits. In the
ordinary button path, masks 1/2/4/8 mean accelerate/brake/left/right.
Throttle settings can alter this behavior. One branch derives acceleration
from the absence of braking, so adding triggers should preserve the game's
chosen auto-acceleration mode.

`0x001e8190` applies steering. It chooses among:

```text
steering mode 0 / default   0x001e5e20, digital buttons
steering mode 1             0x001e7820, tilt/float calculation
steering mode 2             0x001e7380, another float-driven path
```

The digital routine takes a control object and an integer elapsed-time value.
At `0x001e5e55` and `0x001e5e80`, it tests left/right bits. It starts with
a signed response step of approximately `3 * elapsed / 16`. Further code
applies sensitivity, response limits, direction changes and flag-dependent
handling associated with the game's special driving states. Keyboard input
therefore already produces smoothed steering rather than an immediate
maximum-angle assignment.

The internal car steering value at `+0x330` is centered on
`0x3fc90fdb`, or pi/2 radians. The digital neutral branch at `0x001e5f90`
computes approximately:

```text
angle_next = (32 * angle + 50.2654838562) / 64
           = pi/2 + 0.5 * (angle - pi/2)
```

It then snaps sufficiently small differences to center. This is a per-call
decay in the original routine, so its response depends on the frequency at
which that routine runs. It is not a proposed controller response curve.

`0x001e5b90` computes an effective steering limit in radians using the
car's speed and tuning fields around `+0x5bc` through `+0x5dc`. It has
additional flag and vehicle-type branches. The digital routine scales its
output around pi/2 using this limit. `0x000d3ca0` converts a configured
maximum angle in the physics object from degrees to radians.

The shared application routine subtracts pi/2, applies its remaining limits
and corrections, then calls `0x000d7250`. That setter stores the signed
physics steering value at physics `+0x864` and updates the associated
trigonometric values. The physics object is embedded at car `+0x54`.
Writing only the final car float would skip these side effects.

The `Accelerometer` singleton at `0x007411e8` is real, not just an unused
string. Its accessor is `0x000babd0`; its update method at `0x0012c100`
stores float samples at `+4`, `+8` and `+0xc`. `0x001e7590` reads these
samples and filters them with weights 0.4 for the new sample and 0.6 for
the previous value. The mode-1 steering routine consumes the filtered floats.
Reusing this path would require supplying a coherent tilt vector and dealing
with calibration and orientation. A direct steering hook is easier to
control and does not need to impersonate a motion sensor.

There is a concrete candidate for that hook: the call at `0x001e81e3`
targets `0x001e5e20`. Its five original bytes are `e8 38 dc ff ff`.
Replacing that call for the selected local car would preserve the common
steering application below it. A candidate output is:

```text
u = signed stick value after dead zone and remapping, in [-1, 1]
limit = original effective steering limit from 0x001e5b90
candidate steering = pi/2 + u * limit
```

This is an implementation proposal, not a verified patch. Match its sign
against keyboard steering; preserve the game's direction/drift state when
necessary; and check whether separate smoothing is desirable. Apply it only
to the chosen local control object and when the game would otherwise execute
the digital path. Other cars and other steering modes must retain their
original behavior. The apparently useful dispatcher at `0x001e8150` is
insufficient by itself: the observed main update calls the individual
steering routines from `0x001e8190`.

Guest float returns use x87 ST(0). A callback that returns the float's bits
only in EAX would break the caller, which immediately stores ST(0). Our
existing float-return bridge can support this, but the implementation must
use that convention explicitly and retain the guest call's stack alignment.
Check the executable hash and original call bytes before patching a mapped
image. Do not treat these i386 addresses as native 64-bit function pointers.

**Throttle, brake, nitro and menus.** The original pedal outputs are floats.
At physics `+0x754` and `+0x758`, the game stores acceleration and braking
values. `0x000d3b60` clamps acceleration to zero through the configured
maximum at `+0x75c`; `0x000d3bb0` clamps braking to 0 through 100.
`0x000d3c00` swaps those outputs in a special branch. These addresses make
proportional triggers plausible, but simply writing them once per rendered
frame would compete with the normal control update. A later analog-trigger
implementation needs an appropriate point in the physics update and must
preserve those special branches. Keep digital triggers for the first
steering implementation.

Space is handled through the existing HUD/game logic. `0x0017c7d0` sets
a flag at its receiver's `+0x39` on press and clears it on release. Camera
digits and Escape also use this routine's existing logic. Feeding their
normal input events retains the game's camera setup and pause handling.
The numeric camera keys select preset camera modes; they are not free-camera
axes.

The `StateStack` singleton pointer is at `0x007411e4`, through slot
`0x00705860`. `0x000c8a60` returns its top state. The race state's name
method at `0x000c86f0` returns `GS_Race`. Its `+0x2c` field is also
important: `0x000c3b70` sets it to 1 while opening pause. The race remains
the top state. Checking only the state name therefore does not distinguish
active driving from a paused race. `0x000c8700` checks this same field for
zero. Our controller mode should account for pause and active overlays.

The Windows right-stick cursor remains usable during pause, but its
left-stick/D-pad mode switch and idle cursor hiding use the state-name test.
For macOS, switch those inputs to menu navigation when pause opens and
release racing inputs immediately. Use time-based cursor speed and the
existing view/window coordinate conversion so fullscreen and secondary
displays work. Avoid moving the system pointer when an in-app event can
express the same action.

**Integration into this repository.** `src/controller_bridge.m` already
reads GameController.framework devices and records button edges. Its
installation currently returns early when the profile has no legacy HID
layout. The Asphalt profile has no such layout; adding fake LEGO HID records
would not connect to Asphalt's input path.

Add an Asphalt-specific adapter alongside the legacy HID route. Reuse native
device discovery and normalized samples, with a separate action mapper.
`src/objc_bridge.m` currently polls the LEGO controller bridge in
`OpenGLView -swapBuffers`. Asphalt presents through the Gameloft
`NSOpenGLContext -flushBuffer` handling instead. That path needs input
servicing, but one-shot releases should ultimately follow input commits,
not just presentation counts. Hooking the action-commit point or tracking
the application update is more precise.

The scripted-input code already builds valid NSEvent characters, flags and
mouse coordinates. Extract shared production event construction rather than
depending on `LP32_TEST_*` state or invoking test-only helpers. In-app Cocoa
dispatch avoids a global keyboard injector and its Accessibility permission.

Keep keyboard and controller state separately and deliver changes in their
combined state. Otherwise, releasing a gamepad trigger can release an action
that the keyboard still holds. Queue very short physical button presses,
release controller-owned input on disconnect or focus loss, and preserve
the focus policy in `src/focus_policy.h`. Do not enable background physical
input merely because rendering is allowed to continue in the background.

A useful implementation order is digital racing and menu control first,
then the guarded analog-steering hook, then proportional pedals if desired.
Checks should cover held-key disconnects, quick button taps, keyboard/pad
overlap, pause/resume, camera changes, menu clicks after resizing, steering
at multiple speeds, reverse steering, and the game's drift behavior. LAN
and replay paths need a separate check before claiming parity there.

The address map, event buffering, Windows mappings and float setters above
come from static instruction/data inspection. Windowless tests now exercise
the implemented mapper and shared Cocoa event construction. A synthetic i386
fixture verifies the actual nested guest/host float-return path and stack
alignment without executing either game. Exact source provenance, controller
feel, and complete special-driving-state semantics remain open.
