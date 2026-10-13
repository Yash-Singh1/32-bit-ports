#include "game_profile.h"
#include "macho_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * LEGO Pirates of the Caribbean (TransGaming, 2011; recovered from the SecuROM
 * wrapped LEGOPirates.macbin).  Addresses were recovered from the unpacked
 * image, see analysis/REPORT.md.
 * --------------------------------------------------------------------- */

static const struct lp32_startup_latch_patch pirates_startup_latch = {
    .hook = 0x0005c261,
    .state_true = 0x0005c268,
    .original_frontend = 0x0005c2c0,
    .function_epilogue = 0x0005c331,
    .state_pointer = 0x00c89df0,
};

static const struct lp32_controller_layout pirates_controller = {
    .hid_device_list = 0x00e3c5b0,
    .hid_last_error = 0x00e3c5a4,
    .input_manager_pointer = 0x00cef244,
    .manager_records_offset = 0x4e40,
    .record_stride = 0x17c,
    .record_actions_offset = 0x70,
    .hid_get_element_value = {
        .address = 0x0098531e,
        .expected = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x68},
        .length = 6,
    },
    .hid_build_device_list = {
        .address = 0x0098c2a4,
        .expected = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x58},
        .length = 6,
    },
    .hid_get_first_device = 0x009855b1,
    .hid_get_next_device = 0x009855c2,
    .convert_hid_state = 0x00253b80,
    .glyph_provider_slot = 0x00e0aa00,
    .control_name_slot_check = {
        .address = 0x00250a92,
        .expected = {0x83, 0x7d, 0x0c, 0x03, 0x0f, 0x9e, 0xc0},
        .length = 7,
    },
    .xinput_axis_name_table = 0x00b1aba5,
};

/* Button glyph font (see game_profile.h).  0x32c420 appends the literal "PC"
   to "framework-data\gui\Font\buttons_"; 0x32c5f0 appends the name of GUI
   platform id dword_C75C04 (5 = PC) through the table at 0xb8b790
   {"360","PS3","PS2","WII","PSP","PC","MAC"} and keeps the loaded font in
   dword_C75DB4. */
static const struct lp32_button_font_layout pirates_button_font = {
    .suffix_lea = {
        .address = 0x0032c487,
        .expected = {0x8d, 0x83, 0x6b, 0xfc, 0x69, 0x00},
        .length = 6,
    },
    .pc_suffix_string = 0x009cc09c,
    .ps3_suffix_string = 0x009d09aa,
    .platform_load = {
        .address = 0x0032c6e1,
        .expected = {0x8b, 0x83, 0x03, 0x96, 0x94, 0x00},
        .length = 6,
    },
    .ps3_platform_id = 1,
    .loaded_font_slot = 0x00c75db4,
};

/* SaveCore worker (see game_profile.h).  sub_43D280 (save-system init) is
   called from both the BgProc boot sequence (0x45b38f in sub_45B1F0) and the
   main-thread game init (0x8ec151 in sub_8EBE70).  It allocates the worker
   through sub_FB350 (ctor sub_E2D20 clears the run flag at +0x74), publishes
   it in dword_C94AF8 and calls sub_E1020, which runs three virtual
   initialisers on the worker (one allocates the save buffer at +0x70 that the
   load job's post-handler sub_DB470 dereferences) and then spawns "SaveCore"
   (sub_DA660, which drains the ring at dword_C94B00 with indices +0x60/+0x64)
   through the generic thread starter sub_E0CC0.  The latch sits on that inner
   spawn call so every worker object is still initialised. */
static const struct lp32_save_worker_patch pirates_save_worker = {
    .ctor_running_flag = {
        .address = 0x000e2df6,
        .expected = {0xc6, 0x46, 0x74, 0x00},
        .length = 4,
    },
    .start_call = 0x000e1081,
    .thread_starter = 0x000e0cc0,
};

/* Stage binder sub_1281C0 (called for each material's texture list): after
   validating the stage's texture id it reads the record bound to the stage's
   sampler slot (sub_265B10 on the render context at off_C2C728+0xC4400) and
   tests record+0x14 at 0x1282f4 before rebinding.  Slot 111 (layer0_sampler)
   is empty when the layered material's own program failed to load (its
   begin-draw sub_1E19A0 then skips the block-22 binds), see the header. */
static const struct lp32_texture_bind_guard pirates_texture_bind_guard = {
    .load = {
        .address = 0x001282f4,
        .expected = {0x8b, 0x48, 0x14, 0x85, 0xc9},
        .length = 5,
    },
    .resume = 0x001282f9,
};

static const struct lp32_display_layout pirates_display = {
    .screen_width = 0x00ceedac,
    .screen_height = 0x00ceedb0,
    .refresh_rate = 0x00ceedc4,
    .renderer_display_slot = 0x00dcc0bc,
    .legacy_renderer_display = 0x00cc4be0,
    .frontend_state_pointer = 0x00c89df0,
    .frontend_page_slot = 0x00d0d76c,
    .frontend_gate_slot = 0x00c7821c,
    .frontend_active_word = 0x00c77e84,
};

static const struct lp32_render_pool pirates_render_pool = {
    .free_count = 0x00c2c328,
    .free_head = 0x00c2c32c,
    .mutex = 0x00ca8f80,
    .return_address = 0x0018d64e,
    .object_size = 0x58,
    .object_count = 4096,
};

static const struct lp32_activator_layout pirates_activator = {
    .app_delegate_isa = 0x00e3d390,
    .awake_from_nib = 0x0098126a,
    .did_finish_launching = 0x00981649,
    .text_did_change = 0x009810e0,
    .cancel = 0x0098138d,
    .activate_manually = 0x00981eeb,
    .activate_online = 0x009817cb,
};

static const struct lp32_game_profile pirates_profile = {
    .title = LP32_TITLE_PIRATES,
    .name = "LEGOPirates",
    .display_name = "LEGO Pirates of the Caribbean",
    .log_directory = "LEGOPiratesCompat",
    .image_file = "LEGOPirates.image",
    .entry_eip = 0x000363b0,
    .image_end = 0x00e56000,
    .main_address = 0x000d7fd0,
    .startup_latch = &pirates_startup_latch,
    .controller = &pirates_controller,
    .button_font = &pirates_button_font,
    .save_worker = &pirates_save_worker,
    .texture_bind_guard = &pirates_texture_bind_guard,
    .display = &pirates_display,
    .render_pool = &pirates_render_pool,
    .activator = &pirates_activator,
};

/* ------------------------------------------------------------------------
 * LEGO Star Wars III: The Clone Wars (Feral, 1.0.1, plain i386 Mach-O).
 * --------------------------------------------------------------------- */

/* pcconfig.txt setters: ScreenWidth 0x480a20, ScreenHeight 0x4809f0,
   ScreenRefreshRate 0x480960 -> config block 0xe7cba0 + 0x40c/0x410/0x424. */
static const struct lp32_display_layout clone_wars_display = {
    .screen_width = 0x00e7cfac,
    .screen_height = 0x00e7cfb0,
    .refresh_rate = 0x00e7cfc4,
};

/* Same HID Utilities drop and input-manager code as Pirates, recovered from
   the plain image (analysis/CLONE_WARS_PORT.md): HIDBuildDeviceList is the
   IOServiceAddMatchingNotification caller, gpDeviceList its refcon,
   HIDGetFirstDevice/NextDevice the getters beside HIDGetElementValue; the
   enumerator (0x487420, "IDVID"/"Saitek") stores records at manager+0x4e3c
   with a 380-byte stride, and the prompt formatter (0x4833e0) reads the action
   table at record+0x70, calls the glyph provider slot and gates the XInput
   glyph/axis-name path on `cmp [ebp+0xc],3` at 0x4834b2. */
static const struct lp32_controller_layout clone_wars_controller = {
    .hid_device_list = 0x00fbf64c,
    .hid_last_error = 0x00fbf640,
    .input_manager_pointer = 0x00e7d444,
    .manager_records_offset = 0x4e3c,
    .record_stride = 0x17c,
    .record_actions_offset = 0x70,
    .hid_get_element_value = {
        .address = 0x00aefbd2,
        .expected = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x68},
        .length = 6,
    },
    .hid_build_device_list = {
        .address = 0x00af6b58,
        .expected = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x58},
        .length = 6,
    },
    .hid_get_first_device = 0x00aefe65,
    .hid_get_next_device = 0x00aefe76,
    .convert_hid_state = 0x004817c0,
    .register_hid_device = 0x00487420,
    .read_hid_state = 0x00484660,
    /* sub_487420 must not reuse a live slot after sub_484BE0 clears its
       one-second "new device" flag. Both same-VID/PID matching and the
       fallback free-slot scan currently mistake that flag for occupancy. */
    .hid_slot_occupancy_checks = {
        {.address = 0x00487493, .length = 7,
         .expected = {0x80, 0xba, 0x5a, 0x54, 0, 0, 0}},
        {.address = 0x004874c6, .length = 7,
         .expected = {0x80, 0xbe, 0x5a, 0x54, 0, 0, 0}},
        {.address = 0x004874dc, .length = 7,
         .expected = {0x80, 0xbe, 0xd6, 0x55, 0, 0, 0}},
        {.address = 0x004874f2, .length = 7,
         .expected = {0x80, 0xbe, 0x52, 0x57, 0, 0, 0}},
        {.address = 0x00487508, .length = 7,
         .expected = {0x80, 0xbe, 0xce, 0x58, 0, 0, 0}},
        {.address = 0x0048751e, .length = 7,
         .expected = {0x80, 0xbe, 0x4a, 0x5a, 0, 0, 0}},
        {.address = 0x0048752b, .length = 7,
         .expected = {0x80, 0xbe, 0xc6, 0x5b, 0, 0, 0}},
    },
    .glyph_provider_slot = 0x00f99960,
    .control_name_slot_check = {
        .address = 0x004834b2,
        .expected = {0x83, 0x7d, 0x0c, 0x03, 0x0f, 0x9e, 0xc0},
        .length = 7,
    },
    .xinput_axis_name_table = 0x00c7c905,
};

/* Button glyph font: only the platform-id path exists here (0x54b000 names
   "framework-data\gui\Font\buttons_" + platform table 0xcf31ec[dword_DD9558],
   same {"360","PS3",...,"PC","MAC"} order as Pirates) and keeps the loaded
   font in dword_DD9734. */
static const struct lp32_button_font_layout clone_wars_button_font = {
    .platform_load = {
        .address = 0x0054b0f1,
        .expected = {0x8b, 0x83, 0x47, 0xe5, 0x88, 0x00},
        .length = 6,
    },
    .ps3_platform_id = 1,
    .loaded_font_slot = 0x00dd9734,
};

static const struct lp32_game_profile clone_wars_profile = {
    .title = LP32_TITLE_CLONE_WARS,
    .name = "LEGOCloneWars",
    .display_name = "LEGO Star Wars III: The Clone Wars",
    .log_directory = "LEGOCloneWarsCompat",
    .image_file = "LEGOCloneWars.image",
    .entry_eip = 0x0002550c,
    .image_end = 0x00fe0000,
    .controller = &clone_wars_controller,
    .button_font = &clone_wars_button_font,
    .display = &clone_wars_display,
    .stream_input_callback = 0x00453fb0,
};

/* Valve's launcher executable; Source itself is loaded from guest dylibs. */
static const struct lp32_game_profile portal2_profile = {
    .title = LP32_TITLE_PORTAL2,
    .name = "Portal2",
    .display_name = "Portal 2",
    .log_directory = "Portal2Compat",
    .image_file = "Portal2.image",
    .entry_eip = 0x00001cf0,
    .image_end = 0x0000335c,
    .main_address = 0x00001d60,
    .thread_argument_is_direct = 1,
    .callee_pops_struct_return = 1,
    .steam_app_id = 620,
    .source_data_directory = "Portal2",
    .source_executable = "portal2_osx",
    .source_game_directory = "portal2",
};

/* Steam Mac depot 553: hl2_osx UUID 45DE3007-ADDB-36ED-BE1C-F1FF3383D658.
   _main is exported at 0x1e00. libtier0 passes allocated thread contexts directly;
   launcher's objc_msgSend_stret callers compensate the callee's four-byte pop. */
static const struct lp32_game_profile l4d2_profile = {
    .title = LP32_TITLE_L4D2,
    .name = "L4D2",
    .display_name = "Left 4 Dead 2",
    .log_directory = "L4D2Compat",
    .image_file = "L4D2.image",
    .entry_eip = 0x00001d90,
    .image_end = 0x00004000,
    .main_address = 0x00001e00,
    .thread_argument_is_direct = 1,
    .callee_pops_struct_return = 1,
    .steam_app_id = 550,
    .source_data_directory = "L4D2",
    .source_executable = "hl2_osx",
    .source_game_directory = "left4dead2",
};

static const struct lp32_game_profile unknown_profile = {
    .title = LP32_TITLE_UNKNOWN,
    .name = "unknown",
    .display_name = "unknown title",
    .log_directory = "LP32Compat",
    .image_file = "game.image",
};

static const struct lp32_game_profile *const known_profiles[] = {
    &pirates_profile,
    &clone_wars_profile,
    &portal2_profile,
    &l4d2_profile,
};

static const struct lp32_game_profile *current_profile = &unknown_profile;

const struct lp32_game_profile *lp32_profile(void)
{
    return current_profile;
}

int lp32_profile_is_source(void)
{
    return current_profile->title == LP32_TITLE_PORTAL2 ||
           current_profile->title == LP32_TITLE_L4D2;
}

const struct lp32_game_profile *lp32_profile_named(const char *name)
{
    if (!name) return NULL;
    for (size_t index = 0; index < sizeof(known_profiles) / sizeof(known_profiles[0]); ++index) {
        if (strcasecmp(known_profiles[index]->name, name) == 0) {
            return known_profiles[index];
        }
    }
    /* Accept the friendlier spellings used on the command line. */
    if (strcasecmp(name, "pirates") == 0) return &pirates_profile;
    if (strcasecmp(name, "left4dead2") == 0) return &l4d2_profile;
    if (strcasecmp(name, "clonewars") == 0 || strcasecmp(name, "lsw3") == 0) {
        return &clone_wars_profile;
    }
    return NULL;
}

static uint32_t call_target(const struct macho_image32 *image, uint32_t site)
{
    int32_t relative;
    memcpy(&relative, (const void *)(uintptr_t)(site + 1), sizeof(relative));
    uint32_t target = site + 5 + (uint32_t)relative;
    return target >= image->min_address && target < image->max_address ? target : 0;
}

uint32_t lp32_profile_main_address(const struct macho_image32 *image)
{
    /*
     * crt1's `start` stub ends with `call __start; hlt`.  The C-level
     * `__start` asks dyld for its initializer/terminator hooks through the
     * __dyld section (which this loader does not populate: initializers are
     * run explicitly), then ends with `call main; mov %eax,(%esp); call exit`.
     * Both titles share this crt, so find main as the call preceding that
     * exit sequence rather than executing the crt.
     */
    const uint8_t *stub = (const void *)(uintptr_t)image->entry_eip;
    uint32_t crt_start = 0;
    for (uint32_t offset = 0; offset + 6 <= 96; ++offset) {
        if (stub[offset] != 0xe8 || stub[offset + 5] != 0xf4) continue;
        crt_start = call_target(image, image->entry_eip + offset);
        if (crt_start) break;
    }
    if (!crt_start) return 0;

    const uint8_t *code = (const void *)(uintptr_t)crt_start;
    for (uint32_t offset = 0; offset + 9 <= 0x200; ++offset) {
        /* e8 rel32 ; 89 04 24 ; e8 */
        if (code[offset] != 0xe8 || code[offset + 5] != 0x89 ||
            code[offset + 6] != 0x04 || code[offset + 7] != 0x24 ||
            code[offset + 8] != 0xe8) {
            continue;
        }
        uint32_t target = call_target(image, crt_start + offset);
        if (target) return target;
    }
    return 0;
}

int lp32_profile_select_hint(const struct macho_image32 *image,
                             const struct lp32_game_profile *hint)
{
    current_profile = &unknown_profile;
    const struct lp32_game_profile *selected = NULL;
    const char *override = getenv("LP32_GAME");
    if (override && override[0]) {
        selected = lp32_profile_named(override);
        if (!selected) {
            fprintf(stderr, "game_loader: unknown LP32_GAME profile: %s\n", override);
            return -1;
        }
    } else if (hint) {
        if (!hint->entry_eip || !hint->image_end ||
            hint->entry_eip != image->entry_eip || hint->image_end != image->max_address) {
            fprintf(stderr, "game_loader: %s bundle contains an unrecognised launcher (entry=0x%08x end=0x%08x)\n",
                    hint->name, image->entry_eip, image->max_address);
            return -1;
        }
        selected = hint;
    } else {
        for (size_t index = 0; index < sizeof(known_profiles) / sizeof(known_profiles[0]); ++index) {
            const struct lp32_game_profile *candidate = known_profiles[index];
            if (candidate->entry_eip && candidate->image_end &&
                candidate->entry_eip == image->entry_eip &&
                candidate->image_end == image->max_address) {
                if (selected) {
                    fprintf(stderr, "game_loader: ambiguous image; set LP32_GAME or launch its compatibility bundle\n");
                    return -1;
                }
                selected = candidate;
            }
        }
    }
    if (!selected) {
        fprintf(stderr,
                "game_loader: unrecognised image (entry=0x%08x end=0x%08x); "
                "set LP32_GAME to force a profile\n",
                image->entry_eip, image->max_address);
        return -1;
    }
    current_profile = selected;
    fprintf(stderr, "compat32: title profile %s (%s)\n", selected->name,
            selected->display_name);
    return 0;
}

int lp32_profile_select(const struct macho_image32 *image)
{
    return lp32_profile_select_hint(image, NULL);
}
