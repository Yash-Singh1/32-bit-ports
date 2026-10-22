#ifndef LP32_OBJC_BRIDGE_H
#define LP32_OBJC_BRIDGE_H

#include "compat_runtime.h"

#include <stdint.h>

uint32_t objc_bridge32_guest_selector(const char *name);
void *objc_bridge32_host_object(uint32_t token);
uint32_t objc_bridge32_guest_pointer(void *pointer);
uint32_t objc_bridge32_guest_object(void *object);
uint32_t objc_bridge32_guest_owned_object(void *object);

int objc_bridge32_dispatch(const char *import_name, const uint32_t *arguments,
                           uint64_t *result);
/* Direct handler for a hot OpenGL import, or NULL to keep using the chain. */
lp32_fast_import_fn objc_bridge32_fast_import(const char *import_name);
uint32_t objc_bridge32_pointer_import(const char *import_name);
int objc_bridge32_run_proxy_self_test(void);
int objc_bridge32_run_display_self_test(void);
int objc_bridge32_run_pointer_self_test(void);
int objc_bridge32_run_dispatch_fast_self_test(void);
int objc_bridge32_run_event_monitor_self_test(void);
int objc_bridge32_run_app_event_delivery_self_test(void);
int objc_bridge32_run_focus_self_test(int expected);
int objc_bridge32_run_date_formatter_self_test(void);
void objc_bridge32_initialize_test_mode(void);
void objc_bridge32_capture_launch_display(void);
int objc_bridge32_prepare_shader_cache(void);
float objc_bridge32_preferred_display_aspect_ratio(void);
struct lp32_screen_aspect_patch;
int objc_bridge32_run_panel_scale_self_test(const struct lp32_screen_aspect_patch *patch);
int objc_bridge32_run_gl_parameter_self_test(void);
int objc_bridge32_run_gl_buffer_self_test(void);
int objc_bridge32_run_gl_texture_self_test(void);

void objc_bridge32_pin_event(uint32_t, int);
int objc_bridge32_test_key_down(unsigned key);
uint32_t objc_bridge32_borrowed_bytes(void *owner,const void *bytes,size_t length,unsigned slot);
void objc_bridge32_commit_borrowed_bytes(void *owner,void *bytes,size_t length,unsigned slot);
void objc_bridge32_release_borrowed_bytes(void *owner,unsigned slot);
uint32_t objc_bridge32_borrowed_object(void *owner,void *object,unsigned slot);
#endif
