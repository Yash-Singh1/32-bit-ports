#include "controller_bridge.h"
#include "asphalt6_controller.h"
#include "compat_runtime.h"
#include "game_profile.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDUsageTables.h>

#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * LEGO Pirates contains Apple's old HID Utilities sources and talks directly
 * to their pRecDevice/pRecElement linked lists.  Current macOS no longer
 * exposes that 32-bit IOHIDDeviceInterface ABI.  Recreate only the records the
 * title consumes and feed them from GameController.framework.
 *
 * The title's built-in "XBOX 360 For Windows (Controller)" preset is used
 * verbatim.  Its PID/VID are presented to the guest, while physical buttons
 * are translated through the title's live action-to-HID mapping table.  This
 * is important because built-in and user mappings can assign the same action
 * to different raw button numbers.
 */
enum {
    kMaximumControllers = 4,
    kAxisCount = 6,
    kHatCount = 1,
    /* The old Mac Xbox driver exposed Button 17 (raw index 16).  Several
       shipped/user mappings use it, so advertising only sixteen buttons makes
       those actions impossible to press. */
    kButtonCount = 17,
    kElementCount = kAxisCount + kHatCount + kButtonCount,

    kGuestDeviceSize = 0x464,
    kGuestElementSize = 0x150,
    kGuestControllerDataBase = 0x7f080000,
    kGuestControllerDataSize = 0x00010000,
    kGuestDeviceBase = kGuestControllerDataBase,
    kGuestElementBase = kGuestControllerDataBase + 0x2000,

    /*
     * Button prompts.  The control-name formatter (Pirates guest 0x002509b0)
     * asks a registered provider function for the display text of a pad
     * button by raw index, and the title's own provider (0x00310470) answers
     * with the font's button glyphs.  Both are only consulted for input slots
     * 0-3, which on Windows hold XInput pads; HID pads live in slots 4-9 and
     * are formatted as "Button N".  The bridge widens the slot test to 0-9
     * and points the provider at itself, since raw indices follow the live
     * HID mapping rather than XInput order.
     */
    kGuestControlNameSlotLimit = 9,
    /* Axis-name index table used by the XInput branch, indexed by
       axis*2+direction.  Entries for axes 2 and 5 are unset there (XInput
       delivers triggers separately); this device has analog triggers on
       those axes, so they name the trigger strings instead of "Hat Up". */
    kGuestAxisNameLeftTrigger = 28,
    kGuestAxisNameRightTrigger = 29,
};

/* Guest addresses of the title's HID Utilities globals, hook sites and input
   manager layout; see game_profile.c. */
static const struct lp32_controller_layout *layout;

enum {
    kDeviceProductName = 0x014,
    kDeviceVendorID = 0x114,
    kDeviceProductID = 0x118,
    kDeviceProductNameSecondary = 0x220,
    kDevicePrimaryUsage = 0x424,
    kDevicePrimaryUsagePage = 0x428,
    kDeviceAxisCount = 0x440,
    kDeviceButtonCount = 0x444,
    kDeviceHatCount = 0x448,
    kDeviceRemovalFlag = 0x458,
    kDeviceElements = 0x45c,
    kDeviceNext = 0x460,

    kElementType = 0x000,
    kElementUsagePage = 0x004,
    kElementUsage = 0x008,
    kElementCookie = 0x00c,
    kElementLogicalMin = 0x010,
    kElementLogicalMax = 0x014,
    kElementCurrentValue = 0x130,
    kElementParent = 0x144,
    kElementChild = 0x148,
    kElementSibling = 0x14c,
};

enum {
    kHIDElementTypeInputMisc = 1,
    kHIDElementTypeInputButton = 2,
    kHIDElementTypeInputAxis = 3,
    kHIDUsagePageGenericDesktop = 1,
    kHIDUsagePageButton = 9,
    kHIDUsageJoystick = 4,
    kHIDUsageGamePad = 5,
    kHIDUsageXAxis = 0x30,
    kHIDUsageYAxis = 0x31,
    kHIDUsageZAxis = 0x32,
    kHIDUsageXRotation = 0x33,
    kHIDUsageYRotation = 0x34,
    kHIDUsageZRotation = 0x35,
    kHIDUsageHatSwitch = 0x39,
};

/* Canonical action IDs recovered from the game's control-name resolver at
   guest 0x00255790.  Back and Accept alias Circle and Cross respectively. */
enum {
    kActionLeftStickX = 0,
    kActionLeftStickY = 2,
    kActionRightStickX = 4,
    kActionRightStickY = 6,
    kActionPadUp = 8,
    kActionPadRight = 9,
    kActionPadDown = 10,
    kActionPadLeft = 11,
    kActionTriangle = 12,
    kActionCircle = 13,
    kActionCross = 14,
    kActionSquare = 15,
    kActionLeftShoulder = 16,
    kActionLeftTrigger = 17,
    kActionRightShoulder = 18,
    kActionRightTrigger = 19,
    kActionLeftStick = 20,
    kActionRightStick = 21,
    kActionSelect = 22,
    kActionStart = 23,
    kActionCount = 24,
};

/* Zero-based HID button indices from the title's stock 045e:028e mapping.
   These are used only before the title has built its live per-slot table. */
static int fallback_raw_button_for_action(unsigned action)
{
    switch (action) {
        case kActionTriangle: return 16;
        case kActionCircle: return 14;
        case kActionCross: return 13;
        case kActionSquare: return 15;
        case kActionLeftShoulder: return 11;
        case kActionRightShoulder: return 10;
        case kActionLeftStick: return 8;
        case kActionRightStick: return 9;
        case kActionSelect: return 7;
        case kActionStart: return 6;
        default: return -1; /* L2/R2 are axes, not buttons. */
    }
}

struct controller_slot {
    GCController *controller;
    bool active;
    bool virtual_device;
    bool persistent_device;
    atomic_uint event_button_mask;
    atomic_uint latched_button_mask;
    atomic_uint latched_release_mask;
    uint32_t queued_button_press_mask;
    uint32_t queued_button_release_mask;
    uint32_t reported_button_mask;
    uint8_t button_latch_frames[kButtonCount];
};

static struct controller_slot controller_slots[kMaximumControllers];
static bool bridge_installed;
static unsigned virtual_controller_count;
static bool virtual_test_script;
static uint64_t virtual_join_swap = 6000;
static uint64_t virtual_join_repeat;
static uint64_t virtual_action_repeat;
static unsigned virtual_action = kActionCross;
static unsigned linked_controller_count;
static uint32_t traced_game_button_masks[10];
static uint32_t traced_action_map_signatures[kMaximumControllers];
static uint32_t glyph_provider_thunk;
static long glyph_probe_base = -1;

static void configure_physical_controller(unsigned index,
                                           GCController *controller);
static int patch_guest_code(uintptr_t address, const uint8_t *expected,
                            size_t expected_size, const uint8_t *patch,
                            size_t patch_size, const char *name);

static uint8_t *guest_device(unsigned index)
{
    return (void *)(uintptr_t)(kGuestDeviceBase + index * kGuestDeviceSize);
}

static uint8_t *guest_element(unsigned controller, unsigned element)
{
    return (void *)(uintptr_t)(kGuestElementBase +
        (controller * kElementCount + element) * kGuestElementSize);
}

static uint32_t guest_address(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static void write_u32(uint8_t *record, unsigned offset, uint32_t value)
{
    memcpy(record + offset, &value, sizeof(value));
}

static uint32_t read_u32(const uint8_t *record, unsigned offset)
{
    uint32_t value;
    memcpy(&value, record + offset, sizeof(value));
    return value;
}

static int16_t read_i16(const uint8_t *record, unsigned offset)
{
    int16_t value;
    memcpy(&value, record + offset, sizeof(value));
    return value;
}

static const uint8_t *game_record_for_controller(unsigned controller_index,
                                                  unsigned *game_slot)
{
    if (!layout->input_manager_pointer) return NULL;
    uint32_t manager =
        *(const volatile uint32_t *)(uintptr_t)layout->input_manager_pointer;
    if (manager < UINT32_C(0x1000) || manager >= UINT32_C(0x70000000)) {
        return NULL;
    }
    uint32_t expected_device = guest_address(guest_device(controller_index));
    for (unsigned slot = 0; slot < 10; ++slot) {
        const uint8_t *record = (const void *)(uintptr_t)(
            manager + layout->manager_records_offset +
            slot * layout->record_stride);
        if (read_u32(record, 4) == expected_device) {
            if (game_slot) *game_slot = slot;
            return record;
        }
    }
    return NULL;
}

/* Each 4-byte action entry is {type, flags, int16 rawIndex}.  Type 1 is a
   digital pad button; the formatter displays it as "Button rawIndex+1". */
static int mapped_raw_button(unsigned controller_index, unsigned action)
{
    if (action >= kActionCount) return -1;
    const uint8_t *record = game_record_for_controller(controller_index,
                                                        NULL);
    if (record) {
        const uint8_t *entry = record + layout->record_actions_offset + action * 4;
        int value = read_i16(entry, 2);
        if (entry[0] == 1 && value >= 0 && value < kButtonCount) {
            return value;
        }
        return -1;
    }
    int fallback = fallback_raw_button_for_action(action);
    return fallback >= 0 && fallback < kButtonCount ? fallback : -1;
}

/* Inverse of mapped_raw_button.  The formatter does not say which slot a
   prompt is for, so the first linked pad's live table decides; the shipped
   preset gives every pad the same table unless the user remapped one. */
static int action_for_raw_button(unsigned raw)
{
    for (unsigned index = 0; index < kMaximumControllers; ++index) {
        if (!controller_slots[index].active) continue;
        const uint8_t *record = game_record_for_controller(index, NULL);
        if (!record) continue;
        for (unsigned action = kActionPadUp; action < kActionCount; ++action) {
            const uint8_t *entry =
                record + layout->record_actions_offset + action * 4;
            if (entry[0] == 1 && read_i16(entry, 2) == (int)raw) {
                return (int)action;
            }
        }
        return -1;
    }
    for (unsigned action = kActionTriangle; action < kActionCount; ++action) {
        if (fallback_raw_button_for_action(action) == (int)raw) return (int)action;
    }
    return -1;
}

static char *append_glyph(char *out, unsigned codepoint)
{
    *out++ = (char)(0xc0 | (codepoint >> 6));
    *out++ = (char)(0x80 | (codepoint & 0x3f));
    return out;
}

/*
 * Output is what the title's own provider produces for XInput pads: the
 * markup "~0" + glyph + "~~" selects the font's button glyph range, where
 * U+0531.. hold A, B, X, Y, LB, RB, (stick arrows), D-pad and Back/Start.
 * The caller's buffer is 16 bytes and the provider's text replaces the
 * "(...)" form, so text fallbacks carry their own parentheses.
 */
void controller_bridge32_button_glyph(uint32_t raw_index, char *out)
{
    if (glyph_probe_base >= 0) {
        char *cursor = out;
        *cursor++ = '~';
        *cursor++ = '0';
        cursor = append_glyph(cursor, (unsigned)glyph_probe_base + raw_index);
        *cursor++ = '~';
        *cursor++ = '~';
        *cursor = 0;
        return;
    }
    unsigned glyph = 0;
    if (raw_index >= 128 && raw_index <= 131) {
        static const unsigned hat_glyphs[4] = {0x53c, 0x53a, 0x539, 0x53b};
        glyph = hat_glyphs[raw_index - 128];
    } else {
        switch (action_for_raw_button(raw_index)) {
            case kActionPadUp: glyph = 0x53c; break;
            case kActionPadRight: glyph = 0x53a; break;
            case kActionPadDown: glyph = 0x539; break;
            case kActionPadLeft: glyph = 0x53b; break;
            case kActionCross: glyph = 0x531; break;
            case kActionCircle: glyph = 0x532; break;
            case kActionSquare: glyph = 0x533; break;
            case kActionTriangle: glyph = 0x534; break;
            case kActionLeftShoulder: glyph = 0x535; break;
            case kActionRightShoulder: glyph = 0x536; break;
            case kActionSelect: glyph = 0x53d; break;
            case kActionStart: glyph = 0x53e; break;
            case kActionLeftStick:
                snprintf(out, 16, "(Left Stick)");
                return;
            case kActionRightStick:
                snprintf(out, 16, "(Right Stick)");
                return;
            default: break;
        }
    }
    if (!glyph) {
        snprintf(out, 16, "(Button %u)", raw_index + 1);
        return;
    }
    char *cursor = out;
    *cursor++ = '~';
    *cursor++ = '0';
    cursor = append_glyph(cursor, glyph);
    *cursor++ = '~';
    *cursor++ = '~';
    *cursor = 0;
}

static void install_button_glyphs(void)
{
    if (getenv("LP32_NO_BUTTON_GLYPHS")) return;
    if (!layout->glyph_provider_slot || !layout->control_name_slot_check.address ||
        !layout->xinput_axis_name_table) {
        fprintf(stderr, "compat32: button prompts keep the title's Button N text\n");
        return;
    }
    const char *probe = getenv("LP32_GLYPH_PROBE");
    if (probe && probe[0]) glyph_probe_base = strtol(probe, NULL, 0);
    uint32_t thunk = compat_runtime32_guest_callback(kControllerGlyphCallbackName);
    if (!thunk) {
        fprintf(stderr, "compat32: no thunk for button glyph provider\n");
        return;
    }
    /* cmp dword [ebp+0xc],3; setle al  ->  cmp dword [ebp+0xc],9; setle al
       The flag gates both the button path (provider) and the axis path
       (XInput stick/trigger names instead of "X Axis Low"). */
    static const uint8_t axis_expected[] = {0, 0};
    static const uint8_t left_trigger[] = {
        kGuestAxisNameLeftTrigger, kGuestAxisNameLeftTrigger,
    };
    static const uint8_t right_trigger[] = {
        kGuestAxisNameRightTrigger, kGuestAxisNameRightTrigger,
    };
    if (patch_guest_code(layout->xinput_axis_name_table + 4, axis_expected, 2,
                         left_trigger, 2, "axis-name table (axis 2)") != 0 ||
        patch_guest_code(layout->xinput_axis_name_table + 10, axis_expected, 2,
                         right_trigger, 2, "axis-name table (axis 5)") != 0) {
        return;
    }
    const struct lp32_code_signature *check = &layout->control_name_slot_check;
    uint8_t patch[sizeof(check->expected)];
    memcpy(patch, check->expected, check->length);
    patch[3] = kGuestControlNameSlotLimit;
    if (memcmp((const void *)(uintptr_t)check->address, check->expected,
               check->length) != 0) {
        fprintf(stderr, "compat32: control-name slot check signature mismatch\n");
        return;
    }
    /* From here on the provider must be ours: the widened check would
       otherwise route HID raw indices through the title's XInput-ordered
       glyph table. */
    glyph_provider_thunk = thunk;
    patch_guest_code(check->address, check->expected, check->length,
                     patch, check->length, "control-name slot check");
    fprintf(stderr, "compat32: button prompts use the title's pad glyphs\n");
}

/* The title registers its own provider during LoadPermData, after the bridge
   is installed, so the slot is re-pointed whenever it drifts. */
static void keep_glyph_provider(void)
{
    if (!glyph_provider_thunk) return;
    uint32_t *slot = (uint32_t *)(uintptr_t)layout->glyph_provider_slot;
    if (*slot != glyph_provider_thunk) *slot = glyph_provider_thunk;
}

/*
 * PlayStation glyphs.  The glyph provider above only emits code points; which
 * artwork they show depends on the button font the title loads, and the
 * archives ship buttons_PS3_nxg.ft2 next to the buttons_PC (Xbox) one it
 * hardwires.  Retarget the two name-building sites (game_profile.h) while a
 * PlayStation pad is the active controller.  The font is loaded once during
 * frontend start-up, so the choice is made from the pads connected until
 * then and frozen afterwards.
 */
enum button_glyph_set {
    kButtonGlyphsXbox,
    kButtonGlyphsPlayStation,
};

static const struct lp32_button_font_layout *button_font;
static enum button_glyph_set applied_glyph_set = kButtonGlyphsXbox;
static bool button_font_frozen;
/* GameController.framework lists pads only after the run loop has turned,
   and the font loads within the first few frames, so the start-up choice
   also looks at the raw HID devices (filled in by connected_hid_gamepad_count). */
static unsigned startup_hid_gamepads;
static unsigned startup_hid_playstation_gamepads;

static bool controller_is_playstation(GCController *controller)
{
    if (!controller) return false;
    NSMutableString *text = [NSMutableString string];
    if (@available(macOS 11.0, *)) {
        NSString *category = [controller productCategory];
        if (category) [text appendString:category];
    }
    NSString *vendor = [controller vendorName];
    if (vendor) [text appendFormat:@" %@", vendor];
    NSString *lower = [text lowercaseString];
    static NSString *const markers[] = {
        @"dualshock", @"dualsense", @"playstation", @"sony", @"ps4", @"ps5",
    };
    for (size_t index = 0; index < sizeof(markers) / sizeof(markers[0]); ++index) {
        if ([lower containsString:markers[index]]) return true;
    }
    return false;
}

static const char *button_glyph_set_name(enum button_glyph_set set)
{
    return set == kButtonGlyphsPlayStation ? "PlayStation" : "Xbox";
}

/* Player 1's pad decides; with slot 0 not yet bound any bound PlayStation pad
   does, and before GameController has reported anything the HID snapshot is
   used when every pad it saw is a PlayStation one.  LP32_BUTTON_GLYPHS=
   xbox|playstation overrides detection. */
static enum button_glyph_set desired_button_glyph_set(void)
{
    const char *override = getenv("LP32_BUTTON_GLYPHS");
    if (override && override[0]) {
        if (strcasecmp(override, "playstation") == 0 ||
            strcasecmp(override, "ps") == 0 || strcasecmp(override, "ps3") == 0 ||
            strcasecmp(override, "sony") == 0) {
            return kButtonGlyphsPlayStation;
        }
        if (strcasecmp(override, "xbox") == 0 || strcasecmp(override, "pc") == 0) {
            return kButtonGlyphsXbox;
        }
    }
    if (controller_slots[0].controller) {
        return controller_is_playstation(controller_slots[0].controller) ?
            kButtonGlyphsPlayStation : kButtonGlyphsXbox;
    }
    bool any_bound = false;
    for (unsigned index = 1; index < kMaximumControllers; ++index) {
        if (!controller_slots[index].controller) continue;
        any_bound = true;
        if (controller_is_playstation(controller_slots[index].controller)) {
            return kButtonGlyphsPlayStation;
        }
    }
    if (!any_bound && startup_hid_gamepads &&
        startup_hid_playstation_gamepads == startup_hid_gamepads) {
        return kButtonGlyphsPlayStation;
    }
    return kButtonGlyphsXbox;
}

static int apply_button_glyph_set(enum button_glyph_set set)
{
    const struct lp32_button_font_layout *font = button_font;
    uint8_t lea_pc[6], lea_ps3[6], load_pc[6], load_ps3[6];
    memcpy(lea_pc, font->suffix_lea.expected, sizeof(lea_pc));
    memcpy(lea_ps3, lea_pc, sizeof(lea_ps3));
    int32_t displacement;
    memcpy(&displacement, lea_pc + 2, sizeof(displacement));
    displacement += (int32_t)(font->ps3_suffix_string - font->pc_suffix_string);
    memcpy(lea_ps3 + 2, &displacement, sizeof(displacement));
    memcpy(load_pc, font->platform_load.expected, sizeof(load_pc));
    /* mov eax, imm32 ; nop  replaces  mov eax, [ebx+disp32] */
    load_ps3[0] = 0xb8;
    memcpy(load_ps3 + 1, &font->ps3_platform_id, sizeof(uint32_t));
    load_ps3[5] = 0x90;

    bool playstation = set == kButtonGlyphsPlayStation;
    if (font->suffix_lea.address &&
        patch_guest_code(font->suffix_lea.address,
                         playstation ? lea_pc : lea_ps3, sizeof(lea_pc),
                         playstation ? lea_ps3 : lea_pc, sizeof(lea_pc),
                         "button font suffix") != 0) {
        return -1;
    }
    if (patch_guest_code(font->platform_load.address,
                         playstation ? load_pc : load_ps3, sizeof(load_pc),
                         playstation ? load_ps3 : load_pc, sizeof(load_pc),
                         "button font platform id") != 0) {
        return -1;
    }
    applied_glyph_set = set;
    return 0;
}

static void update_button_glyph_font(uint64_t swap_count)
{
    if (!button_font || button_font_frozen) return;
    uint32_t loaded =
        *(const volatile uint32_t *)(uintptr_t)button_font->loaded_font_slot;
    if (loaded) {
        button_font_frozen = true;
        fprintf(stderr,
                "compat32: button glyph font loaded with %s glyphs at swap %llu; "
                "pads connected later keep it\n",
                button_glyph_set_name(applied_glyph_set),
                (unsigned long long)swap_count);
        return;
    }
    enum button_glyph_set desired = desired_button_glyph_set();
    if (desired == applied_glyph_set) return;
    if (apply_button_glyph_set(desired) != 0) {
        /* Signature mismatch: leave whatever is in place and stop trying. */
        button_font_frozen = true;
        return;
    }
    fprintf(stderr, "compat32: button prompts will use %s glyphs\n",
            button_glyph_set_name(desired));
}

static void install_button_glyph_font(void)
{
    button_font = lp32_profile()->button_font;
    if (!button_font || getenv("LP32_NO_BUTTON_GLYPHS")) {
        button_font = NULL;
        return;
    }
    if ((button_font->suffix_lea.address &&
         memcmp((const void *)(uintptr_t)button_font->suffix_lea.address,
                button_font->suffix_lea.expected,
                button_font->suffix_lea.length) != 0) ||
        memcmp((const void *)(uintptr_t)button_font->platform_load.address,
               button_font->platform_load.expected,
               button_font->platform_load.length) != 0) {
        fprintf(stderr, "compat32: button font signature mismatch; "
                        "Xbox glyphs only\n");
        button_font = NULL;
    }
}

static int32_t axis_value(float value)
{
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return (int32_t)lrintf(value * 32767.0f);
}

static int hat_value(bool up, bool right, bool down, bool left)
{
    if (up && right) return 1;
    if (right && down) return 3;
    if (down && left) return 5;
    if (left && up) return 7;
    if (up) return 0;
    if (right) return 2;
    if (down) return 4;
    if (left) return 6;
    return 8; /* One beyond logicalMax means centered to the guest. */
}

static void set_element_value(unsigned controller, unsigned element,
                              int32_t value)
{
    write_u32(guest_element(controller, element), kElementCurrentValue,
              (uint32_t)value);
}

static void initialize_element(unsigned controller, unsigned element,
                               uint32_t type, uint32_t usage_page,
                               uint32_t usage, int32_t logical_min,
                               int32_t logical_max)
{
    uint8_t *record = guest_element(controller, element);
    memset(record, 0, kGuestElementSize);
    write_u32(record, kElementType, type);
    write_u32(record, kElementUsagePage, usage_page);
    write_u32(record, kElementUsage, usage);
    write_u32(record, kElementCookie,
              controller * kElementCount + element + 1);
    write_u32(record, kElementLogicalMin, (uint32_t)logical_min);
    write_u32(record, kElementLogicalMax, (uint32_t)logical_max);
    write_u32(record, kElementParent, 0);
    write_u32(record, kElementChild, 0);
    write_u32(record, kElementSibling,
              element + 1 < kElementCount ?
                  guest_address(guest_element(controller, element + 1)) : 0);
    set_element_value(controller, element,
                      element == kAxisCount ? 8 : 0);
}

static void initialize_device(unsigned index)
{
    static const uint32_t axis_usages[kAxisCount] = {
        kHIDUsageXAxis, kHIDUsageYAxis, kHIDUsageZAxis,
        kHIDUsageXRotation, kHIDUsageYRotation, kHIDUsageZRotation,
    };
    uint8_t *device = guest_device(index);
    memset(device, 0, kGuestDeviceSize);
    snprintf((char *)device + kDeviceProductName, 0x100,
             "XBOX 360 For Windows (Controller)");
    snprintf((char *)device + kDeviceProductNameSecondary, 0x100,
             "XBOX 360 For Windows (Controller)");
    write_u32(device, kDeviceVendorID, 0x045e);
    write_u32(device, kDeviceProductID, 0x028e);
    write_u32(device, kDevicePrimaryUsage, kHIDUsageGamePad);
    write_u32(device, kDevicePrimaryUsagePage,
              kHIDUsagePageGenericDesktop);
    write_u32(device, kDeviceAxisCount, kAxisCount);
    write_u32(device, kDeviceButtonCount, kButtonCount);
    write_u32(device, kDeviceHatCount, kHatCount);
    write_u32(device, kDeviceElements,
              guest_address(guest_element(index, 0)));

    for (unsigned axis = 0; axis < kAxisCount; ++axis) {
        bool trigger_axis = axis == 2 || axis == 5;
        initialize_element(index, axis, kHIDElementTypeInputAxis,
                           kHIDUsagePageGenericDesktop, axis_usages[axis],
                           trigger_axis ? 0 : -32768, 32767);
    }
    initialize_element(index, kAxisCount, kHIDElementTypeInputMisc,
                       kHIDUsagePageGenericDesktop, kHIDUsageHatSwitch,
                       0, 7);
    for (unsigned button = 0; button < kButtonCount; ++button) {
        initialize_element(index, kAxisCount + kHatCount + button,
                           kHIDElementTypeInputButton, kHIDUsagePageButton,
                           button + 1, 0, 1);
    }
}

static int patch_guest_code(uintptr_t address, const uint8_t *expected,
                            size_t expected_size, const uint8_t *patch,
                            size_t patch_size, const char *name)
{
    if (memcmp((const void *)address, expected, expected_size) != 0) {
        fprintf(stderr, "compat32: controller %s signature mismatch\n", name);
        return -1;
    }
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = address & ~(page_size - 1);
    if (mprotect((void *)page, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("compat32: mprotect controller guest hook");
        return -1;
    }
    memcpy((void *)address, patch, patch_size);
    __builtin___clear_cache((char *)address, (char *)(address + patch_size));
    if (mprotect((void *)page, page_size, PROT_READ | PROT_EXEC) != 0) {
        perror("compat32: protect controller guest hook");
        return -1;
    }
    return 0;
}

static int install_guest_hid_hooks(void)
{
    static const uint8_t get_value_patch[] = {
        0x8b, 0x4c, 0x24, 0x08,             /* mov 8(%esp),%ecx */
        0x8b, 0x81, 0x30, 0x01, 0x00, 0x00, /* mov 0x130(%ecx),%eax */
        0xc3,                               /* ret */
    };
    static const uint8_t build_patch[] = {
        0x31, 0xc0, /* xor %eax,%eax */
        0xc3,       /* ret */
    };
    const struct lp32_code_signature *get_value = &layout->hid_get_element_value;
    const struct lp32_code_signature *build = &layout->hid_build_device_list;
    if (patch_guest_code(get_value->address, get_value->expected,
                         get_value->length, get_value_patch,
                         sizeof(get_value_patch), "HIDGetElementValue") != 0) {
        return -1;
    }
    if (patch_guest_code(build->address, build->expected, build->length,
                         build_patch, sizeof(build_patch),
                         "HIDBuildDeviceList") != 0) {
        return -1;
    }
    return 0;
}

static int install_game_slot_occupancy_hooks(void)
{
    const size_t count = sizeof(layout->hid_slot_occupancy_checks) /
        sizeof(layout->hid_slot_occupancy_checks[0]);
    for (size_t index = 0; index < count; ++index) {
        const struct lp32_code_signature *check =
            &layout->hid_slot_occupancy_checks[index];
        if (!check->address) continue;
        uint8_t patch[7];
        if (check->length != sizeof(patch)) return -1;
        memcpy(patch, check->expected, sizeof(patch));
        /* cmp byte [record+0x2e],0 -> cmp dword [record+4],0.
           The title clears +0x2e after one second even while the pad is
           connected. +4 remains the device pointer until real removal.
           Preserve the allocator's saved mappings and slot order. */
        patch[0] = 0x83;
        write_u32(patch, 2, read_u32(patch, 2) - (0x2e - 4));
        if (patch_guest_code(check->address, check->expected, check->length,
                             patch, sizeof(patch),
                             "game input-slot occupancy") != 0) return -1;
    }
    if (layout->hid_slot_occupancy_checks[0].address) {
        fprintf(stderr, "compat32: game input slots retain connected controllers\n");
    }
    return 0;
}

static void parse_test_environment(void)
{
    const char *count_text = getenv("LP32_VIRTUAL_CONTROLLERS");
    unsigned long count = count_text ? strtoul(count_text, NULL, 0) : 0;
    if (count > kMaximumControllers) count = kMaximumControllers;
    virtual_controller_count = (unsigned)count;
    virtual_test_script = getenv("LP32_VIRTUAL_CONTROLLER_TEST") != NULL;
    const char *join_text = getenv("LP32_VIRTUAL_JOIN_AT_SWAP");
    if (join_text && join_text[0]) {
        virtual_join_swap = strtoull(join_text, NULL, 0);
    }
    const char *repeat_text = getenv("LP32_VIRTUAL_JOIN_REPEAT");
    if (repeat_text && repeat_text[0]) {
        virtual_join_repeat = strtoull(repeat_text, NULL, 0);
    }
    const char *action_repeat_text =
        getenv("LP32_VIRTUAL_ACTION_REPEAT");
    if (action_repeat_text && action_repeat_text[0]) {
        virtual_action_repeat = strtoull(action_repeat_text, NULL, 0);
    }
    const char *action_text = getenv("LP32_VIRTUAL_ACTION_ID");
    if (action_text && action_text[0]) {
        unsigned long action = strtoul(action_text, NULL, 0);
        if (action >= kActionTriangle && action <= kActionStart) {
            virtual_action = (unsigned)action;
        }
    }
}

static bool controller_is_connected(GCController *controller,
                                    NSArray<GCController *> *connected)
{
    for (GCController *candidate in connected) {
        if (candidate == controller) return true;
    }
    return false;
}

static bool controller_is_assigned(GCController *controller)
{
    for (unsigned index = 0; index < kMaximumControllers; ++index) {
        if (controller_slots[index].controller == controller) return true;
    }
    return false;
}

static bool hid_device_is_playstation(IOHIDDeviceRef device)
{
    NSNumber *vendor = (NSNumber *)IOHIDDeviceGetProperty(
        device, CFSTR(kIOHIDVendorIDKey));
    if ([vendor unsignedIntValue] == 0x054c) return true;
    NSString *product = (NSString *)IOHIDDeviceGetProperty(
        device, CFSTR(kIOHIDProductKey));
    NSString *lower = [product lowercaseString];
    return [lower containsString:@"dualshock"] ||
        [lower containsString:@"dualsense"] ||
        [lower containsString:@"playstation"];
}

static unsigned connected_hid_gamepad_count(void)
{
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault,
                                                  kIOHIDOptionsTypeNone);
    if (!manager) return 0;
    NSDictionary *gamepad = @{
        @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop),
        @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_GamePad),
    };
    NSDictionary *joystick = @{
        @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop),
        @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_Joystick),
    };
    IOHIDManagerSetDeviceMatchingMultiple(
        manager, (CFArrayRef)@[gamepad, joystick]);
    IOReturn result = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    CFSetRef devices = result == kIOReturnSuccess ?
        IOHIDManagerCopyDevices(manager) : NULL;
    CFIndex count = devices ? CFSetGetCount(devices) : 0;
    startup_hid_gamepads = count > 0 ? (unsigned)count : 0;
    startup_hid_playstation_gamepads = 0;
    if (count > 0) {
        const void **values = calloc((size_t)count, sizeof(*values));
        if (values) {
            CFSetGetValues(devices, values);
            for (CFIndex index = 0; index < count; ++index) {
                if (hid_device_is_playstation((IOHIDDeviceRef)values[index])) {
                    ++startup_hid_playstation_gamepads;
                }
            }
            free(values);
        }
    }
    if (devices) CFRelease(devices);
    if (result == kIOReturnSuccess) IOHIDManagerClose(
        manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    if (count < 0) count = 0;
    if (count > kMaximumControllers) count = kMaximumControllers;
    return (unsigned)count;
}

static void reserve_connected_hardware_slots(void)
{
    if (getenv("LP32_DISABLE_PHYSICAL_CONTROLLERS")) return;
    unsigned count = connected_hid_gamepad_count();
    for (unsigned index = 0; index < count; ++index) {
        initialize_device(index);
        controller_slots[index].active = true;
        controller_slots[index].persistent_device = true;
    }
    if (count) {
        fprintf(stderr,
                "compat32: reserved %u startup legacy HID controller slot%s "
                "(%u PlayStation)\n",
                count, count == 1 ? "" : "s", startup_hid_playstation_gamepads);
    }
}

static void clear_controller_input(unsigned index)
{
    struct controller_slot *slot = &controller_slots[index];
    for (unsigned axis = 0; axis < kAxisCount; ++axis) {
        set_element_value(index, axis, 0);
    }
    set_element_value(index, kAxisCount, 8);
    for (unsigned button = 0; button < kButtonCount; ++button) {
        set_element_value(index, kAxisCount + kHatCount + button, 0);
    }
    atomic_store_explicit(&slot->event_button_mask, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&slot->latched_button_mask, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&slot->latched_release_mask, 0,
                          memory_order_relaxed);
    slot->queued_button_press_mask = 0;
    slot->queued_button_release_mask = 0;
    slot->reported_button_mask = 0;
    memset(slot->button_latch_frames, 0,
           sizeof(slot->button_latch_frames));
}

static void bind_physical_controller(unsigned index,
                                     GCController *controller)
{
    struct controller_slot *slot = &controller_slots[index];
    if (!slot->active) initialize_device(index);
    slot->controller = [controller retain];
    slot->active = true;
    slot->virtual_device = false;
    guest_device(index)[kDeviceRemovalFlag] = 0;
    [controller setPlayerIndex:(GCControllerPlayerIndex)index];
    configure_physical_controller(index, controller);
    fprintf(stderr, "compat32: controller %u connected (%s)%s\n",
            index + 1, [[controller vendorName] UTF8String],
            slot->persistent_device ? " in startup slot" : "");
}

static void refresh_hardware_slots(void)
{
    if (getenv("LP32_DISABLE_PHYSICAL_CONTROLLERS")) return;
    NSArray<GCController *> *connected = [GCController controllers];
    for (unsigned index = 0; index < kMaximumControllers; ++index) {
        struct controller_slot *slot = &controller_slots[index];
        if (slot->controller &&
            !controller_is_connected(slot->controller, connected)) {
            fprintf(stderr, "compat32: controller %u disconnected (%s)\n",
                    index + 1, [[slot->controller vendorName] UTF8String]);
            [[slot->controller extendedGamepad] setValueChangedHandler:nil];
            [slot->controller release];
            slot->controller = nil;
            clear_controller_input(index);
            if (slot->persistent_device) {
                slot->active = true;
                guest_device(index)[kDeviceRemovalFlag] = 0;
            } else {
                slot->active = false;
                guest_device(index)[kDeviceRemovalFlag] = 1;
            }
        }
    }
    for (GCController *controller in connected) {
        if (controller_is_assigned(controller) || ![controller extendedGamepad]) {
            continue;
        }
        bool bound = false;
        for (unsigned index = 0; index < kMaximumControllers; ++index) {
            struct controller_slot *slot = &controller_slots[index];
            if (slot->active && slot->persistent_device && !slot->controller) {
                bind_physical_controller(index, controller);
                bound = true;
                break;
            }
        }
        if (bound) continue;
        for (unsigned index = 0; index < kMaximumControllers; ++index) {
            struct controller_slot *slot = &controller_slots[index];
            if (slot->controller || slot->active) continue;
            bind_physical_controller(index, controller);
            break;
        }
    }
}

static void update_linked_list(void)
{
    uint32_t head = 0;
    uint8_t *previous = NULL;
    unsigned count = 0;
    for (unsigned index = 0; index < kMaximumControllers; ++index) {
        uint8_t *device = guest_device(index);
        if (!controller_slots[index].active) {
            write_u32(device, kDeviceNext, 0);
            continue;
        }
        if (!head) head = guest_address(device);
        if (previous) write_u32(previous, kDeviceNext, guest_address(device));
        previous = device;
        ++count;
    }
    if (previous) write_u32(previous, kDeviceNext, 0);
    *(volatile uint32_t *)(uintptr_t)layout->hid_device_list = head;
    if (count != linked_controller_count) {
        linked_controller_count = count;
        fprintf(stderr, "compat32: legacy HID list now has %u controller%s\n",
                count, count == 1 ? "" : "s");
    }
}

static bool pressed(GCControllerButtonInput *button)
{
    return button && [button isPressed];
}

static uint32_t physical_button_mask(unsigned controller_index,
                                     GCExtendedGamepad *gamepad)
{
    uint32_t mask = 0;
#define MAP_ACTION(action, input)                                           \
    do {                                                                    \
        if (pressed((input))) {                                             \
            int raw = mapped_raw_button(controller_index, (action));        \
            if (raw >= 0) mask |= UINT32_C(1) << (unsigned)raw;              \
        }                                                                   \
    } while (0)
    MAP_ACTION(kActionTriangle, [gamepad buttonY]);
    MAP_ACTION(kActionCircle, [gamepad buttonB]);
    MAP_ACTION(kActionCross, [gamepad buttonA]);
    MAP_ACTION(kActionSquare, [gamepad buttonX]);
    MAP_ACTION(kActionLeftShoulder, [gamepad leftShoulder]);
    MAP_ACTION(kActionLeftTrigger, [gamepad leftTrigger]);
    MAP_ACTION(kActionRightShoulder, [gamepad rightShoulder]);
    MAP_ACTION(kActionRightTrigger, [gamepad rightTrigger]);
    MAP_ACTION(kActionLeftStick, [gamepad leftThumbstickButton]);
    MAP_ACTION(kActionRightStick, [gamepad rightThumbstickButton]);
    MAP_ACTION(kActionSelect, [gamepad buttonOptions]);
    MAP_ACTION(kActionStart, [gamepad buttonMenu]);
    MAP_ACTION(kActionStart, [gamepad buttonHome]);
    /* The stock 045e:028e preset binds the D-pad to raw buttons 3-6, not to
       the HID hat, so the hat element alone never reaches those actions. */
    GCControllerDirectionPad *dpad = [gamepad dpad];
    MAP_ACTION(kActionPadUp, [dpad up]);
    MAP_ACTION(kActionPadRight, [dpad right]);
    MAP_ACTION(kActionPadDown, [dpad down]);
    MAP_ACTION(kActionPadLeft, [dpad left]);
#undef MAP_ACTION
    return mask;
}

static void configure_physical_controller(unsigned index,
                                           GCController *controller)
{
    struct controller_slot *slot = &controller_slots[index];
    GCExtendedGamepad *gamepad = [controller extendedGamepad];
    atomic_store_explicit(&slot->event_button_mask, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&slot->latched_button_mask, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&slot->latched_release_mask, 0,
                          memory_order_relaxed);
    slot->queued_button_press_mask = 0;
    slot->queued_button_release_mask = 0;
    slot->reported_button_mask = 0;
    memset(slot->button_latch_frames, 0,
           sizeof(slot->button_latch_frames));

    /*
     * Some central buttons are delayed or swallowed while macOS decides
     * whether they belong to a system gesture.  AlwaysReceive preserves the
     * system gesture while also giving the legacy title an immediate event.
     */
    if (@available(macOS 11.0, *)) {
        NSArray<GCControllerButtonInput *> *central_buttons = @[
            [gamepad buttonMenu],
            [gamepad buttonOptions] ?: (id)[NSNull null],
            [gamepad buttonHome] ?: (id)[NSNull null],
        ];
        for (id candidate in central_buttons) {
            if (candidate != [NSNull null]) {
                [(GCControllerButtonInput *)candidate
                    setPreferredSystemGestureState:
                        GCSystemGestureStateAlwaysReceive];
            }
        }
    }

    /*
     * Polling alone can miss a quick press/release delivered between two
     * rendered frames.  Record both edges.  The poll path serializes them so
     * the guest sees every press and every release at least once.
     */
    [gamepad setValueChangedHandler:^(GCExtendedGamepad *changed,
                                      GCControllerElement *element) {
        (void)element;
        uint32_t mask = physical_button_mask(index, changed);
        uint32_t previous = atomic_exchange_explicit(
            &slot->event_button_mask, mask, memory_order_relaxed);
        uint32_t rising = mask & ~previous;
        uint32_t falling = previous & ~mask;
        if (rising) {
            atomic_fetch_or_explicit(&slot->latched_button_mask, rising,
                                     memory_order_relaxed);
        }
        if (falling) {
            atomic_fetch_or_explicit(&slot->latched_release_mask, falling,
                                     memory_order_relaxed);
        }
        if (getenv("LP32_TRACE_CONTROLLERS")) {
            for (uint32_t bit = 1; bit; bit <<= 1) {
                if (rising & bit) {
                    fprintf(stderr,
                            "compat32: controller %u pressed raw Button %u\n",
                            index + 1, __builtin_ctz(bit) + 1);
                }
                if (falling & bit) {
                    fprintf(stderr,
                            "compat32: controller %u released raw Button %u\n",
                            index + 1, __builtin_ctz(bit) + 1);
                }
            }
        }
    }];
}

static void update_button_elements(unsigned index, uint32_t live_mask)
{
    struct controller_slot *slot = &controller_slots[index];
    uint32_t rising = atomic_exchange_explicit(&slot->latched_button_mask, 0,
                                                memory_order_relaxed);
    uint32_t falling = atomic_exchange_explicit(&slot->latched_release_mask, 0,
                                                 memory_order_relaxed);
    slot->queued_button_press_mask |= rising;
    slot->queued_button_release_mask |= falling;
    for (unsigned button = 0; button < kButtonCount; ++button) {
        uint32_t bit = 1u << button;
        bool reported = (slot->reported_button_mask & bit) != 0;
        bool live = (live_mask & bit) != 0;
        bool active = reported;

        if (reported) {
            if (slot->button_latch_frames[button]) {
                /* Complete the minimum three-poll press even when a quick
                   release was delivered in the same rendered frame. */
                active = true;
                --slot->button_latch_frames[button];
            } else if ((slot->queued_button_release_mask & bit) || !live) {
                /* A falling edge takes priority over a stale/high live poll.
                   This guarantees aim/charge actions see their release. */
                active = false;
                slot->queued_button_release_mask &= ~bit;
            } else {
                active = true;
            }
            if (active && !(slot->queued_button_release_mask & bit)) {
                slot->queued_button_press_mask &= ~bit;
            }
        } else if ((slot->queued_button_press_mask & bit) || live) {
            active = true;
            slot->queued_button_press_mask &= ~bit;
            /* This poll is the first of three visible high reports. */
            slot->button_latch_frames[button] = 2;
        } else {
            active = false;
            slot->queued_button_release_mask &= ~bit;
        }

        if (active) slot->reported_button_mask |= bit;
        else slot->reported_button_mask &= ~bit;
        set_element_value(index, kAxisCount + kHatCount + button,
                          active ? 1 : 0);
    }
}

static void update_mapped_axis(unsigned controller_index, unsigned action,
                               unsigned fallback_axis, float value)
{
    unsigned axis = fallback_axis;
    bool negative = false;
    const uint8_t *record = game_record_for_controller(controller_index,
                                                        NULL);
    if (record) {
        const uint8_t *entry = record + layout->record_actions_offset + action * 4;
        int mapped_axis = read_i16(entry, 2);
        if ((entry[0] != 2 && entry[0] != 3) ||
            mapped_axis < 0 || mapped_axis >= kAxisCount) {
            return;
        }
        axis = (unsigned)mapped_axis;
        /* The old mapping types distinguish the positive and negative halves
           of an analog axis.  The stock trigger bindings use type 2. */
        negative = entry[0] == 3;
    }
    set_element_value(controller_index, axis,
                      axis_value(negative ? -value : value));
}

static void update_physical_controller(unsigned index,
    GCExtendedGamepad *gamepad)
{
    GCControllerDirectionPad *left = [gamepad leftThumbstick];
    GCControllerDirectionPad *right = [gamepad rightThumbstick];
    for (unsigned axis = 0; axis < kAxisCount; ++axis) {
        set_element_value(index, axis, 0);
    }
    update_mapped_axis(index, kActionLeftStickX, 0,
                       [[left xAxis] value]);
    update_mapped_axis(index, kActionLeftStickY, 1,
                       -[[left yAxis] value]);
    update_mapped_axis(index, kActionLeftTrigger, 2,
                       [[gamepad leftTrigger] value]);
    update_mapped_axis(index, kActionRightStickX, 3,
                       [[right xAxis] value]);
    update_mapped_axis(index, kActionRightStickY, 4,
                       -[[right yAxis] value]);
    update_mapped_axis(index, kActionRightTrigger, 5,
                       [[gamepad rightTrigger] value]);

    GCControllerDirectionPad *dpad = [gamepad dpad];
    set_element_value(index, kAxisCount,
        hat_value(pressed([dpad up]), pressed([dpad right]),
                  pressed([dpad down]), pressed([dpad left])));

    /* event_button_mask is callback history for detecting edges, not current
       state.  ORing it here used to let a delayed/missed release callback
       override the authoritative live isPressed state indefinitely. */
    update_button_elements(index, physical_button_mask(index, gamepad));
}

static void set_virtual_action(unsigned controller_index, unsigned action)
{
    int raw = mapped_raw_button(controller_index, action);
    if (raw >= 0) {
        set_element_value(controller_index,
                          kAxisCount + kHatCount + (unsigned)raw, 1);
    }
}

static void update_virtual_controller(unsigned index, uint64_t swap_count)
{
    int32_t motion = 0;
    if (virtual_test_script && !getenv("LP32_VIRTUAL_NO_MOTION")) {
        uint64_t phase = (swap_count / 45 + index * 2) % 8;
        static const int32_t positions[8] = {
            0, 12000, 24000, 12000, 0, -12000, -24000, -12000,
        };
        motion = positions[phase];
    }
    for (unsigned axis = 0; axis < kAxisCount; ++axis) {
        set_element_value(index, axis, 0);
    }
    set_element_value(index, 0, index & 1 ? -motion : motion);
    set_element_value(index, 1, index & 1 ? motion : -motion);
    set_element_value(index, 3, index & 1 ? motion : -motion);
    set_element_value(index, 4, index & 1 ? -motion : motion);
    set_element_value(index, kAxisCount, 8);
    for (unsigned button = 0; button < kButtonCount; ++button) {
        set_element_value(index, kAxisCount + kHatCount + button, 0);
    }
    if (virtual_test_script) {
        uint64_t press_start = virtual_join_swap + index * 90;
        uint64_t press_elapsed = swap_count >= press_start ?
            swap_count - press_start : UINT64_MAX;
        bool start_pressed = press_elapsed < 18;
        if (!start_pressed && virtual_join_repeat &&
            press_elapsed != UINT64_MAX) {
            start_pressed = press_elapsed % virtual_join_repeat < 18;
        }
        if (start_pressed) {
            set_virtual_action(index, kActionStart);
        }
        uint64_t action_start = press_start + 180;
        uint64_t action_elapsed = swap_count >= action_start ?
            swap_count - action_start : UINT64_MAX;
        bool action_pressed = action_elapsed < 18;
        if (!action_pressed && virtual_action_repeat &&
            action_elapsed != UINT64_MAX) {
            action_pressed = action_elapsed % virtual_action_repeat < 18;
        }
        if (action_pressed) {
            set_virtual_action(index, virtual_action);
        }
    }
}

static void trace_action_mapping(unsigned controller_index)
{
    unsigned game_slot = 0;
    const uint8_t *record = game_record_for_controller(controller_index,
                                                        &game_slot);
    if (!record) return;
    static const unsigned actions[] = {
        kActionTriangle, kActionCircle, kActionCross, kActionSquare,
        kActionLeftShoulder, kActionLeftTrigger,
        kActionRightShoulder, kActionRightTrigger,
        kActionLeftStick, kActionRightStick, kActionSelect, kActionStart,
    };
    static const char *names[] = {
        "Triangle", "Circle", "Cross", "Square", "L1", "L2",
        "R1", "R2", "L3", "R3", "Select", "Start",
    };
    uint32_t signature = UINT32_C(2166136261);
    for (unsigned action = 0; action < kActionCount; ++action) {
        const uint8_t *entry = record + layout->record_actions_offset + action * 4;
        signature = (signature ^ entry[0]) * UINT32_C(16777619);
        signature = (signature ^ (uint16_t)read_i16(entry, 2)) *
            UINT32_C(16777619);
    }
    if (!signature) signature = 1;
    if (traced_action_map_signatures[controller_index] == signature) return;
    traced_action_map_signatures[controller_index] = signature;
    for (unsigned action = 0; action < kActionTriangle; ++action) {
        const uint8_t *entry = record + layout->record_actions_offset + action * 4;
        fprintf(stderr,
                "compat32: controller %u action %u type=%u value=%d\n",
                controller_index + 1, action, entry[0],
                read_i16(entry, 2));
    }
    fprintf(stderr,
            "compat32: controller %u game slot=%u live mapping "
            "Triangle=%d Circle=%d Cross=%d Square=%d "
            "L1=%d L2=Axis%d R1=%d R2=Axis%d "
            "L3=%d R3=%d Select=%d Start=%d\n",
            controller_index + 1, game_slot,
            mapped_raw_button(controller_index, kActionTriangle) + 1,
            mapped_raw_button(controller_index, kActionCircle) + 1,
            mapped_raw_button(controller_index, kActionCross) + 1,
            mapped_raw_button(controller_index, kActionSquare) + 1,
            mapped_raw_button(controller_index, kActionLeftShoulder) + 1,
            read_i16(record + layout->record_actions_offset + kActionLeftTrigger * 4, 2),
            mapped_raw_button(controller_index, kActionRightShoulder) + 1,
            read_i16(record + layout->record_actions_offset + kActionRightTrigger * 4, 2),
            mapped_raw_button(controller_index, kActionLeftStick) + 1,
            mapped_raw_button(controller_index, kActionRightStick) + 1,
            mapped_raw_button(controller_index, kActionSelect) + 1,
            mapped_raw_button(controller_index, kActionStart) + 1);
    for (unsigned index = 0;
         index < sizeof(actions) / sizeof(actions[0]); ++index) {
        const uint8_t *entry = record + layout->record_actions_offset + actions[index] * 4;
        fprintf(stderr,
                "compat32: controller %u action %s type=%u value=%d\n",
                controller_index + 1, names[index], entry[0],
                read_i16(entry, 2));
    }
}

static void trace_game_controller_states(uint64_t swap_count)
{
    if (!getenv("LP32_TRACE_CONTROLLERS") || !layout->input_manager_pointer) return;
    uint32_t manager =
        *(const volatile uint32_t *)(uintptr_t)layout->input_manager_pointer;
    if (manager < UINT32_C(0x1000) || manager >= UINT32_C(0x70000000)) {
        return;
    }
    for (unsigned index = 0; index < kMaximumControllers; ++index) {
        if (controller_slots[index].active) trace_action_mapping(index);
    }
    for (unsigned index = 0; index < 10; ++index) {
        const uint8_t *record = (const void *)(uintptr_t)(
            manager + layout->manager_records_offset +
            index * layout->record_stride);
        uint32_t device = read_u32(record, 4);
        if (!device) continue;
        uint32_t mask = read_u32(record, 0x178);
        if (swap_count == 1) {
            uint32_t mapping_count = read_u32(
                (const void *)(uintptr_t)manager, 0x67f0);
            fprintf(stderr,
                    "compat32: game controller slot=%u device=0x%08x "
                    "axes=%u buttons=%u hats=%u mappings=%u\n",
                    index, device, record[0x1a], record[0x18],
                    record[0x1b], mapping_count);
        }
        if (mask != traced_game_button_masks[index]) {
            fprintf(stderr,
                    "compat32: game controller slot=%u swap=%llu "
                    "device=0x%08x buttons=0x%08x\n",
                    index, (unsigned long long)swap_count, device, mask);
            traced_game_button_masks[index] = mask;
        }
    }
}

int controller_bridge32_install(const char *image_path)
{
    if (lp32_profile()->asphalt6_controller)
        return asphalt6_controller32_install(image_path);
    if (bridge_installed) return 0;
    layout = lp32_profile()->controller;
    if (!layout) {
        fprintf(stderr,
                "compat32: no controller layout for %s; legacy HID bridge "
                "not installed\n", lp32_profile()->name);
        return 0;
    }
    if (mprotect((void *)(uintptr_t)kGuestControllerDataBase,
                 kGuestControllerDataSize,
                 PROT_READ | PROT_WRITE) != 0) {
        perror("compat32: mprotect controller guest data");
        return -1;
    }
    memset((void *)(uintptr_t)kGuestControllerDataBase, 0,
           kGuestControllerDataSize);
    if (install_guest_hid_hooks() != 0) return -1;
    if (install_game_slot_occupancy_hooks() != 0) return -1;
    install_button_glyphs();
    install_button_glyph_font();
    parse_test_environment();

    if (@available(macOS 11.3, *)) {
        [GCController setShouldMonitorBackgroundEvents:YES];
    }
    if (virtual_controller_count) {
        for (unsigned index = 0; index < virtual_controller_count; ++index) {
            initialize_device(index);
            controller_slots[index].active = true;
            controller_slots[index].virtual_device = true;
        }
        fprintf(stderr,
                "compat32: enabled %u deterministic virtual controller%s%s\n",
                virtual_controller_count,
                virtual_controller_count == 1 ? "" : "s",
                virtual_test_script ? " with scripted input" : "");
    } else {
        reserve_connected_hardware_slots();
        refresh_hardware_slots();
    }
    update_button_glyph_font(0);
    update_linked_list();
    bridge_installed = true;
    fprintf(stderr, "compat32: installed modern controller/legacy HID bridge\n");
    return 0;
}

void controller_bridge32_poll(uint64_t swap_count)
{
    if (lp32_profile()->asphalt6_controller) {
        asphalt6_controller32_poll();
        return;
    }
    if (!bridge_installed) return;
    keep_glyph_provider();
    if (!virtual_controller_count) refresh_hardware_slots();
    update_button_glyph_font(swap_count);
    for (unsigned index = 0; index < kMaximumControllers; ++index) {
        struct controller_slot *slot = &controller_slots[index];
        if (!slot->active) continue;
        if (slot->virtual_device) {
            update_virtual_controller(index, swap_count);
        } else if (slot->controller) {
            update_physical_controller(index,
                                       [slot->controller extendedGamepad]);
        }
    }
    update_linked_list();
    trace_game_controller_states(swap_count);
    if (getenv("LP32_TRACE_CONTROLLERS") &&
        (swap_count <= 10 || swap_count % 300 == 0)) {
        int first_start = mapped_raw_button(0, kActionStart);
        int second_start = mapped_raw_button(1, kActionStart);
        fprintf(stderr,
                "compat32: controller poll swap=%llu linked=%u "
                "p1=(%d,%d start=%u) p2=(%d,%d start=%u)\n",
                (unsigned long long)swap_count, linked_controller_count,
                (int32_t)read_u32(guest_element(0, 0), kElementCurrentValue),
                (int32_t)read_u32(guest_element(0, 1), kElementCurrentValue),
                read_u32(guest_element(0,
                                      kAxisCount + kHatCount +
                                          (unsigned)first_start),
                         kElementCurrentValue),
                (int32_t)read_u32(guest_element(1, 0), kElementCurrentValue),
                (int32_t)read_u32(guest_element(1, 1), kElementCurrentValue),
                read_u32(guest_element(1,
                                      kAxisCount + kHatCount +
                                          (unsigned)second_start),
                         kElementCurrentValue));
    }
}

static int self_test_failure(const char *message)
{
    fprintf(stderr, "Controller bridge self-test: FAIL (%s)\n", message);
    return -1;
}

/* The leaf HID tests cannot catch two devices being assigned to the same
   game input record. Exercise Clone Wars' actual registration and per-slot
   conversion, including a second pad arriving after its one-second "new
   device" flag has expired. No frontend or user settings are loaded. */
static int self_test_game_slot_allocation(void)
{
    if (!layout->register_hid_device || !layout->read_hid_state) return 0;
    uint32_t *manager_pointer =
        (void *)(uintptr_t)layout->input_manager_pointer;
    uint32_t saved_manager = *manager_pointer;
    uint32_t manager = compat_runtime32_allocate(0x7000, 1);
    if (!manager) return self_test_failure("test input manager allocation");
    *manager_pointer = manager;
    uint8_t *first_record = (void *)(uintptr_t)(manager +
        layout->manager_records_offset + 4 * layout->record_stride);
    uint8_t *second_record = first_record + layout->record_stride;
    uint32_t first = guest_address(guest_device(0));
    uint32_t second = guest_address(guest_device(1));
    uint32_t original_product = read_u32(guest_device(1), kDeviceProductID);
    const char *failure = NULL;

    compat_runtime32_call(layout->register_hid_device, &first, 1);
    if (compat_runtime32_last_call_trapped() || read_u32(first_record, 4) != first) {
        failure = "first pad game-slot registration";
        goto done;
    }
    /* sub_484BE0 clears this temporary flag after one second. The device
       pointer and the connected flag at +0x2d remain live. */
    first_record[0x2e] = 0;
    compat_runtime32_call(layout->register_hid_device, &second, 1);
    if (compat_runtime32_last_call_trapped() ||
        read_u32(first_record, 4) != first ||
        read_u32(second_record, 4) != second) {
        fprintf(stderr, "Controller bridge self-test: game slots 4/5="
                "0x%08x/0x%08x expected=0x%08x/0x%08x\n",
                read_u32(first_record, 4), read_u32(second_record, 4),
                first, second);
        failure = "late second pad must not replace player one's game slot";
        goto done;
    }

    /* Simultaneous movement and different buttons must survive the game's
       per-slot input copy, not just the bridge's individual HID records. */
    clear_controller_input(0);
    clear_controller_input(1);
    set_element_value(0, 0, 12345);
    set_element_value(1, 0, -23456);
    /* The frontend hasn't initialized semantic action descriptors here;
       feed raw reports using the stock preset's Cross/Start indices. */
    int cross = fallback_raw_button_for_action(kActionCross);
    int start = fallback_raw_button_for_action(kActionStart);
    set_element_value(0, kAxisCount + kHatCount + (unsigned)cross, 1);
    set_element_value(1, kAxisCount + kHatCount + (unsigned)start, 1);
    uint8_t *state = (void *)(uintptr_t)(manager + 0x6d00);
    for (unsigned index = 0; index < 2; ++index) {
        uint32_t arguments[] = {manager, 4 + index, guest_address(state)};
        uint32_t result = compat_runtime32_call(layout->read_hid_state,
                                                arguments, 3);
        if (compat_runtime32_last_call_trapped() || result != 1 ||
            read_i16(state, 0) != (index ? -23456 : 12345) ||
            state[0x40 + cross] != (index ? 0 : 0xff) ||
            state[0x40 + start] != (index ? 0xff : 0)) {
            failure = "independent axes/buttons through real game slots";
            goto done;
        }
    }

    /* A genuinely disconnected slot remains reusable; the other pad stays
       assigned. The title's removal path clears these three fields. */
    write_u32(second_record, 4, 0);
    write_u32(second_record, 0, 0);
    second_record[0x2d] = 0;
    second_record[0x2e] = 0;
    compat_runtime32_call(layout->register_hid_device, &second, 1);
    if (compat_runtime32_last_call_trapped() ||
        read_u32(first_record, 4) != first ||
        read_u32(second_record, 4) != second) {
        failure = "reconnect without replacing another live game slot";
        goto done;
    }

    /* Exercise every fallback vacancy check, including a different VID/PID
       so the matching-preset path cannot hide a broken free-slot scan. Slot
       records stand in for other live pads; their pointers aren't polled. */
    write_u32(guest_device(1), kDeviceProductID, original_product + 1);
    for (unsigned vacancy = 1; vacancy <= 6; ++vacancy) {
        memset(first_record, 0, 6 * layout->record_stride);
        for (unsigned index = 0; index < 6; ++index) {
            write_u32(first_record + index * layout->record_stride, 4,
                      index == vacancy ? 0 : first);
        }
        compat_runtime32_call(layout->register_hid_device, &second, 1);
        if (compat_runtime32_last_call_trapped()) {
            failure = "fallback game-slot registration";
            goto done;
        }
        for (unsigned index = 0; index < 6; ++index) {
            if (read_u32(first_record + index * layout->record_stride, 4) !=
                (index == vacancy ? second : first)) {
                failure = "fallback/full game slots must retain live pads";
                goto done;
            }
        }
    }

done:
    write_u32(guest_device(1), kDeviceProductID, original_product);
    *manager_pointer = saved_manager;
    compat_runtime32_deallocate(manager);
    if (!failure) {
        printf("Controller game-slot self-test: PASS (late second pad, "
               "independent axes/buttons, reconnect, all fallback slots, "
               "full capacity)\n");
    }
    return failure ? self_test_failure(failure) : 0;
}

int controller_bridge32_run_self_test(void)
{
    if (!bridge_installed) return self_test_failure("bridge not installed");
    if (virtual_controller_count < 2) {
        return self_test_failure("LP32_VIRTUAL_CONTROLLERS must be at least 2");
    }
    if (!virtual_test_script) {
        return self_test_failure("LP32_VIRTUAL_CONTROLLER_TEST is required");
    }
    controller_bridge32_poll(virtual_join_swap + 90);

    uint32_t result = compat_runtime32_call(layout->hid_build_device_list.address,
                                            (uint32_t[]){1, 5}, 2);
    if (compat_runtime32_last_call_trapped() || result != 0) {
        return self_test_failure("guest HIDBuildDeviceList hook");
    }
    uint32_t first = compat_runtime32_call(layout->hid_get_first_device, NULL, 0);
    uint32_t second = compat_runtime32_call(layout->hid_get_next_device, &first, 1);
    uint32_t third = second ?
        compat_runtime32_call(layout->hid_get_next_device, &second, 1) : 0;
    if (first != guest_address(guest_device(0)) ||
        second != guest_address(guest_device(1)) || third != 0) {
        return self_test_failure("two-device guest enumeration");
    }
    if (read_u32((const void *)(uintptr_t)first, kDeviceVendorID) != 0x045e ||
        read_u32((const void *)(uintptr_t)first, kDeviceProductID) != 0x028e ||
        read_u32((const void *)(uintptr_t)first, kDevicePrimaryUsage) != 5 ||
        read_u32((const void *)(uintptr_t)first, kDevicePrimaryUsagePage) != 1 ||
        read_u32((const void *)(uintptr_t)first, kDeviceAxisCount) !=
            kAxisCount ||
        read_u32((const void *)(uintptr_t)first, kDeviceButtonCount) !=
            kButtonCount ||
        read_u32((const void *)(uintptr_t)first, kDeviceHatCount) !=
            kHatCount ||
        strcmp((const char *)(uintptr_t)first +
                   kDeviceProductNameSecondary,
               "XBOX 360 For Windows (Controller)") != 0) {
        return self_test_failure("legacy Xbox metadata");
    }
    for (unsigned device_index = 0; device_index < 2; ++device_index) {
        uint32_t element = read_u32(guest_device(device_index),
                                    kDeviceElements);
        unsigned element_count = 0;
        while (element) {
            uint32_t first_expected = guest_address(
                guest_element(device_index, 0));
            uint32_t last_expected = guest_address(
                guest_element(device_index, kElementCount - 1));
            if (element < first_expected || element > last_expected ||
                (element - first_expected) % kGuestElementSize != 0) {
                return self_test_failure("invalid legacy element link");
            }
            ++element_count;
            if (element_count > kElementCount) {
                return self_test_failure("cyclic legacy element list");
            }
            element = read_u32((const void *)(uintptr_t)element,
                               kElementSibling);
        }
        if (element_count != kElementCount) {
            return self_test_failure("legacy element count");
        }
    }
    int first_start_raw = mapped_raw_button(0, kActionStart);
    int second_start_raw = mapped_raw_button(1, kActionStart);
    if (first_start_raw < 0 || second_start_raw < 0) {
        return self_test_failure("Start action mapping");
    }
    uint32_t second_start = guest_address(guest_element(
        1, kAxisCount + kHatCount + (unsigned)second_start_raw));
    uint32_t arguments[] = {second, second_start};
    result = compat_runtime32_call(layout->hid_get_element_value.address, arguments, 2);
    if (compat_runtime32_last_call_trapped() || result != 1) {
        return self_test_failure("independent player-two Start report");
    }
    uint32_t first_start = guest_address(guest_element(
        0, kAxisCount + kHatCount + (unsigned)first_start_raw));
    arguments[0] = first;
    arguments[1] = first_start;
    result = compat_runtime32_call(layout->hid_get_element_value.address, arguments, 2);
    if (compat_runtime32_last_call_trapped() || result != 0) {
        return self_test_failure("player-one/player-two report isolation");
    }

    /* Exercise the title's real recursive HID converter, not only our leaf
       value hook.  This is the exact path used by the input manager each
       frame before controller presets are applied. */
    uint8_t *converted =
        (void *)(uintptr_t)(kGuestControllerDataBase + 0xf000);
    memset(converted, 0xa5, 0x110);
    set_element_value(0,
                      kAxisCount + kHatCount + (unsigned)first_start_raw, 1);
    uint32_t convert_arguments[] = {
        guest_address(converted), guest_address(guest_device(0)),
    };
    uint32_t pending_hid_error =
        *(const volatile uint32_t *)(uintptr_t)layout->hid_last_error;
    result = compat_runtime32_call(layout->convert_hid_state,
                                   convert_arguments, 2);
    if (compat_runtime32_last_call_trapped() || result != 1 ||
        converted[0x30 + first_start_raw] != 0xff) {
        fprintf(stderr,
                "Controller bridge self-test: converter result=%u "
                "rawStart=0x%02x pendingHIDError=0x%08x\n",
                result, converted[0x30 + first_start_raw],
                pending_hid_error);
        return self_test_failure("original high-level HID conversion");
    }
    set_element_value(0,
                      kAxisCount + kHatCount + (unsigned)first_start_raw, 0);

    /* Regression for the physical-controller failure that originally exposed
       only sixteen buttons: the guest preset can bind an action to raw index
       16 and formats that binding as "Button 17". */
    set_element_value(0, kAxisCount + kHatCount + 16, 1);
    memset(converted, 0, 0x110);
    result = compat_runtime32_call(layout->convert_hid_state,
                                   convert_arguments, 2);
    if (compat_runtime32_last_call_trapped() || result != 1 ||
        converted[0x30 + 16] != 0xff) {
        return self_test_failure("Button 17 high-index conversion");
    }
    set_element_value(0, kAxisCount + kHatCount + 16, 0);

    /* The old preset binds R2 to raw axis 5.  Keeping only the four stick
       axes would silently read zero there (and put the right stick on the
       L2 axis), so exercise the highest legacy axis too. */
    set_element_value(0, 5, 12345);
    memset(converted, 0, 0x110);
    result = compat_runtime32_call(layout->convert_hid_state,
                                   convert_arguments, 2);
    if (compat_runtime32_last_call_trapped() || result != 1 ||
        (int32_t)read_u32(converted, 4 * 5) != 12345) {
        fprintf(stderr,
                "Controller bridge self-test: R2 result=%u axis5=%d "
                "element=%d trapped=%d\n",
                result, (int32_t)read_u32(converted, 4 * 5),
                (int32_t)read_u32(guest_element(0, 5),
                                  kElementCurrentValue),
                compat_runtime32_last_call_trapped());
        return self_test_failure("six-axis trigger conversion");
    }
    set_element_value(0, 5, 0);

    atomic_store_explicit(&controller_slots[0].latched_button_mask,
                          1u << (unsigned)first_start_raw,
                          memory_order_relaxed);
    for (unsigned frame = 0; frame < 3; ++frame) {
        update_button_elements(0, 0);
        if (read_u32(guest_element(0,
                                  kAxisCount + kHatCount +
                                      (unsigned)first_start_raw),
                     kElementCurrentValue) != 1) {
            return self_test_failure("latched Start visibility");
        }
    }
    update_button_elements(0, 0);
    if (read_u32(guest_element(0,
                              kAxisCount + kHatCount +
                                  (unsigned)first_start_raw),
                 kElementCurrentValue) != 0) {
        return self_test_failure("latched Start release");
    }

    /* A release callback must become visible even if the accompanying live
       sample is still stale/high.  This is the throw-after-aim regression:
       hold high for three polls, force one low report, then permit a genuine
       live high sample to begin a new press. */
    uint32_t start_bit = 1u << (unsigned)first_start_raw;
    atomic_store_explicit(&controller_slots[0].latched_button_mask,
                          start_bit, memory_order_relaxed);
    update_button_elements(0, start_bit);
    atomic_store_explicit(&controller_slots[0].latched_release_mask,
                          start_bit, memory_order_relaxed);
    update_button_elements(0, start_bit);
    update_button_elements(0, start_bit);
    update_button_elements(0, start_bit);
    if (read_u32(guest_element(0,
                              kAxisCount + kHatCount +
                                  (unsigned)first_start_raw),
                 kElementCurrentValue) != 0) {
        return self_test_failure("queued release over stale live state");
    }
    update_button_elements(0, start_bit);
    if (read_u32(guest_element(0,
                              kAxisCount + kHatCount +
                                  (unsigned)first_start_raw),
                 kElementCurrentValue) != 1) {
        return self_test_failure("press after queued release");
    }
    clear_controller_input(0);

    controller_bridge32_poll(0);
    int32_t first_x = (int32_t)read_u32(guest_element(0, 0),
                                        kElementCurrentValue);
    int32_t second_x = (int32_t)read_u32(guest_element(1, 0),
                                         kElementCurrentValue);
    if (first_x == second_x) {
        return self_test_failure("player-one/player-two axis isolation");
    }
    if (self_test_game_slot_allocation() != 0) return -1;
    printf("Controller bridge self-test: PASS "
           "(2 devices, 6 axes, 17 buttons, original HID conversion "
           "including R2/Button 17, independent P2 report, "
           "3-frame press latch and queued release edge)\n");
    return 0;
}
