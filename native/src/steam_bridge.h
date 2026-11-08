#ifndef LP32_STEAM_BRIDGE_H
#define LP32_STEAM_BRIDGE_H
#include <stdint.h>
#define LP32_STEAM_CLIENT_HANDLE UINT32_C(0xffc00001)
uint32_t steam_bridge32_wrap_export(const char *name, uint32_t address);
uint32_t steam_bridge32_open(const char *path);
uint32_t steam_bridge32_symbol(const char *name);
int steam_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result);
int steam_bridge32_stret(const char *name);
#endif
