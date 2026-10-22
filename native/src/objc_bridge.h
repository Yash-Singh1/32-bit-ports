#ifndef LP32_OBJC_BRIDGE_H
#define LP32_OBJC_BRIDGE_H

#include "compat_runtime.h"

#include <stdint.h>

uint32_t objc_bridge32_guest_selector(const char *name);
void *objc_bridge32_host_object(uint32_t token);
/* MW2 dynamic index buffer copies; see objc_bridge.m. */
int objc_bridge32_index_shadow_acquire(uint32_t offset, int32_t count, uint32_t type,
                                       const void **pointer, uint32_t *buffer);
void objc_bridge32_index_shadow_release(uint32_t buffer);
/* Keep SHADOW texture targets on depth-compare samplers. Colour targets that
   the engine labels SHADOW are rewritten to plain targets before the draw. */
void objc_bridge32_reconcile_fragment_shadows(void);
void objc_bridge32_display_size(uint32_t display, int32_t size[2]);
uint32_t objc_bridge32_guest_pointer(void *pointer);
uint32_t objc_bridge32_guest_object(void *object);
uint32_t objc_bridge32_guest_owned_object(void *object);
/* Bridge token that stays valid for the next several thousand such tokens. */
uint32_t objc_bridge32_guest_recent_object(void *object);
/* Fragile-ABI exception support: isKindOfClass: on guest tokens, and a
   class name for diagnostics. */
uint32_t objc_bridge32_guest_is_kind_of_class(uint32_t exception, uint32_t cls);
const char *objc_bridge32_guest_class_name(uint32_t object);

int objc_bridge32_dispatch(const char *import_name, const uint32_t *arguments,
                           uint64_t *result);
/* Direct handler for a hot OpenGL import, or NULL to keep using the chain. */
lp32_fast_import_fn objc_bridge32_fast_import(const char *import_name);
uint32_t objc_bridge32_pointer_import(const char *import_name);
int objc_bridge32_run_proxy_self_test(void);
int objc_bridge32_run_bundle_self_test(void);
int objc_bridge32_run_pointer_self_test(void);
int objc_bridge32_run_gamma_self_test(void);
int objc_bridge32_run_focus_self_test(int expected);
int objc_bridge32_prepare_shader_cache(void);
int objc_bridge32_run_gl_parameter_self_test(void);
int objc_bridge32_run_gl_buffer_self_test(void);
int objc_bridge32_run_gl_texture_self_test(void);

void objc_bridge32_pin_event(uint32_t, int);
int objc_bridge32_test_key_down(unsigned key);
void objc_bridge32_install_test_input(void *window);
void objc_bridge32_note_mouse_event(void *event);

void objc_bridge32_note_agl_frame(void);
void objc_bridge32_carbon_focus_changed(int active);
int objc_bridge32_run_carbon_input_self_test(void);

/* Keep Carbon polling consistent with mouse events delivered to the app. */
void objc_bridge32_record_mouse_position(int16_t horizontal, int16_t vertical);
#endif
