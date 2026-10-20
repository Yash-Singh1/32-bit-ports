#ifndef LP32_SDL_BRIDGE_H
#define LP32_SDL_BRIDGE_H

#include <stdint.h>

/*
 * Native implementation of the SDL 1.3 (1.2-compatible) calls a title imports
 * from a bundled guest SDL.framework that the loader does not map (its Cocoa
 * classes would be guest Objective-C inside a guest dylib).  Joysticks are
 * GameController pads; video/event calls follow SDL's documented results for
 * a title that owns its own Cocoa window and event loop.
 */
int sdl_bridge32_dispatch(const char *name, const uint32_t *arguments, uint64_t *result);

#endif
