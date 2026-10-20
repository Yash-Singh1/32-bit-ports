#include "sdl_bridge.h"
#include "compat_runtime.h"

#import <AppKit/AppKit.h>
#import <GameController/GameController.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SDL 1.3 (SDL_joystick.h, SDL_keycode.h, SDL_init.h) constants. */
enum {
    kSDLInitVideo = 0x00000020,
    kSDLInitJoystick = 0x00000200,
    kSDLHatUp = 0x01, kSDLHatRight = 0x02, kSDLHatDown = 0x04, kSDLHatLeft = 0x08,
    kSDLScancodeMask = 1 << 30,
    kMaxJoysticks = 8,
    kAxisCount = 6,     /* left x/y, right x/y, left/right trigger */
    kButtonCount = 11,  /* A B X Y LB RB Back Start L3 R3 Home */
    kHatCount = 1,
};

/* Joystick handles are opaque to the title; each is a distinct guest token. */
static const uint32_t kJoystickTokenBase = 0x5d100010;

static pthread_mutex_t sdl_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t initialized_subsystems;
static bool opened[kMaxJoysticks];
/* Snapshot taken by SDL_JoystickUpdate (and SDL_JoystickOpen), as SDL reads
   device state only when the title updates. */
static int16_t axes[kMaxJoysticks][kAxisCount];
static uint8_t buttons[kMaxJoysticks][kButtonCount];
static uint8_t hats[kMaxJoysticks];

static bool trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_TRACE_SDL") != NULL;
    return enabled;
}

/* One line per distinct call, plus every call under LP32_TRACE_SDL. */
static void note_call(const char *name, const uint32_t *arguments)
{
    static const char *seen[64];
    static unsigned seen_count;
    bool first = true;
    pthread_mutex_lock(&sdl_lock);
    for (unsigned i = 0; i < seen_count; ++i) if (seen[i] == name || !strcmp(seen[i], name)) first = false;
    if (first && seen_count < 64) seen[seen_count++] = name;
    pthread_mutex_unlock(&sdl_lock);
    if (first || trace_enabled())
        fprintf(stderr, "compat32: sdl %s(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)\n", name, arguments[0], arguments[1], arguments[2],
                arguments[3], arguments[4]);
}

static NSArray<GCController *> *pads(void)
{
    NSMutableArray<GCController *> *result = [NSMutableArray array];
    for (GCController *controller in [GCController controllers]) {
        if (controller.extendedGamepad && result.count < kMaxJoysticks) [result addObject:controller];
    }
    return result;
}

static int16_t stick(float value, bool invert)
{
    if (invert) value = -value;
    if (value > 1.f) value = 1.f;
    if (value < -1.f) value = -1.f;
    return (int16_t)(value < 0 ? value * 32768.f : value * 32767.f);
}

/* Triggers rest at -32768 like an Xbox 360 pad under SDL's HID backend. */
static int16_t trigger(float value)
{
    if (value < 0.f) value = 0.f;
    if (value > 1.f) value = 1.f;
    return (int16_t)(value * 65535.f - 32768.f);
}

static void snapshot(void)
{
    @autoreleasepool {
        NSArray<GCController *> *list = pads();
        pthread_mutex_lock(&sdl_lock);
        for (unsigned index = 0; index < kMaxJoysticks; ++index) {
            memset(axes[index], 0, sizeof(axes[index]));
            memset(buttons[index], 0, sizeof(buttons[index]));
            hats[index] = 0;
            if (!opened[index] || index >= list.count) continue;
            GCExtendedGamepad *pad = list[index].extendedGamepad;
            axes[index][0] = stick(pad.leftThumbstick.xAxis.value, false);
            axes[index][1] = stick(pad.leftThumbstick.yAxis.value, true);
            axes[index][2] = stick(pad.rightThumbstick.xAxis.value, false);
            axes[index][3] = stick(pad.rightThumbstick.yAxis.value, true);
            axes[index][4] = trigger(pad.leftTrigger.value);
            axes[index][5] = trigger(pad.rightTrigger.value);
            GCControllerButtonInput *list_buttons[kButtonCount] = {
                pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY,
                pad.leftShoulder, pad.rightShoulder, nil, pad.buttonMenu, nil, nil, nil};
            if (@available(macOS 10.15, *)) list_buttons[6] = pad.buttonOptions;
            if (@available(macOS 10.14.1, *)) {
                list_buttons[8] = pad.leftThumbstickButton;
                list_buttons[9] = pad.rightThumbstickButton;
            }
            if (@available(macOS 11.0, *)) list_buttons[10] = pad.buttonHome;
            for (unsigned button = 0; button < kButtonCount; ++button)
                buttons[index][button] = list_buttons[button] && list_buttons[button].pressed;
            uint8_t hat = 0;
            if (pad.dpad.up.pressed) hat |= kSDLHatUp;
            if (pad.dpad.right.pressed) hat |= kSDLHatRight;
            if (pad.dpad.down.pressed) hat |= kSDLHatDown;
            if (pad.dpad.left.pressed) hat |= kSDLHatLeft;
            hats[index] = hat;
        }
        pthread_mutex_unlock(&sdl_lock);
    }
}

static int joystick_index(uint32_t token)
{
    if (token < kJoystickTokenBase || token >= kJoystickTokenBase + kMaxJoysticks * 16 ||
        (token - kJoystickTokenBase) % 16) return -1;
    int index = (int)((token - kJoystickTokenBase) / 16);
    return opened[index] ? index : -1;
}

/* SDL scancodes are USB HID keyboard usages; names follow SDL_keyboard.c. */
static const char *scancode_name(uint32_t scancode)
{
    static const char *const named[] = {
        [40] = "Return", [41] = "Escape", [42] = "Backspace", [43] = "Tab", [44] = "Space",
        [45] = "-", [46] = "=", [47] = "[", [48] = "]", [49] = "\\", [50] = "#", [51] = ";",
        [52] = "'", [53] = "`", [54] = ",", [55] = ".", [56] = "/", [57] = "CapsLock",
        [70] = "PrintScreen", [71] = "ScrollLock", [72] = "Pause", [73] = "Insert",
        [74] = "Home", [75] = "PageUp", [76] = "Delete", [77] = "End", [78] = "PageDown",
        [79] = "Right", [80] = "Left", [81] = "Down", [82] = "Up", [83] = "Numlock",
        [84] = "Keypad /", [85] = "Keypad *", [86] = "Keypad -", [87] = "Keypad +",
        [88] = "Keypad Enter", [99] = "Keypad .", [103] = "Keypad =",
        [224] = "Left Ctrl", [225] = "Left Shift", [226] = "Left Alt", [227] = "Left GUI",
        [228] = "Right Ctrl", [229] = "Right Shift", [230] = "Right Alt", [231] = "Right GUI",
    };
    static char generated[256][16];
    if (scancode >= 256) return "";
    if (scancode < sizeof(named) / sizeof(named[0]) && named[scancode]) return named[scancode];
    char *out = generated[scancode];
    if (scancode >= 4 && scancode <= 29) snprintf(out, 16, "%c", 'A' + (int)(scancode - 4));
    else if (scancode >= 30 && scancode <= 38) snprintf(out, 16, "%c", '1' + (int)(scancode - 30));
    else if (scancode == 39) snprintf(out, 16, "0");
    else if (scancode >= 58 && scancode <= 69) snprintf(out, 16, "F%u", scancode - 57);
    else if (scancode >= 89 && scancode <= 97) snprintf(out, 16, "Keypad %u", scancode - 88);
    else if (scancode == 98) snprintf(out, 16, "Keypad 0");
    else out[0] = 0;
    return out;
}

static const char *keycode_name(uint32_t keycode)
{
    if (keycode & kSDLScancodeMask) return scancode_name(keycode & ~(uint32_t)kSDLScancodeMask);
    switch (keycode) {
    case '\r': return "Return";
    case 27: return "Escape";
    case '\b': return "Backspace";
    case '\t': return "Tab";
    case ' ': return "Space";
    case 127: return "Delete";
    default: break;
    }
    static char generated[128][2];
    if (keycode >= 128 || keycode < 32) return "";
    generated[keycode][0] = (char)(keycode >= 'a' && keycode <= 'z' ? keycode - 32 : keycode);
    return generated[keycode];
}

/* SDL returns static strings; keep one guest copy per name. */
static uint32_t guest_string(const char *text)
{
    static struct { const char *host; uint32_t guest; } cache[512];
    static unsigned count;
    pthread_mutex_lock(&sdl_lock);
    uint32_t result = 0;
    for (unsigned i = 0; i < count && !result; ++i)
        if (!strcmp(cache[i].host, text)) result = cache[i].guest;
    if (!result) {
        result = compat_runtime32_copy_cstring(text);
        if (result && count < 512) cache[count++] = (typeof(cache[0])){strdup(text), result};
    }
    pthread_mutex_unlock(&sdl_lock);
    return result;
}

/* SDL 1.3's event queue (SDL_events.h): 56-byte SDL_Event records. */
enum {
    kSDLEventSize = 56,
    kSDLKeyDown = 0x300, kSDLKeyUp = 0x301, kSDLTextInput = 0x303,
    kSDLAddEvent = 0, kSDLPeekEvent = 1, kSDLGetEvent = 2,
    kEventCapacity = 256,
};
struct sdl_event { uint8_t bytes[kSDLEventSize]; };
static struct sdl_event event_queue[kEventCapacity];
static unsigned event_head, event_count;
static bool unicode_enabled;

static uint32_t event_type(const struct sdl_event *event)
{
    uint32_t type;
    memcpy(&type, event->bytes, 4);
    return type;
}

static void queue_event(const struct sdl_event *event)
{
    pthread_mutex_lock(&sdl_lock);
    if (event_count == kEventCapacity) {          /* SDL drops when full */
        pthread_mutex_unlock(&sdl_lock);
        return;
    }
    event_queue[(event_head + event_count++) % kEventCapacity] = *event;
    pthread_mutex_unlock(&sdl_lock);
}

/* SDL_PeepEvents over the queue, in SDL's order and filtering. */
static int peep_events(uint8_t *events, int count, uint32_t action, uint32_t min_type, uint32_t max_type)
{
    if (action == kSDLAddEvent) {
        for (int i = 0; i < count; ++i) queue_event((const struct sdl_event *)(events + i * kSDLEventSize));
        return count > 0 ? count : 0;
    }
    int used = 0;
    pthread_mutex_lock(&sdl_lock);
    for (unsigned i = 0; i < event_count && used < count;) {
        struct sdl_event *event = &event_queue[(event_head + i) % kEventCapacity];
        uint32_t type = event_type(event);
        if (type < min_type || type > max_type) { ++i; continue; }
        if (events) memcpy(events + used * kSDLEventSize, event->bytes, kSDLEventSize);
        ++used;
        if (action != kSDLGetEvent) { ++i; continue; }
        /* Remove this record, keeping the rest in order. */
        for (unsigned j = i; j + 1 < event_count; ++j)
            event_queue[(event_head + j) % kEventCapacity] = event_queue[(event_head + j + 1) % kEventCapacity];
        --event_count;
    }
    pthread_mutex_unlock(&sdl_lock);
    return used;
}

/* Mac virtual key codes to SDL scancodes (SDL 1.3 darwin_scancode_table). */
static const uint8_t darwin_scancodes[128] = {
    4, 22, 7, 9, 11, 10, 29, 27, 6, 25, 100, 5, 20, 26, 8, 21,
    28, 23, 30, 31, 32, 33, 35, 34, 46, 38, 36, 45, 37, 39, 48, 18,
    24, 47, 12, 19, 40, 15, 13, 52, 14, 51, 49, 54, 56, 17, 16, 55,
    43, 44, 53, 42, 0, 41, 231, 227, 225, 57, 226, 224, 229, 230, 228, 0,
    108, 99, 0, 85, 0, 87, 0, 83, 128, 129, 127, 84, 88, 0, 86, 109,
    110, 103, 98, 89, 90, 91, 92, 93, 94, 95, 111, 96, 97, 0, 0, 0,
    62, 63, 64, 60, 65, 66, 0, 68, 0, 104, 107, 105, 0, 67, 0, 69,
    0, 106, 117, 74, 75, 76, 61, 77, 59, 78, 58, 80, 79, 81, 82, 0,
};

/* SDL keycodes: printable keys and the ASCII control keys are their
   character on a US layout; everything else is the scancode with bit 30.
   (The title reads the active layout itself through TIS for AZERTY.) */
static uint32_t keycode_for_scancode(uint8_t scancode)
{
    if (scancode >= 4 && scancode <= 29) return 'a' + (scancode - 4);
    if (scancode >= 30 && scancode <= 38) return '1' + (scancode - 30);
    static const struct { uint8_t scancode; uint8_t sym; } ascii[] = {
        {39, '0'}, {40, '\r'}, {41, 27}, {42, '\b'}, {43, '\t'}, {44, ' '}, {45, '-'},
        {46, '='}, {47, '['}, {48, ']'}, {49, '\\'}, {51, ';'}, {52, '\''}, {53, '`'},
        {54, ','}, {55, '.'}, {56, '/'}, {76, 127},
    };
    for (unsigned i = 0; i < sizeof(ascii) / sizeof(ascii[0]); ++i)
        if (ascii[i].scancode == scancode) return ascii[i].sym;
    return scancode | kSDLScancodeMask;
}

static uint16_t modifier_bit(uint8_t scancode)
{
    switch (scancode) {
    case 225: return 0x0001; case 229: return 0x0002;   /* KMOD_LSHIFT, RSHIFT */
    case 224: return 0x0040; case 228: return 0x0080;   /* KMOD_LCTRL, RCTRL */
    case 226: return 0x0100; case 230: return 0x0200;   /* KMOD_LALT, RALT */
    case 227: return 0x0400; case 231: return 0x0800;   /* KMOD_LGUI, RGUI */
    case 57: return 0x2000;                             /* KMOD_CAPS */
    default: return 0;
    }
}

static uint16_t key_modifiers;
static uint8_t keys_down[256];

/* SDL_SendKeyboardKey: SDL_KeyboardEvent {type, windowID, state, repeat,
   pad[2], keysym {scancode, sym, mod, unicode}}. */
static void queue_key(uint8_t scancode, bool down, bool repeat, uint32_t unicode)
{
    if (!scancode) return;
    uint16_t bit = modifier_bit(scancode);
    if (bit && bit != 0x2000) key_modifiers = down ? key_modifiers | bit : key_modifiers & ~bit;
    if (!down && !keys_down[scancode]) return;          /* SDL drops unmatched releases */
    if (down && keys_down[scancode]) repeat = true;
    keys_down[scancode] = down;
    struct sdl_event event = {{0}};
    uint32_t type = down ? kSDLKeyDown : kSDLKeyUp, sym = keycode_for_scancode(scancode), code = scancode;
    memcpy(event.bytes, &type, 4);
    event.bytes[8] = down;                              /* SDL_PRESSED */
    event.bytes[9] = repeat;
    memcpy(event.bytes + 12, &code, 4);
    memcpy(event.bytes + 16, &sym, 4);
    memcpy(event.bytes + 20, &key_modifiers, 2);
    if (down && unicode_enabled) memcpy(event.bytes + 24, &unicode, 4);
    queue_event(&event);
}

/* SDL_SendKeyboardText: UTF-8 text in a 32-byte field after type/windowID. */
static void queue_text(NSString *characters)
{
    const char *utf8 = characters.UTF8String;
    if (!utf8 || !utf8[0] || (uint8_t)utf8[0] < 32 || utf8[0] == 127) return;
    if (!strncmp(utf8, "\xef\x9c", 2) || !strncmp(utf8, "\xef\x9d", 2) ||
        !strncmp(utf8, "\xef\x9e", 2) || !strncmp(utf8, "\xef\x9f", 2)) return; /* function keys */
    struct sdl_event event = {{0}};
    uint32_t type = kSDLTextInput;
    memcpy(event.bytes, &type, 4);
    strlcpy((char *)event.bytes + 8, utf8, 32);
    queue_event(&event);
}

/* Cocoa_PumpEvents/Cocoa_HandleKeyEvent: key events become SDL keyboard
   events.  Returns true when AppKit must not see the event: only Command
   shortcuts and modifier changes still go to NSApp (menus, flag state). */
static bool translate_key_event(NSEvent *event)
{
    NSEventType type = event.type;
    if (type != NSEventTypeKeyDown && type != NSEventTypeKeyUp && type != NSEventTypeFlagsChanged) return false;
    uint8_t scancode = event.keyCode < 128 ? darwin_scancodes[event.keyCode] : 0;
    if (type == NSEventTypeFlagsChanged) {
        uint16_t bit = modifier_bit(scancode);
        if (bit == 0x2000) {
            bool locked = (event.modifierFlags & NSEventModifierFlagCapsLock) != 0;
            key_modifiers = locked ? key_modifiers | bit : key_modifiers & ~bit;
            queue_key(scancode, true, false, 0);
            queue_key(scancode, false, false, 0);
        } else if (bit) {
            queue_key(scancode, !keys_down[scancode], false, 0);
        }
        return false;
    }
    bool down = type == NSEventTypeKeyDown;
    uint32_t unicode = 0;
    if (down && event.characters.length) {
        unicode = [event.characters characterAtIndex:0];
        if (unicode >= 0xf700 && unicode <= 0xf8ff) unicode = 0;   /* function-key private use */
    }
    queue_key(scancode, down, event.isARepeat, unicode);
    if (down && unicode_enabled && !(event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)))
        queue_text(event.characters);
    return !(event.modifierFlags & NSEventModifierFlagCommand);
}

int sdl_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    if (strncmp(name, "_SDL_", 5)) return 0;
    const char *call = name + 5;
    note_call(name, a);
    *result = 0;
    if (!strcmp(call, "Init") || !strcmp(call, "InitSubSystem")) {
        pthread_mutex_lock(&sdl_lock);
        initialized_subsystems |= a[0];
        pthread_mutex_unlock(&sdl_lock);
        return 1;
    }
    if (!strcmp(call, "QuitSubSystem")) {
        pthread_mutex_lock(&sdl_lock);
        initialized_subsystems &= ~a[0];
        if (a[0] & kSDLInitJoystick) memset(opened, 0, sizeof(opened));
        pthread_mutex_unlock(&sdl_lock);
        return 1;
    }
    if (!strcmp(call, "NumJoysticks")) {
        if (!(initialized_subsystems & kSDLInitJoystick)) { *result = (uint32_t)-1; return 1; }
        @autoreleasepool { *result = (uint32_t)pads().count; }
        return 1;
    }
    if (!strcmp(call, "JoystickOpen")) {
        int32_t index = (int32_t)a[0];
        NSUInteger count;
        @autoreleasepool { count = pads().count; }
        if (index < 0 || index >= kMaxJoysticks || (NSUInteger)index >= count) return 1;
        pthread_mutex_lock(&sdl_lock);
        opened[index] = true;
        pthread_mutex_unlock(&sdl_lock);
        snapshot();
        *result = kJoystickTokenBase + (uint32_t)index * 16;
        return 1;
    }
    if (!strcmp(call, "JoystickOpened")) {
        int32_t index = (int32_t)a[0];
        *result = index >= 0 && index < kMaxJoysticks && opened[index];
        return 1;
    }
    if (!strcmp(call, "JoystickClose")) {
        int index = joystick_index(a[0]);
        if (index >= 0) {
            pthread_mutex_lock(&sdl_lock);
            opened[index] = false;
            pthread_mutex_unlock(&sdl_lock);
        }
        return 1;
    }
    if (!strcmp(call, "JoystickUpdate")) { snapshot(); return 1; }
    if (!strcmp(call, "JoystickNumAxes") || !strcmp(call, "JoystickNumButtons") ||
        !strcmp(call, "JoystickNumHats")) {
        int index = joystick_index(a[0]);
        *result = index < 0 ? (uint32_t)-1 :
            call[8] == 'A' ? kAxisCount : call[8] == 'B' ? kButtonCount : kHatCount;
        return 1;
    }
    if (!strcmp(call, "JoystickGetAxis")) {
        int index = joystick_index(a[0]);
        *result = index >= 0 && a[1] < kAxisCount ? (uint32_t)(int32_t)axes[index][a[1]] : 0;
        return 1;
    }
    if (!strcmp(call, "JoystickGetButton")) {
        int index = joystick_index(a[0]);
        *result = index >= 0 && a[1] < kButtonCount ? buttons[index][a[1]] : 0;
        return 1;
    }
    if (!strcmp(call, "JoystickGetHat")) {
        int index = joystick_index(a[0]);
        *result = index >= 0 && a[1] < kHatCount ? hats[index] : 0;
        return 1;
    }
    /* As SDL's Cocoa backend: on the main thread, drain AppKit's queue and
       dispatch each event through NSApp (the title's own -sendEvent: and
       views take input from there).  This is also what services the run
       loop's timers and delayed performs while the game loop owns the main
       thread.  As there, key events become SDL keyboard events: the title
       reads its keys with SDL_PeepEvents (SDL_KEYDOWN..SDL_TEXTINPUT). */
    if (!strcmp(call, "PumpEvents")) {
        if (pthread_main_np()) {
            @autoreleasepool {
                for (;;) {
                    NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                                           inMode:NSDefaultRunLoopMode dequeue:YES];
                    if (!event) break;
                    if (!translate_key_event(event)) [NSApp sendEvent:event];
                }
            }
        }
        return 1;
    }
    if (!strcmp(call, "PeepEvents")) {
        *result = (uint32_t)peep_events((uint8_t *)(uintptr_t)a[0], (int32_t)a[1], a[2], a[3], a[4]);
        return 1;
    }
    if (!strcmp(call, "EnableUNICODE")) {
        int previous = unicode_enabled;
        if ((int32_t)a[0] >= 0) unicode_enabled = (int32_t)a[0] != 0;
        *result = (uint32_t)previous;
        return 1;
    }
    if (!strcmp(call, "GetKeyName")) { *result = guest_string(keycode_name(a[0])); return 1; }
    if (!strcmp(call, "GetScancodeName")) { *result = guest_string(scancode_name(a[0])); return 1; }
    /* The title creates its NSOpenGLContext itself; attributes requested here
       only matter to SDL_SetVideoMode, which is never reached on the Cocoa
       path (a NULL surface reports failure if it ever is). */
    if (!strcmp(call, "GL_SetAttribute")) return 1;
    if (!strcmp(call, "WM_SetCaption") || !strcmp(call, "GL_SwapBuffers")) return 1;
    if (!strcmp(call, "SetVideoMode")) {
        fprintf(stderr, "compat32: SDL_SetVideoMode(%d, %d, %d, 0x%x) is not bridged\n",
                (int32_t)a[0], (int32_t)a[1], (int32_t)a[2], a[3]);
        return 1;
    }
    return 0;
}
