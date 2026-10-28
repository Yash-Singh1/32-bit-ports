#ifndef LP32_ASPHALT_SCENE_H
#define LP32_ASPHALT_SCENE_H
#include "compat_runtime.h"
lp32_fast_import_fn asphalt_scene_fast_import(const char *name);
void asphalt_scene_before_draw(void);
void asphalt_scene_resolve_texture(unsigned texture);
void asphalt_scene_delete_textures(int count, const unsigned *textures);
void asphalt_scene_forget_context(void *context);
#endif
