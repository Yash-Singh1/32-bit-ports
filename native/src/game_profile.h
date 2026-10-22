#ifndef LP32_GAME_PROFILE_H
#define LP32_GAME_PROFILE_H

/*
 * Per-title knowledge.  Everything the compatibility layer has to know about
 * a specific recovered game image (guest addresses of hooks and data slots,
 * product names, bundle layout) lives here, so the same loader binary can
 * serve every TT Games title built on this engine, whichever studio (Feral,
 * TransGaming) did the Mac port.  Fields set to
 * zero/NULL mean "this title does not need or does not have that patch".
 */

#include <stddef.h>
#include <stdint.h>

struct macho_image32;

enum lp32_title {
    LP32_TITLE_UNKNOWN = 0,
    LP32_TITLE_PIRATES,
    LP32_TITLE_CLONE_WARS,
    LP32_TITLE_MARVEL,
    LP32_TITLE_COMPLETE_SAGA,
    LP32_TITLE_COD4,
    LP32_TITLE_COD4_MP,
    LP32_TITLE_MW2,
    LP32_TITLE_MW2_MP,
    LP32_TITLE_MW3,
    LP32_TITLE_MW3_MP,
    LP32_TITLE_COD2,
    LP32_TITLE_COD2_MP,
    LP32_TITLE_GANGSTAR,
    LP32_TITLE_ASPHALT6,
};

/* Gameloft's Cocoa ports use EAGLView and Apple's i386 libstdc++ ABI. */
static inline int lp32_title_is_gameloft(enum lp32_title title)
{
    return title == LP32_TITLE_GANGSTAR || title == LP32_TITLE_ASPHALT6;
}

/* Aspyr's Carbon/AGL ports of COD2 and COD4 share one Mac engine layer
   (CMacGameEngine, the Direct3D-on-OpenGL wrapper, a Carbon event loop with
   AGL fullscreen display ports, ARB programs and the Game Guide launcher). */
static inline int lp32_title_is_cod_carbon(enum lp32_title title)
{
    return title == LP32_TITLE_COD4 || title == LP32_TITLE_COD4_MP ||
           title == LP32_TITLE_COD2 || title == LP32_TITLE_COD2_MP;
}

/* Aspyr's SDL-based ports of MW2 and MW3 share one engine family and
   runtime (Miles/Bink guest libraries, core-profile GL, SDL event pump). */
static inline int lp32_title_is_mw_sdl(enum lp32_title title)
{
    return title == LP32_TITLE_MW2 || title == LP32_TITLE_MW2_MP ||
           title == LP32_TITLE_MW3 || title == LP32_TITLE_MW3_MP;
}

/* Splash-dismiss repeat latch (see game_loader.c). */
struct lp32_startup_latch_patch {
    uint32_t hook;
    uint32_t state_true;
    uint32_t original_frontend;
    uint32_t function_epilogue;
    uint32_t state_pointer;
};

/* A short code/data signature checked before a guest patch is applied. */
struct lp32_code_signature {
    uint32_t address;
    uint8_t expected[8];
    uint8_t length;
};

/* A verified in-place replacement of guest code bytes. */
struct lp32_code_patch {
    uint32_t address;
    uint8_t expected[8];
    uint8_t replacement[8];
    uint8_t length;
    const char *reason;
};

/* Asphalt 6's Cocoa input and digital-steering call sites, guarded by the
   original executable's SHA-256 as well as mapped instruction signatures. */
struct lp32_asphalt6_controller_layout {
    const char *sha256;
    struct lp32_code_signature commit_call, steering_call;
    uint32_t commit, digital_steering, steering_limit, steering_maximum;
    uint32_t stack_pointer_slot, stack_top, race_name;
};

/* Apple HID Utilities globals/functions and the input manager layout that
   controller_bridge.m recreates and hooks. */
struct lp32_controller_layout {
    uint32_t hid_device_list;         /* gpDeviceList */
    uint32_t hid_last_error;
    uint32_t input_manager_pointer;   /* static pointer to the input manager */
    uint32_t manager_records_offset;  /* device records inside the manager */
    uint32_t record_stride;
    uint32_t record_actions_offset;   /* {type, flags, int16 raw} entries */
    struct lp32_code_signature hid_get_element_value;
    struct lp32_code_signature hid_build_device_list;
    uint32_t hid_get_first_device;    /* self-test only */
    uint32_t hid_get_next_device;     /* self-test only */
    uint32_t convert_hid_state;       /* self-test only */
    uint32_t register_hid_device;     /* self-test: game input-slot allocation */
    uint32_t read_hid_state;          /* self-test: per-slot input conversion */
    /* Optional: allocator checks of the temporary record+0x2e flag. Replace
       with checks of the live record+4 device pointer (Clone Wars only). */
    struct lp32_code_signature hid_slot_occupancy_checks[7];
    /* Button prompt glyphs (0 = leave the title's "Button N" text). */
    uint32_t glyph_provider_slot;
    struct lp32_code_signature control_name_slot_check;
    uint32_t xinput_axis_name_table;
};

/*
 * Button-prompt glyph font.  The titles ship one glyph font per pad family
 * (framework-data\gui\Font\buttons_<360|PS3|PC|...>_nxg.ft2, identical
 * code points U+0531..) but the PC/Mac build hardwires the PC (Xbox) one in
 * two places: a literal "PC" suffix, and the GUI platform id (index into the
 * title's platform-name table) the per-style copies are named from.
 * controller_bridge.m retargets both to PS3 while a PlayStation pad is the
 * active controller, until the font has been loaded.
 */
struct lp32_button_font_layout {
    struct lp32_code_signature suffix_lea;   /* lea eax,[ebx+disp32] -> "PC" */
    uint32_t pc_suffix_string;               /* guest address of "PC" */
    uint32_t ps3_suffix_string;              /* guest address of "PS3" */
    struct lp32_code_signature platform_load; /* mov eax,[ebx+disp32] (6 bytes) */
    uint32_t ps3_platform_id;                /* platform-table index of "PS3" */
    uint32_t loaded_font_slot;               /* dword, non-zero once loaded */
};

/*
 * Save-worker de-duplication (see game_loader.c).  The save system init
 * allocates a worker object, publishes it in a global and starts a "SaveCore"
 * thread that drains a 4-entry lock-free job ring, polling the worker's run
 * flag through the global.  Pirates calls that init from two threads (the
 * BgProc boot sequence and the main-thread game init), so two SaveCore
 * threads consume a single-consumer ring; when both pop the same job the read
 * index overtakes the write index and the next pop dispatches a never-written
 * slot (type -1 -> call through NULL).  The loader lets the first thread
 * spawn win and makes the constructor publish the run flag already set so the
 * surviving thread does not exit when the second init swaps the worker object.
 * The latch must sit on the bare spawn call, not on the worker's start method:
 * that method also runs the initialisers every published worker needs (save
 * buffer etc.), and the surviving thread serves whichever object is current.
 */
struct lp32_save_worker_patch {
    struct lp32_code_signature ctor_running_flag; /* mov byte [reg+d8], 0 */
    uint32_t start_call;        /* call <thread starter> in the start method */
    uint32_t thread_starter;    /* target of that call (spawns the thread) */
};

/*
 * Texture-stage binder NULL guard (see game_loader.c).  The renderer's stage
 * binder re-reads the record currently bound to a sampler slot and tests its
 * +0x14 field before rebinding.  When a layered material's shader failed to
 * load, the material's begin-draw path falls back to a flat-colour program
 * without binding the layer samplers, but the stage binder still follows the
 * material type byte and dereferences the empty slot.  The guard treats an
 * empty slot like a record whose field is zero: the stage is skipped for this
 * pass.  Safety net only: the shader failure that exposed it (the ARB math
 * guard reading back write-only result registers in SM2 vertex programs) is
 * fixed in arb_program_guard.c.
 */
struct lp32_texture_bind_guard {
    struct lp32_code_signature load; /* mov ecx, [eax+14h]; test ecx, ecx */
    uint32_t resume;                 /* the jnz that follows the test */
};

/* Cooperative redraws during COD4 SP's synchronous mission initialization.
   Both entry points are cdecl, with only integer/pointer stack arguments.
   The timer call is a completed script-loop boundary, outside engine locks. */
struct lp32_loading_screen_patch {
    uint32_t client_state_pointer; /* cls indirection; connection state at +12 */
    struct lp32_code_signature redraw;
    struct lp32_code_signature is_main_thread;
    struct lp32_code_signature milliseconds;
    struct lp32_code_signature entry_points[2]; /* script load, entity parse */
    uint32_t vm_loop_timer_call;
};

/* Marvel can keep running after Steam startup requested a relaunch. Its
   achievement submitter, unlike its stats polling code, assumes stats exist. */
struct lp32_steam_achievement_guard {
    uint32_t entry;
    uint32_t stats_pointer;
    struct lp32_code_signature enabled_check;
    struct lp32_code_signature stats_load;
    struct lp32_code_signature skip_return;
};

/* Display/frontend globals consulted by objc_bridge.m. */
struct lp32_display_layout {
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t refresh_rate;
    uint32_t renderer_display_slot;
    uint32_t legacy_renderer_display;
    uint32_t frontend_state_pointer;
    uint32_t frontend_page_slot;
    uint32_t frontend_gate_slot;
    uint32_t frontend_active_word;
};

/* Fixed-size render object pool replenished by compat_runtime.c. */
struct lp32_render_pool {
    uint32_t free_count;
    uint32_t free_head;
    uint32_t mutex;
    uint32_t return_address;
    uint32_t object_size;
    uint32_t object_count;
};

/* SecuROM activation window delegate implemented in the guest. */
struct lp32_activator_layout {
    uint32_t app_delegate_isa;
    uint32_t awake_from_nib;
    uint32_t did_finish_launching;
    uint32_t text_did_change;
    uint32_t cancel;
    uint32_t activate_manually;
    uint32_t activate_online;
};

struct lp32_game_profile {
    enum lp32_title title;
    const char *name;               /* short identifier, e.g. "LEGOPirates" */
    const char *display_name;
    const char *log_directory;      /* under ~/Library/Logs */
    const char *image_file;         /* Contents/SharedSupport/<image_file> */
    uint32_t entry_eip;             /* detection key */
    uint32_t image_end;             /* detection key (max_address) */
    uint8_t thread_argument_is_direct; /* persistent task object, not a stack-local pointer */
    uint8_t callee_pops_struct_return; /* Clang i386 sret ABI (Marvel) */
    uint32_t main_address;          /* 0 = derive from the crt start stub */
    uint32_t steam_app_id;          /* relocated Steam bundle identity, 0 = none */
    struct lp32_code_signature depth_capability_check;
    struct lp32_code_signature server_name_compare; /* ctype regression only */
    uint32_t license_log_return_address; /* omit the original SDK key's puts */
    const struct lp32_startup_latch_patch *startup_latch;
    const struct lp32_controller_layout *controller;
    const struct lp32_asphalt6_controller_layout *asphalt6_controller;
    const struct lp32_button_font_layout *button_font; /* NULL = Xbox glyphs only */
    const struct lp32_save_worker_patch *save_worker;  /* NULL = single init */
    const struct lp32_texture_bind_guard *texture_bind_guard; /* NULL = none */
    const struct lp32_loading_screen_patch *loading_screen; /* NULL = none */
    const struct lp32_code_patch *code_patches; /* terminated by address 0 */
    const struct lp32_steam_achievement_guard *steam_achievement_guard;
    const struct lp32_display_layout *display;
    const struct lp32_render_pool *render_pool;
    const struct lp32_activator_layout *activator;
    uint32_t application_should_terminate; /* NuMacApplicationDelegate */
    uint32_t application_will_unhide;
    /* NuSound streamer render callback (diagnostics only: lets the audio
       bridge read the stream's refill-request ring). 0 = unknown. */
    uint32_t stream_input_callback;
    uint8_t prefers_lp32gl;         /* default to LP32GL over Apple's OpenGL (gl_backend.h) */
};

/* Chooses the profile for a loaded image (LP32_GAME overrides detection).
   Returns -1 when the image is not a known title. */
int lp32_profile_select(const struct macho_image32 *image, const char *image_path);

/* Always non-NULL after lp32_profile_select; before that a neutral profile
   with no patches is returned. */
const struct lp32_game_profile *lp32_profile(void);

/* Profile used when a bundle name is all we have (Makefile/bundle tooling). */
const struct lp32_game_profile *lp32_profile_named(const char *name);

/* Finds `main` from the crt `start` stub (the call immediately before hlt). */
uint32_t lp32_profile_main_address(const struct macho_image32 *image);

#endif
