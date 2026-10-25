#ifndef LP32_CARBON_UI_H
#define LP32_CARBON_UI_H
#include <stdint.h>
/* Returns a retained CFBundle corresponding to the bridge's NSBundle object. */
void *carbon_ui_copy_bundle(void *bundle);
void carbon_ui_paint(void *window, void *image);
void carbon_ui_geometry_changed(void *window);
int carbon_ui_bind_gl(void *agl, void *cgl, void *window);
int carbon_ui_update_gl(void *agl);
int carbon_ui_swap_gl(void *agl);
#ifdef __OBJC__
@class NSOpenGLContext;
void carbon_ui_capture_gl_frame(NSOpenGLContext *context, uint64_t swaps);
#endif
void carbon_ui_release_gl(void *agl);
void carbon_ui_fullscreen(void *window, uint32_t display, int32_t width, int32_t height);
void carbon_ui_show(void *window);
void carbon_ui_hide(void *window);
int carbon_ui_select(void *window);
void carbon_ui_sync_focus(void);
void carbon_ui_set_menu_bar_visible(int visible);
void carbon_ui_dispose(void *window);
int32_t carbon_ui_run_modal(void *window);
int carbon_ui_stop_modal(void *window);
int carbon_ui_is_visible(void *window);
int carbon_ui_is_active(void *window);
int carbon_ui_convert_game_point(void *window, int16_t *point, int to_local);
void carbon_ui_use_native(void *window);
void carbon_ui_prepare_standard_alert(void *dialog, uint32_t type,
    const void *title, const void *message, int16_t default_item, int16_t cancel_item);
int32_t carbon_ui_run_standard_alert(void *dialog, int16_t *item);
int carbon_ui_test_click_standard_alert(void *dialog, int16_t item);
int32_t carbon_ui_show_sheet(void *sheet, void *parent);
int32_t carbon_ui_hide_sheet(void *sheet);
#endif
