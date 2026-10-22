#include "asphalt6_controller.h"
#include "asphalt6_input.h"
#include "cocoa_input.h"
#include "compat_runtime.h"
#include "focus_policy.h"
#include "game_profile.h"

#import <GameController/GameController.h>
#import <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <math.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const struct lp32_asphalt6_controller_layout *layout;
static struct a6_input input;
static bool installed, analog_enabled, trace;
static _Thread_local bool injecting;
static GCController *controller;
static NSView *input_view;
static bool mouse_down_delivered;
static NSPoint cursor;
static double cursor_time;
static atomic_bool accept_samples;
static atomic_uint generation;

struct sample { uint32_t buttons; float x, y, rx, ry; };
static struct sample samples[128], current_sample;
static unsigned sample_head, sample_count;
static bool sample_overflow;
static os_unfair_lock sample_lock = OS_UNFAIR_LOCK_INIT;

static bool read_word(uint32_t address, uint32_t *value)
{
    if (address < 0x1000 || address > UINT32_MAX - 4) return false;
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, 4,
        (mach_vm_address_t)value, &copied) == KERN_SUCCESS && copied == 4;
}

static bool is_racing(void)
{
    uint32_t slot = 0, stack = 0, vtable = 0, name = 0, paused = 0;
    if (!read_word(layout->stack_pointer_slot, &slot) ||
        !read_word(slot, &stack) || !stack) return false;
    uint32_t state = compat_runtime32_call(layout->stack_top, &stack, 1);
    return !compat_runtime32_last_call_trapped() &&
        read_word(state, &vtable) && read_word(vtable, &name) &&
        name == layout->race_name && read_word(state + 0x2c, &paused) && !paused;
}

static NSView *find_game_view(NSView *view)
{
    if (!view || view.hidden) return nil;
    Class game_class = NSClassFromString(@"EAGLView");
    if (game_class && [view isKindOfClass:game_class]) return view;
    for (NSView *child in view.subviews) {
        NSView *found = find_game_view(child);
        if (found) return found;
    }
    return nil;
}

static bool focused(void)
{
    /* Rendering in the background and scripted-test overrides never grant
       permission to forward physical controller input to an inactive app. */
    return NSApp.isActive && input_view.window.isKeyWindow &&
        !lp32_suppress_background_input();
}

static NSEvent *mouse_event(NSEventType type, bool down)
{
    NSWindow *window = input_view.window;
    if (!window) return nil;
    NSPoint point = [input_view convertPoint:cursor toView:nil];
    return [NSEvent mouseEventWithType:type location:point modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime
        windowNumber:window.windowNumber context:nil eventNumber:0
        clickCount:type == NSEventTypeMouseMoved ? 0 : 1 pressure:down ? 1 : 0];
}

static void move_cursor(void)
{
    bool dragging = mouse_down_delivered;
    NSEvent *event = mouse_event(dragging ? NSEventTypeLeftMouseDragged :
                                            NSEventTypeMouseMoved, dragging);
    if (!event) return;
    injecting = true;
    if (dragging) [input_view mouseDragged:event];
    else [input_view mouseMoved:event];
    injecting = false;
}

static const unsigned short keys[A6_CLICK] = {
    [A6_UP] = 126, [A6_DOWN] = 125, [A6_LEFT] = 123, [A6_RIGHT] = 124,
    [A6_NITRO] = 49, [A6_BACK] = 53,
    [A6_CAMERA1] = 18, [A6_CAMERA2] = 19, [A6_CAMERA3] = 20
};

static void emit_action(void *unused, enum a6_action action, bool down)
{
    (void)unused;
    injecting = true;
    if (action == A6_CLICK) {
        if (down) move_cursor();
        injecting = true;
        NSEvent *event = mouse_event(down ? NSEventTypeLeftMouseDown :
                                             NSEventTypeLeftMouseUp, down);
        if (event) {
            if (down) [input_view mouseDown:event];
            else [input_view mouseUp:event];
            mouse_down_delivered = down;
        }
    } else {
        NSEvent *event = lp32_key_event(input_view.window, keys[action], down, 0);
        if (event) {
            if (down) [input_view keyDown:event];
            else [input_view keyUp:event];
        }
    }
    injecting = false;
    if (trace) fprintf(stderr, "compat32: Asphalt controller action=%u %s\n",
                       action, down ? "down" : "up");
}

static void clear_samples(void)
{
    os_unfair_lock_lock(&sample_lock);
    sample_head = sample_count = 0;
    sample_overflow = false;
    memset(&current_sample, 0, sizeof(current_sample));
    os_unfair_lock_unlock(&sample_lock);
}

void asphalt6_controller32_focus_lost(void)
{
    if (!installed || ![NSThread isMainThread]) return;
    atomic_store(&accept_samples, false);
    clear_samples();
    a6_input_release_all(&input);
    if (input_view.window) a6_input_commit(&input, emit_action, NULL);
    cursor_time = 0;
}

static void select_view(void)
{
    NSView *view = find_game_view(NSApp.keyWindow.contentView);
    if (view == input_view) return;
    asphalt6_controller32_focus_lost();
    [input_view release];
    input_view = [view retain];
    cursor = NSMakePoint(NSMidX(view.bounds), NSMidY(view.bounds));
}

static struct sample read_gamepad(GCExtendedGamepad *pad)
{
    struct sample sample = {0};
#define BUTTON(element, bit) if ([(element) isPressed]) sample.buttons |= (bit)
    BUTTON(pad.buttonA, A6_A); BUTTON(pad.buttonB, A6_B);
    BUTTON(pad.buttonX, A6_X); BUTTON(pad.buttonY, A6_Y);
    BUTTON(pad.leftShoulder, A6_LB); BUTTON(pad.rightShoulder, A6_RB);
    BUTTON(pad.buttonMenu, A6_MENU); BUTTON(pad.buttonOptions, A6_OPTIONS);
    BUTTON(pad.dpad.left, A6_DLEFT); BUTTON(pad.dpad.right, A6_DRIGHT);
    BUTTON(pad.dpad.up, A6_DUP); BUTTON(pad.dpad.down, A6_DDOWN);
#undef BUTTON
    if (pad.rightTrigger.value >= .12f) sample.buttons |= A6_RT;
    if (pad.leftTrigger.value >= .12f) sample.buttons |= A6_LT;
    sample.x = pad.leftThumbstick.xAxis.value;
    sample.y = pad.leftThumbstick.yAxis.value;
    sample.rx = pad.rightThumbstick.xAxis.value;
    sample.ry = pad.rightThumbstick.yAxis.value;
    return sample;
}

static void enqueue_sample(struct sample sample, unsigned expected_generation)
{
    if (!atomic_load(&accept_samples)) return;
    os_unfair_lock_lock(&sample_lock);
    if (expected_generation == atomic_load(&generation) && atomic_load(&accept_samples)) {
        if (sample_count == sizeof(samples) / sizeof(samples[0])) {
            sample_head = sample_count = 0;
            sample_overflow = true;
        }
        samples[(sample_head + sample_count++) % 128] = sample;
    }
    os_unfair_lock_unlock(&sample_lock);
}

static void refresh_controller(void)
{
    NSArray<GCController *> *devices = GCController.controllers;
    if (controller && ![devices containsObject:controller]) {
        atomic_fetch_add(&generation, 1);
        controller.extendedGamepad.valueChangedHandler = nil;
        [controller release];
        controller = nil;
        clear_samples();
        a6_input_release_pad(&input);
        fprintf(stderr, "compat32: Asphalt controller disconnected\n");
    }
    if (controller) return;
    for (GCController *candidate in devices) {
        GCExtendedGamepad *pad = candidate.extendedGamepad;
        if (!pad) continue;
        controller = [candidate retain];
        unsigned bound_generation = atomic_fetch_add(&generation, 1) + 1;
        controller.playerIndex = GCControllerPlayerIndex1;
        if (@available(macOS 11.0, *)) {
            pad.buttonMenu.preferredSystemGestureState = GCSystemGestureStateAlwaysReceive;
            pad.buttonOptions.preferredSystemGestureState = GCSystemGestureStateAlwaysReceive;
        }
        pad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
            (void)element;
            enqueue_sample(read_gamepad(changed), bound_generation);
        };
        fprintf(stderr, "compat32: Asphalt controller connected (%s)\n",
                candidate.vendorName.UTF8String ?: "gamepad");
        break;
    }
}

void asphalt6_controller32_poll(void)
{
    if (!installed || injecting || ![NSThread isMainThread]) return;
    select_view();
    refresh_controller();
    if (!focused()) {
        asphalt6_controller32_focus_lost();
        return;
    }
    a6_input_mode(&input, is_racing());
    atomic_store(&accept_samples, true);
    if (controller) enqueue_sample(read_gamepad(controller.extendedGamepad), atomic_load(&generation));
    struct sample pending[128];
    unsigned count;
    bool overflow;
    os_unfair_lock_lock(&sample_lock);
    count = sample_count;
    overflow = sample_overflow;
    for (unsigned i = 0; i < count; ++i) pending[i] = samples[(sample_head + i) % 128];
    sample_head = sample_count = 0;
    sample_overflow = false;
    os_unfair_lock_unlock(&sample_lock);
    if (overflow) a6_input_release_pad(&input);
    for (unsigned i = 0; i < count; ++i) {
        current_sample = pending[i];
        a6_input_sample(&input, current_sample.buttons, current_sample.x);
    }
    double now = NSProcessInfo.processInfo.systemUptime;
    float x = current_sample.rx, y = current_sample.ry;
    if (!input.racing) {
        if (fabsf(current_sample.x) > fabsf(x)) x = current_sample.x;
        if (fabsf(current_sample.y) > fabsf(y)) y = current_sample.y;
        if (current_sample.buttons & A6_DLEFT) x = -1;
        if (current_sample.buttons & A6_DRIGHT) x = 1;
        if (current_sample.buttons & A6_DUP) y = 1;
        if (current_sample.buttons & A6_DDOWN) y = -1;
    }
    float dx, dy;
    a6_cursor_delta(x, y, cursor_time ? now - cursor_time : 0, &dx, &dy);
    cursor_time = now;
    NSRect bounds = input_view.bounds;
    NSPoint previous = cursor;
    cursor.x = fmax(NSMinX(bounds), fmin(NSMaxX(bounds) - 1, cursor.x + dx));
    cursor.y = fmax(NSMinY(bounds), fmin(NSMaxY(bounds) - 1,
        cursor.y + (input_view.isFlipped ? -dy : dy)));
    if (!NSEqualPoints(previous, cursor)) move_cursor();
}

int asphalt6_controller32_event(void *raw_view, const char *selector, void *raw_event)
{
    if (!installed || injecting || ![NSThread isMainThread]) return 0;
    NSView *view = raw_view;
    NSEvent *event = raw_event;
    if (!event || !view.window) return 0;
    if (view != input_view) select_view();
    if (view != input_view) return 0;
    if (!strncmp(selector, "mouse", 5)) {
        if (focused()) cursor = [view convertPoint:event.locationInWindow fromView:nil];
        if (strcmp(selector, "mouseDown:") && strcmp(selector, "mouseUp:")) return 0;
        if (focused()) a6_input_key(&input, A6_CLICK, !strcmp(selector, "mouseDown:"));
        return 1;
    }
    for (unsigned i = 0; i < A6_CLICK; ++i) {
        if (event.keyCode != keys[i]) continue;
        if (!event.isARepeat && event.characters.length && focused()) {
            bool down = !strcmp(selector, "keyDown:");
            a6_input_key(&input, (enum a6_action)i, down);
            if (down && i >= A6_CAMERA1 && i <= A6_CAMERA3)
                input.camera = i - A6_CAMERA1;
        }
        return 1;
    }
    return 0;
}

uint64_t asphalt6_controller32_commit(const uint32_t *arguments)
{
    if (!layout) return 0;
    if ([NSThread isMainThread]) {
        asphalt6_controller32_poll();
        if (input_view.window) a6_input_commit(&input, emit_action, NULL);
    }
    return compat_runtime32_call(layout->commit, arguments, 1);
}

float asphalt6_controller32_steer(const uint32_t *arguments)
{
    if (!layout) return 0;
    /* Retain the original routine's direction changes, drift bookkeeping,
       and neutral response even when an analog result replaces its return. */
    float original = compat_runtime32_call_float(layout->digital_steering, arguments, 2);
    if (compat_runtime32_last_call_trapped() || !installed ||
        ![NSThread isMainThread] || !analog_enabled ||
        !controller || !input.racing || !focused() ||
        (input.keyboard & ((1u << A6_LEFT) | (1u << A6_RIGHT))) ||
        (input.buttons & (A6_DLEFT | A6_DRIGHT))) return original;
    uint32_t control = arguments[0], car = 0, owner = 0, kind = 0, mode = 0;
    uint32_t flags = 0, reverse = 0, enabled = 0;
    if (!read_word(control, &enabled) || !(enabled & 0xff) ||
        !read_word(control + 4, &car) || !car ||
        !read_word(car + 0xef0, &owner) || owner != control ||
        !read_word(car + 0x324, &kind) || kind != 0 ||
        !read_word(control + 0xa4, &mode) || mode != 0 ||
        !read_word(car + 0x328, &flags) || (flags & 0x00024000) ||
        !read_word(car + 0x788, &reverse) || reverse) return original;
    /* car kind 0 is the branch that queries local keyboard actions in
       0x1e6bc0. Remote/replay controls read their recorded bits instead. */
    uint32_t limit_args[] = {control, 0}, maximum_args[] = {car + 0x54};
    float limit = compat_runtime32_call_float(layout->steering_limit, limit_args, 2);
    if (compat_runtime32_last_call_trapped()) return original;
    float maximum = compat_runtime32_call_float(layout->steering_maximum, maximum_args, 1);
    if (compat_runtime32_last_call_trapped()) return original;
    float car_angle, result;
    if (!a6_steering_angles(input.steering, limit, maximum, &car_angle, &result)) return original;
    memcpy((void *)(uintptr_t)(car + 0x330), &car_angle, sizeof(car_angle));
    /* The original caller still applies limits/corrections and invokes the
       physics setter. Its trigonometric updates remain in the guest. */
    return result;
}

static bool image_matches(const char *path)
{
    @autoreleasepool {
        NSData *data = path ? [NSData dataWithContentsOfFile:@(path)
            options:NSDataReadingMappedIfSafe error:nil] : nil;
        if (!data || data.length > UINT32_MAX) return false;
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
        char hex[65];
        for (unsigned i = 0; i < sizeof(digest); ++i)
            snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        return !strcmp(hex, layout->sha256);
    }
}

static bool signature_matches(const struct lp32_code_signature *signature)
{
    const struct macho_image32 *image = compat_runtime32_image();
    return image && signature->length == 5 && signature->address >= image->text_start &&
        (uint64_t)signature->address + signature->length <= image->text_end &&
        !memcmp((void *)(uintptr_t)signature->address, signature->expected, signature->length);
}

int asphalt6_controller32_install(const char *image_path)
{
    if (installed) return 0;
    layout = lp32_profile()->asphalt6_controller;
    if (!layout || (getenv("LP32_ASPHALT6_CONTROLLERS") &&
                    !strcmp(getenv("LP32_ASPHALT6_CONTROLLERS"), "0"))) return 0;
    if (!image_matches(image_path) || !signature_matches(&layout->commit_call) ||
        !signature_matches(&layout->steering_call)) {
        fprintf(stderr, "compat32: Asphalt controller hooks disabled: unrecognized executable or instructions\n");
        return 0;
    }
    uint32_t targets[] = {
        compat_runtime32_guest_callback(kAsphalt6CommitCallback),
        compat_runtime32_guest_callback(kAsphalt6SteeringCallback)
    };
    const struct lp32_code_signature *sites[] = {&layout->commit_call, &layout->steering_call};
    size_t page_size = (size_t)getpagesize();
    uintptr_t pages[2];
    unsigned writable = 0;
    for (unsigned i = 0; i < 2; ++i) {
        pages[i] = sites[i]->address & ~(uintptr_t)(page_size - 1);
        if (!targets[i] || mprotect((void *)pages[i], page_size, PROT_READ | PROT_WRITE | PROT_EXEC)) {
            for (unsigned j = 0; j < writable; ++j)
                mprotect((void *)pages[j], page_size, PROT_READ | PROT_EXEC);
            fprintf(stderr, "compat32: cannot prepare Asphalt controller hooks\n");
            return -1;
        }
        ++writable;
    }
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *code = (void *)(uintptr_t)sites[i]->address;
        int32_t displacement = (int32_t)(targets[i] - (sites[i]->address + 5));
        memcpy(code + 1, &displacement, 4);
        __builtin___clear_cache((char *)code, (char *)code + 5);
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (mprotect((void *)pages[i], page_size, PROT_READ | PROT_EXEC)) {
            perror("compat32: protect Asphalt controller hooks");
            return -1;
        }
    }
    analog_enabled = !getenv("LP32_ASPHALT6_ANALOG_STEERING") ||
        strcmp(getenv("LP32_ASPHALT6_ANALOG_STEERING"), "0");
    trace = getenv("LP32_TRACE_CONTROLLERS") != NULL;
    installed = true;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    for (NSString *notification in @[NSApplicationWillResignActiveNotification,
                                     NSWindowDidResignKeyNotification]) {
        [center addObserverForName:notification object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *note) {
                if (note.object == NSApp || note.object == input_view.window)
                    asphalt6_controller32_focus_lost();
            }];
    }
    fprintf(stderr, "compat32: Asphalt 6 controller support installed (%s steering)\n",
            analog_enabled ? "analog" : "digital");
    return 0;
}

int asphalt6_controller32_float_self_test(void)
{
    const struct macho_image32 *image = compat_runtime32_image();
    uint32_t value = macho_image32_find_symbol(image, "_a6_float_value");
    uint32_t thunk = compat_runtime32_guest_callback(kAsphalt6SteeringCallback);
    if (!value || !thunk || installed) return -1;
    const struct lp32_asphalt6_controller_layout test_layout = {.digital_steering = value};
    const struct lp32_asphalt6_controller_layout *saved_layout = layout;
    layout = &test_layout;
    bool valid = true;
    unsigned short before, after;
    __asm__ volatile("fnstsw %0" : "=m"(before));
    for (unsigned i = 0; i < 2048 && valid; ++i) {
        float expected = i & 1 ? -.625f : .375f;
        uint32_t arguments[] = {0, 16};
        memcpy(arguments, &expected, 4);
        float actual = compat_runtime32_call_float(thunk, arguments, 2);
        valid = !compat_runtime32_last_call_trapped() && actual == expected;
    }
    __asm__ volatile("fnstsw %0" : "=m"(after));
    valid &= (before & 0x3800) == (after & 0x3800);
    layout = saved_layout;
    fprintf(stderr, "Asphalt nested x87 controller fixture: %s\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : -1;
}
