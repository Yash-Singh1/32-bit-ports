#ifndef LP32_CONTROLLER_BRIDGE_H
#define LP32_CONTROLLER_BRIDGE_H

#include <stdint.h>

/* Install the title's controller backend after image/import initialization.
   Asphalt checks image_path before applying its version-specific hooks. */
int controller_bridge32_install(const char *image_path);

/* Refresh connected devices and their current element values once per frame. */
void controller_bridge32_poll(uint64_t swap_count);

/* Deterministic, windowless validation used when no physical pads are present. */
int controller_bridge32_run_self_test(void);

/* The game's control-prompt formatter asks a registered provider for the text
   of a pad button by raw HID index; the bridge registers a guest thunk under
   this import name and answers with the title's own button glyphs. */
#define kControllerGlyphCallbackName "_lp32_button_glyph"
void controller_bridge32_button_glyph(uint32_t raw_index, char *out);

#endif
