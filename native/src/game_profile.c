#include "game_profile.h"
#include "macho_loader.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <CommonCrypto/CommonDigest.h>

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

/* Feral 1.0.1 (2014). HID Utilities is the same GCC library as Clone
   Wars. Feral's newer input layer applies ControllerMappings.txt itself. */
static const struct lp32_controller_layout marvel_controller = {
    .hid_device_list = 0x0160895c,
    .hid_last_error = 0x01608950,
    .hid_get_element_value = {
        .address = 0x00fefee2,
        .expected = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x68},
        .length = 6,
    },
    .hid_build_device_list = {
        .address = 0x00ff6e68,
        .expected = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x58},
        .length = 6,
    },
    .hid_get_first_device = 0x00ff0175,
    .hid_get_next_device = 0x00ff0186,
    .convert_hid_state = 0x004311a0,
};

/* ScreenWidth/Height/RefreshRate setters: 0x432a00/0x432a30/0x432b20. */
static const struct lp32_display_layout marvel_display = {
    .screen_width = 0x016b9ccc,
    .screen_height = 0x016b9cd0,
    .refresh_rate = 0x016b9ce4,
};

static const struct lp32_steam_achievement_guard marvel_steam_achievement_guard = {
    .entry = 0x00249350,
    .stats_pointer = 0x0160b554,
    .enabled_check = {0x0024935f, {0x80,0xbe,0x66,0xbe,0x1e,0x01,0x00}, 7},
    .stats_load = {0x0024936e, {0x8b,0x8e,0xf8,0x21,0x3c,0x01,0x8b,0x11}, 8},
    .skip_return = {0x002493a7, {0x83,0xc4,0x14,0x5e,0x5d,0xc3}, 6},
};

/* This crt calls main directly from start. */
static const struct lp32_game_profile marvel_profile = {
    .title = LP32_TITLE_MARVEL,
    .name = "LEGOMarvel",
    .display_name = "LEGO Marvel Super Heroes",
    .log_directory = "LEGOMarvelCompat",
    .image_file = "LEGOMarvel.image",
    .entry_eip = 0x00002720,
    .image_end = 0x01753000,
    .main_address = 0x0025bc00,
    .steam_app_id = 249130,
    .callee_pops_struct_return = 1,
    .thread_argument_is_direct = 1,
    .controller = &marvel_controller,
    .steam_achievement_guard = &marvel_steam_achievement_guard,
    .display = &marvel_display,
    .application_should_terminate = 0x0043d1a0,
    .application_will_unhide = 0x0043d220,
};

/* Feral Complete Saga 1.0 (R17), plain i386 Carbon executable. */
static const struct lp32_game_profile saga_10_profile = {
    .title = LP32_TITLE_COMPLETE_SAGA,
    .name = "LEGOCompleteSaga10",
    .display_name = "LEGO Star Wars: The Complete Saga",
    .log_directory = "LEGOCompleteSagaCompat",
    .image_file = "LEGOCompleteSaga.image",
    .entry_eip = 0x00002460,
    .image_end = 0x0264d000,
    .thread_argument_is_direct = 1, /* 0x341b60 consumes its persistent wrapper */
};

/* Feral's 1.1.1 (RC4) update adds the publisher's online activation flow. */
static const struct lp32_game_profile saga_profile = {
    .title = LP32_TITLE_COMPLETE_SAGA,
    .name = "LEGOCompleteSaga",
    .display_name = "LEGO Star Wars: The Complete Saga 1.1.1",
    .log_directory = "LEGOCompleteSagaCompat",
    .image_file = "LEGOCompleteSaga.image",
    .entry_eip = 0x000355b4,
    .image_end = 0x0276b000,
    .thread_argument_is_direct = 1,
};

/* Steam 1.2.1 RC4 (124676.25672), Clang i386 with an LC_MAIN entry. */
static const struct lp32_game_profile saga_steam_profile = {
    .title = LP32_TITLE_COMPLETE_SAGA,
    .name = "LEGOCompleteSagaSteam",
    .display_name = "LEGO Star Wars: The Complete Saga 1.2.1 (Steam)",
    .log_directory = "LEGOCompleteSagaSteamCompat",
    .image_file = "LEGOCompleteSaga.image",
    .entry_eip = 0x0016ee80,
    .image_end = 0x02ed4640,
    .thread_argument_is_direct = 1,
    .callee_pops_struct_return = 1,
};

static const struct lp32_game_profile unknown_profile = {
    .title = LP32_TITLE_UNKNOWN,
    .name = "unknown",
    .display_name = "unknown title",
    .log_directory = "LP32Compat",
    .image_file = "game.image",
};

static const struct lp32_loading_screen_patch cod4_loading_screen = {
    .client_state_pointer = 0x00407130,
    .redraw = {0x000bb720, {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08}, 6},
    .is_main_thread = {0x00324e50, {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08}, 6},
    .milliseconds = {0x00032fb0, {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08}, 6},
    .entry_points = {
        {0x00262e90, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53}, 6}, /* Scr_LoadScriptInternal */
        {0x00148550, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53}, 6}, /* G_ParseSpawnVars */
    },
    .vm_loop_timer_call = 0x0027e141,
};

static const struct lp32_game_profile cod4_profile = {
    .title = LP32_TITLE_COD4,
    .name = "COD4",
    .display_name = "Call of Duty 4: Modern Warfare",
    .log_directory = "COD4Compat",
    .image_file = "COD4.image",
    .callee_pops_struct_return = 1,
    .depth_capability_check = {0x0001423a, {0xf6, 0x85, 0xbd, 0xfb, 0xff, 0xff, 0x08}, 7},
    .license_log_return_address = 0x0002ad85,
    .entry_eip = 0x000116a4,
    .image_end = 0x0201b000,
    .main_address = 0x0002b0f0,
    .steam_app_id = 7940,
    .thread_argument_is_direct = 1,
    .loading_screen = &cod4_loading_screen,
};

static const struct lp32_game_profile cod4_mp_profile = {
    .title = LP32_TITLE_COD4_MP,
    .name = "COD4MP",
    .display_name = "Call of Duty 4: Modern Warfare Multiplayer",
    .log_directory = "COD4MPCompat",
    .image_file = "COD4MP.image",
    .callee_pops_struct_return = 1,
    .depth_capability_check = {0x000147fa, {0xf6, 0x85, 0xbd, 0xfb, 0xff, 0xff, 0x08}, 7},
    .server_name_compare = {0x000c80c0, {0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec}, 8},
    .license_log_return_address = 0x0002b375,
    .entry_eip = 0x00011644,
    .image_end = 0x0d7da000,
    .main_address = 0x0002b6e0,
    .steam_app_id = 7940,
    .thread_argument_is_direct = 1,
};

/* Aspyr's Steam 1.3.1 (111540) Mac release, app 2630. Both executables are
   unstripped; main and the proof-of-purchase puts are named in the symbols.
   CDisplayInfo accepts a renderer only if kCGLRPDepthModes has the 24-bit
   bit (testb $0x08 on its second byte); current Apple GPUs offer 32-bit. */
static const struct lp32_game_profile cod2_profile = {
    .title = LP32_TITLE_COD2,
    .name = "COD2",
    .display_name = "Call of Duty 2",
    .log_directory = "COD2Compat",
    .image_file = "COD2.image",
    .callee_pops_struct_return = 1,
    .depth_capability_check = {0x000050b1, {0xf6, 0x45, 0xc1, 0x08}, 4},
    .license_log_return_address = 0x0021ba27,
    .entry_eip = 0x000039c8,
    .image_end = 0x01e22000,
    .main_address = 0x0021ba70,
    .steam_app_id = 2630,
    .thread_argument_is_direct = 1,
};

static const struct lp32_game_profile cod2_mp_profile = {
    .title = LP32_TITLE_COD2_MP,
    .name = "COD2MP",
    .display_name = "Call of Duty 2 Multiplayer",
    .log_directory = "COD2MPCompat",
    .image_file = "COD2MP.image",
    .callee_pops_struct_return = 1,
    .depth_capability_check = {0x000051f1, {0xf6, 0x45, 0xc1, 0x08}, 4},
    .license_log_return_address = 0x00234987,
    .entry_eip = 0x000034f8,
    .image_end = 0x01c15000,
    .main_address = 0x002349d0,
    .steam_app_id = 2630,
    .thread_argument_is_direct = 1,
};

/* Aspyr's WaitForSingleObject shim treats any timeout below 2 ms as a poll
   (`cmp $2,%ebx; jb timeout`). MW2's render workers wait on the backend
   event with a 1 ms timeout, so they spin on the event mutex, contending
   with the render thread that signals it. Windows sleeps for the timeout;
   lowering the threshold to 1 makes it a real timed wait, and SetEvent's
   broadcast still wakes the workers the moment work arrives. */
static const struct lp32_code_patch mw2_code_patches[] = {
    /* The original spin counts make millions of cross-ABI trylock calls
       during shader loading. These routines already fall back to blocking
       pthread_mutex_lock after the spin; use that path immediately. */
    {
        .address = 0x00082ce6,
        .expected = {0xe8, 0xbb, 0xfb, 0x2d, 0x00},
        .replacement = {0xe8, 0xb5, 0xfb, 0x2d, 0x00},
        .length = 5,
        .reason = "ASLCriticalSection waits for contended locks without spinning",
    },
    {
        .address = 0x000af7d4,
        .expected = {0xe8, 0xcd, 0x30, 0x2b, 0x00},
        .replacement = {0xe8, 0xc7, 0x30, 0x2b, 0x00},
        .length = 5,
        .reason = "EnterCriticalSection waits for contended locks without spinning",
    },
    {
        .address = 0x000af431,
        .expected = {0x83, 0xfb, 0x02, 0x0f, 0x82},
        .replacement = {0x83, 0xfb, 0x01, 0x0f, 0x82},
        .length = 5,
        .reason = "WaitForSingleObject 1 ms timeout waits instead of polling",
    },
    /* Load_CreateMaterialPixelShader/VertexShader Sleep(1) after every 10 ms
       of shader creation so a single-core 2009 machine could still draw the
       loading screen. Under translation that is hundreds of sleeps per
       mission load; the render thread has its own core. */
    {
        .address = 0x0011a723,
        .expected = {0xe8, 0xda, 0xcd, 0x1a, 0x00},
        .replacement = {0x0f, 0x1f, 0x44, 0x00, 0x00},
        .length = 5,
        .reason = "pixel shader creation does not sleep between shaders",
    },
    {
        .address = 0x0011a82f,
        .expected = {0xe8, 0xce, 0xcc, 0x1a, 0x00},
        .replacement = {0x0f, 0x1f, 0x44, 0x00, 0x00},
        .length = 5,
        .reason = "vertex shader creation does not sleep between shaders",
    },
    {0},
};

/* MW2's LC_MAIN identifies the game entry. No COD4 address-based patches
   apply to either MW2 executable. The Aspyr Game Guide launcher is separate. */
static const struct lp32_game_profile mw2_profile = {
    .title = LP32_TITLE_MW2,
    .name = "MW2",
    .display_name = "Call of Duty: Modern Warfare 2",
    .log_directory = "MW2Compat",
    .image_file = "MW2.image",
    .callee_pops_struct_return = 1,
    .thread_argument_is_direct = 1,
    .steam_app_id = 10180,
    .code_patches = mw2_code_patches,
};

static const struct lp32_game_profile mw2_mp_profile = {
    .title = LP32_TITLE_MW2_MP,
    .name = "MW2MP",
    .display_name = "Call of Duty: Modern Warfare 2 Multiplayer",
    .log_directory = "MW2MPCompat",
    .image_file = "MW2MP.image",
    .callee_pops_struct_return = 1,
    .thread_argument_is_direct = 1,
    .steam_app_id = 10190,
};

/* The same Aspyr runtime changes as mw2_code_patches, at the Steam MW3
   single-player executable's addresses (same functions, same byte checks). */
static const struct lp32_code_patch mw3_code_patches[] = {
    {
        .address = 0x000894f6,
        .expected = {0xe8, 0x2d, 0x29, 0x3a, 0x00},
        .replacement = {0xe8, 0x27, 0x29, 0x3a, 0x00},
        .length = 5,
        .reason = "ASLCriticalSection waits for contended locks without spinning",
    },
    {
        .address = 0x000b6194,
        .expected = {0xe8, 0x8f, 0x5c, 0x37, 0x00},
        .replacement = {0xe8, 0x89, 0x5c, 0x37, 0x00},
        .length = 5,
        .reason = "EnterCriticalSection waits for contended locks without spinning",
    },
    {
        .address = 0x000b5df1,
        .expected = {0x83, 0xfb, 0x02, 0x0f, 0x82},
        .replacement = {0x83, 0xfb, 0x01, 0x0f, 0x82},
        .length = 5,
        .reason = "WaitForSingleObject 1 ms timeout waits instead of polling",
    },
    {
        .address = 0x002db91f,
        .expected = {0xe8, 0x02, 0xb0, 0x08, 0x00},
        .replacement = {0x0f, 0x1f, 0x44, 0x00, 0x00},
        .length = 5,
        .reason = "pixel shader creation does not sleep between shaders",
    },
    {
        .address = 0x002dba53,
        .expected = {0xe8, 0xce, 0xae, 0x08, 0x00},
        .replacement = {0x0f, 0x1f, 0x44, 0x00, 0x00},
        .length = 5,
        .reason = "vertex shader creation does not sleep between shaders",
    },
    /* RB_EndFrame sleeps 1 ms before every Present on the render thread, the
       frame's bottleneck. The swap fence still bounds frames in flight. */
    {
        .address = 0x002ffb0c,
        .expected = {0xe8, 0x15, 0x6e, 0x06, 0x00},
        .replacement = {0x0f, 0x1f, 0x44, 0x00, 0x00},
        .length = 5,
        .reason = "render thread does not sleep before each present",
    },
    /* Sys_HasInfoChanged compares a fresh CPU frequency estimate with the
       archived one. Under translation that estimate varies with load and
       thermal state, so MW3 repeatedly offers to reset graphics settings.
       Keep its RAM and GPU checks, which still identify a real hardware
       change, but ignore the two unstable CPU comparison branches. */
    {
        .address = 0x003f41fb,
        .expected = {0x77, 0x4a},
        .replacement = {0x90, 0x90},
        .length = 2,
        .reason = "CPU frequency drift does not reset graphics settings",
    },
    {
        .address = 0x003f4209,
        .expected = {0x77, 0x3c},
        .replacement = {0x90, 0x90},
        .length = 2,
        .reason = "CPU frequency drift does not reset graphics settings",
    },
    {0},
};

/* The same changes at the Steam MW3 multiplayer executable's addresses. Its
   WaitForSingleObject compares the timeout in %ecx rather than %ebx. */
static const struct lp32_code_patch mw3_mp_code_patches[] = {
    {
        .address = 0x000ad435,
        .expected = {0xe8, 0x5c, 0x51, 0x3a, 0x00},
        .replacement = {0xe8, 0x56, 0x51, 0x3a, 0x00},
        .length = 5,
        .reason = "ASLCriticalSection waits for contended locks without spinning",
    },
    {
        .address = 0x000b412f,
        .expected = {0xe8, 0x62, 0xe4, 0x39, 0x00},
        .replacement = {0xe8, 0x5c, 0xe4, 0x39, 0x00},
        .length = 5,
        .reason = "EnterCriticalSection waits for contended locks without spinning",
    },
    {
        .address = 0x000b39ab,
        .expected = {0x83, 0xf9, 0x02},
        .replacement = {0x83, 0xf9, 0x01},
        .length = 3,
        .reason = "WaitForSingleObject 1 ms timeout waits instead of polling",
    },
    {
        .address = 0x002d3fc6,
        .expected = {0xe8, 0xb6, 0xd6, 0x09, 0x00},
        .replacement = {0x0f, 0x1f, 0x44, 0x00, 0x00},
        .length = 5,
        .reason = "pixel shader creation does not sleep between shaders",
    },
    {
        .address = 0x002d40ec,
        .expected = {0xe8, 0x90, 0xd5, 0x09, 0x00},
        .replacement = {0x0f, 0x1f, 0x44, 0x00, 0x00},
        .length = 5,
        .reason = "vertex shader creation does not sleep between shaders",
    },
    {
        .address = 0x002f53da,
        .expected = {0xe8, 0xa2, 0xc2, 0x07, 0x00},
        .replacement = {0x0f, 0x1f, 0x44, 0x00, 0x00},
        .length = 5,
        .reason = "render thread does not sleep before each present",
    },
    {0},
};

/* MW3 uses MW2's Aspyr runtime. MW2's addresses do not apply to MW3. */
static const struct lp32_game_profile mw3_profile = {
    .title = LP32_TITLE_MW3,
    .name = "MW3",
    .display_name = "Call of Duty: Modern Warfare 3",
    .log_directory = "MW3Compat",
    .image_file = "MW3.image",
    .callee_pops_struct_return = 1,
    .thread_argument_is_direct = 1,
    .steam_app_id = 42680,
    .code_patches = mw3_code_patches,
};

static const struct lp32_game_profile mw3_mp_profile = {
    .title = LP32_TITLE_MW3_MP,
    .name = "MW3MP",
    .display_name = "Call of Duty: Modern Warfare 3 Multiplayer",
    .log_directory = "MW3MPCompat",
    .image_file = "MW3MP.image",
    .callee_pops_struct_return = 1,
    .thread_argument_is_direct = 1,
    .steam_app_id = 42690,
    .code_patches = mw3_mp_code_patches,
};

/* Gameloft's Gangstar: Miami Vindication (Mac App Store, 2011): a Cocoa app
   that renders fixed-function GL through its own NSOpenGLContext and takes
   input and the GL attributes through SDL 1.3's 1.2-compatible calls
   (sdl_bridge.m implements those natively instead of the bundled framework).
   llvm-gcc i386: the callee pops the hidden struct-return pointer. */
static const struct lp32_game_profile gangstar_profile = {
    .title = LP32_TITLE_GANGSTAR,
    .name = "Gangstar",
    .display_name = "Gangstar: Miami Vindication",
    .log_directory = "GangstarCompat",
    .image_file = "Gangstar.image",
    .callee_pops_struct_return = 1,
    .thread_argument_is_direct = 1,
};

/* Asphalt 6 1.0.9 uses the same Cocoa/llvm-gcc ABI, with GLSL rendering
   and native Cocoa input rather than Gangstar's SDL event pump. */
static const struct lp32_asphalt6_controller_layout asphalt6_controller = {
    .sha256 = "8ce20892c7bf360a4bc71e29b8e32976c40081558db50788e29da9a59a1a6407",
    .commit_call = {0x000bbb88, {0xe8, 0x63, 0xd0, 0xff, 0xff}, 5},
    .steering_call = {0x001e81e3, {0xe8, 0x38, 0xdc, 0xff, 0xff}, 5},
    .commit = 0x000b8bf0,
    .digital_steering = 0x001e5e20,
    .steering_limit = 0x001e5b90,
    .steering_maximum = 0x000d3ca0,
    .stack_pointer_slot = 0x00705860,
    .stack_top = 0x000c8a60,
    .race_name = 0x000c86f0,
};
static const struct lp32_game_profile asphalt6_profile = {
    .title = LP32_TITLE_ASPHALT6,
    .name = "Asphalt6",
    .display_name = "Asphalt 6: Adrenaline",
    .log_directory = "Asphalt6Compat",
    .image_file = "Asphalt6.image",
    .asphalt6_controller = &asphalt6_controller,
    .callee_pops_struct_return = 1,
    .thread_argument_is_direct = 1,
};

static const struct lp32_game_profile *const known_profiles[] = {
    &pirates_profile,
    &clone_wars_profile,
    &marvel_profile,
    &saga_profile,
    &saga_10_profile,
    &saga_steam_profile,
    &cod4_profile,
    &cod4_mp_profile,
    &cod2_profile,
    &cod2_mp_profile,
    &mw2_profile,
    &mw2_mp_profile,
    &mw3_profile,
    &mw3_mp_profile,
    &gangstar_profile,
    &asphalt6_profile,
};

static const struct lp32_game_profile *current_profile = &unknown_profile;
static struct lp32_game_profile cod_detected_profile;

/* SHA-256 of the Steam executables whose fixed addresses the profiles use. */
static bool tested_cod_image(const char *path, enum lp32_title title)
{
    const char *expected;
    switch (title) {
    case LP32_TITLE_COD4: expected = "43629c7f2f1b6f891e93c134f3101dbfb9fc4b46cabb1491e90437a450208870"; break;
    case LP32_TITLE_COD4_MP: expected = "f90ec11a0822628aac0c2f788d980967ad333405b2fec5f3ecfbca1e3d8d1374"; break;
    case LP32_TITLE_COD2: expected = "9c4592fd0bc6d2d04d82162df52cbad3b318c01fd919fbb96d6fa3399adf6a0a"; break;
    case LP32_TITLE_COD2_MP: expected = "a6aa70d4e2b0bf5f388b0653752847bddac95398ee0d860e757ec79e466f5d1b"; break;
    default: return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    CC_SHA256_CTX state;
    CC_SHA256_Init(&state);
    uint8_t buffer[65536];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        CC_SHA256_Update(&state, buffer, (CC_LONG)count);
    bool valid = !ferror(file);
    fclose(file);
    if (!valid) return false;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &state);
    char actual[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (unsigned i = 0; i < sizeof(digest); ++i)
        snprintf(actual + i * 2, 3, "%02x", digest[i]);
    return !strcmp(actual, expected);
}

/* Finds the title's unique depth-mode test in an untested executable. */
static struct lp32_code_signature cod_depth_signature(const struct macho_image32 *image,
                                                      const struct lp32_code_signature *tested)
{
    const uint8_t *pattern = tested->expected;
    const size_t pattern_size = tested->length;
    struct lp32_code_signature found = {0};
    if (!image->header) return found;
    const struct load_command *command = (const void *)(image->header + 1);
    for (uint32_t i = 0; i < image->header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT && command->cmdsize >= sizeof(struct segment_command)) {
            const struct segment_command *segment = (const void *)command;
            if ((segment->initprot & VM_PROT_EXECUTE) &&
                segment->vmaddr >= image->min_address && segment->vmaddr < image->max_address) {
                uint32_t size = segment->vmsize;
                if (size > image->max_address - segment->vmaddr)
                    size = image->max_address - segment->vmaddr;
                const uint8_t *bytes = (const void *)(uintptr_t)segment->vmaddr;
                for (uint32_t offset = 0; offset + pattern_size <= size; ++offset) {
                    if (memcmp(bytes + offset, pattern, pattern_size)) continue;
                    if (found.length) return (struct lp32_code_signature){0};
                    found.address = segment->vmaddr + offset;
                    memcpy(found.expected, pattern, pattern_size);
                    found.length = (uint8_t)pattern_size;
                }
            }
        }
        command = (const void *)((const char *)command + command->cmdsize);
    }
    return found;
}

const struct lp32_game_profile *lp32_profile(void)
{
    return current_profile;
}

const struct lp32_game_profile *lp32_profile_named(const char *name)
{
    if (!name) return NULL;
    if (!strcasecmp(name, "cod") || !strcasecmp(name, "cod4")) return &cod4_profile;
    if (!strcasecmp(name, "cod4mp") || !strcasecmp(name, "cod4-mp")) return &cod4_mp_profile;
    if (!strcasecmp(name, "cod2")) return &cod2_profile;
    if (!strcasecmp(name, "cod2mp") || !strcasecmp(name, "cod2-mp")) return &cod2_mp_profile;
    if (!strcasecmp(name, "mw2") || !strcasecmp(name, "codmw2")) return &mw2_profile;
    if (!strcasecmp(name, "mw2mp") || !strcasecmp(name, "mw2-mp")) return &mw2_mp_profile;
    if (!strcasecmp(name, "mw3") || !strcasecmp(name, "codmw3")) return &mw3_profile;
    if (!strcasecmp(name, "mw3mp") || !strcasecmp(name, "mw3-mp")) return &mw3_mp_profile;
    for (size_t index = 0; index < sizeof(known_profiles) / sizeof(known_profiles[0]); ++index) {
        if (strcasecmp(known_profiles[index]->name, name) == 0) {
            return known_profiles[index];
        }
    }
    if (!strcasecmp(name, "saga") || !strcasecmp(name, "lswc") ||
        !strcasecmp(name, "completesaga") || !strcasecmp(name, "saga-steam"))
        return &saga_steam_profile;
    if (!strcasecmp(name, "saga-retail")) return &saga_profile;
    /* Accept the friendlier spellings used on the command line. */
    if (strcasecmp(name, "marvel") == 0) return &marvel_profile;
    if (strcasecmp(name, "pirates") == 0) return &pirates_profile;
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
    if (image->main_address) return image->main_address;
    if (image->entry_eip < image->min_address || image->entry_eip >= image->max_address)
        return 0;
    uint32_t entry_bytes = image->max_address - image->entry_eip;
    if (entry_bytes > 128) entry_bytes = 128;
    /* Aspyr's COD4 crt calls main directly from start, then stores eax as
       exit's argument before a second call and hlt. */
    const uint8_t *stub = (const void *)(uintptr_t)image->entry_eip;
    for (uint32_t offset = 0; offset + 15 <= entry_bytes; ++offset) {
        if (stub[offset] != 0xe8 || stub[offset + 5] != 0x89 ||
            stub[offset + 6] != 0x44 || stub[offset + 7] != 0x24 ||
            stub[offset + 8] != 0x00 || stub[offset + 9] != 0xe8 ||
            stub[offset + 14] != 0xf4) continue;
        uint32_t main = call_target(image, image->entry_eip + offset);
        if (main) return main;
    }
    /* The 10.6 SDK's crt1 (Gangstar) also calls main from start, with the
       shorter `mov %eax,(%esp)` encoding: e8 main; 89 04 24; e8 exit; hlt. */
    for (uint32_t offset = 0; offset + 14 <= entry_bytes; ++offset) {
        if (stub[offset] != 0xe8 || stub[offset + 5] != 0x89 ||
            stub[offset + 6] != 0x04 || stub[offset + 7] != 0x24 ||
            stub[offset + 8] != 0xe8 || stub[offset + 13] != 0xf4) continue;
        uint32_t main = call_target(image, image->entry_eip + offset);
        if (main) return main;
    }
    /*
     * crt1's `start` stub ends with `call __start; hlt`.  The C-level
     * `__start` asks dyld for its initializer/terminator hooks through the
     * __dyld section (which this loader does not populate: initializers are
     * run explicitly), then ends with `call main; mov %eax,(%esp); call exit`.
     * The LEGO titles using this crt put main before that exit sequence.
     * Find the call instead of executing crt startup.
     */
    uint32_t crt_start = 0;
    for (uint32_t offset = 0; offset + 6 <= entry_bytes && offset < 96; ++offset) {
        if (stub[offset] != 0xe8 || stub[offset + 5] != 0xf4) continue;
        crt_start = call_target(image, image->entry_eip + offset);
        if (crt_start) break;
    }
    if (!crt_start) return 0;

    const uint8_t *code = (const void *)(uintptr_t)crt_start;
    uint32_t crt_bytes = image->max_address - crt_start;
    if (crt_bytes > 0x200) crt_bytes = 0x200;
    for (uint32_t offset = 0; offset + 9 <= crt_bytes; ++offset) {
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

int lp32_profile_select(const struct macho_image32 *image, const char *image_path)
{
    const struct lp32_game_profile *selected = NULL;
    const char *override = getenv("LP32_GAME");
    if (override && override[0]) {
        selected = lp32_profile_named(override);
        if (!selected) {
            fprintf(stderr, "game_loader: unknown LP32_GAME profile: %s\n", override);
            return -1;
        }
    } else {
        for (size_t index = 0; index < sizeof(known_profiles) / sizeof(known_profiles[0]); ++index) {
            const struct lp32_game_profile *candidate = known_profiles[index];
            if (candidate->entry_eip && candidate->entry_eip == image->entry_eip &&
                candidate->image_end == image->max_address) {
                selected = candidate;
                break;
            }
        }
    }
    if (selected && image_path && lp32_title_is_cod_carbon(selected->title) &&
        (!tested_cod_image(image_path, selected->title) ||
         getenv(selected->title == LP32_TITLE_COD2 || selected->title == LP32_TITLE_COD2_MP ?
                "LP32_COD2_GENERIC" : "LP32_COD4_GENERIC")))
        selected = NULL;
    if (!selected && image_path) {
        const char *name = strrchr(image_path, '/');
        name = name ? name + 1 : image_path;
        if (!strcmp(name, "MW2.image")) selected = &mw2_profile;
        if (!strcmp(name, "MW2MP.image")) selected = &mw2_mp_profile;
        if (!strcmp(name, "MW3.image")) selected = &mw3_profile;
        if (!strcmp(name, "MW3MP.image")) selected = &mw3_mp_profile;
        if (!strcmp(name, "Gangstar.image")) selected = &gangstar_profile;
        if (!strcmp(name, "Asphalt6.image")) selected = &asphalt6_profile;
        if (!strcmp(name, "COD4.image") || !strcmp(name, "COD4MP.image")) {
            /* Other Mac releases may place functions at different addresses.
               Retain title-level ABI handling, but never apply the tested
               Steam image's address-based patches to an unknown binary. */
            cod_detected_profile = !strcmp(name, "COD4.image") ? cod4_profile : cod4_mp_profile;
            cod_detected_profile.entry_eip = image->entry_eip;
            cod_detected_profile.image_end = image->max_address;
            cod_detected_profile.main_address = 0;
            cod_detected_profile.depth_capability_check =
                cod_depth_signature(image, &cod_detected_profile.depth_capability_check);
            cod_detected_profile.server_name_compare = (struct lp32_code_signature){0};
            cod_detected_profile.license_log_return_address = 0;
            cod_detected_profile.loading_screen = NULL;
            selected = &cod_detected_profile;
            fprintf(stderr, "compat32: using generic COD4 profile for an untested image\n");
        }
        if (!strcmp(name, "COD2.image") || !strcmp(name, "COD2MP.image")) {
            /* As for COD4: keep the engine-family handling, drop every
               address tied to the tested Steam executables. */
            cod_detected_profile = !strcmp(name, "COD2.image") ? cod2_profile : cod2_mp_profile;
            cod_detected_profile.entry_eip = image->entry_eip;
            cod_detected_profile.image_end = image->max_address;
            cod_detected_profile.main_address = 0;
            cod_detected_profile.depth_capability_check =
                cod_depth_signature(image, &cod_detected_profile.depth_capability_check);
            cod_detected_profile.license_log_return_address = 0;
            selected = &cod_detected_profile;
            fprintf(stderr, "compat32: using generic COD2 profile for an untested image\n");
        }
    }
    if (!selected) {
        fprintf(stderr,
                "game_loader: unrecognised image (entry=0x%08x end=0x%08x); "
                "set LP32_GAME to force a profile\n",
                image->entry_eip, image->max_address);
        return -1;
    }
    if (lp32_title_is_cod_carbon(selected->title)) {
        bool has_steam = false;
        for (unsigned i = 0; i < image->import_count; ++i) {
            const char *name = image->imports[i].name;
            if (name && (!strncmp(name, "_Steam", 6) || !strncmp(name, "Steam", 5))) {
                has_steam = true;
                break;
            }
        }
        if (!has_steam) {
            cod_detected_profile = *selected;
            cod_detected_profile.steam_app_id = 0;
            selected = &cod_detected_profile;
        }
    }
    current_profile = selected;
    fprintf(stderr, "compat32: title profile %s (%s)\n", selected->name,
            selected->display_name);
    return 0;
}
