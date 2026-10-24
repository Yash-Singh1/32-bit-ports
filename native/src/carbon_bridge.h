#ifndef LP32_CARBON_BRIDGE_H
#define LP32_CARBON_BRIDGE_H
#include <stdint.h>
int carbon_bridge32_path_from_spec(uint32_t spec, char *path, uint32_t capacity);
int carbon_bridge32_region_bounds(uint32_t region, void *rect);
int carbon_bridge32_has_symbol(const char *name);
int carbon_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);
int carbon_bridge32_run_geometry_self_test(void);
int carbon_bridge32_run_alert_self_test(void);
int carbon_bridge32_run_dispatch_self_test(void);
int carbon_bridge32_run_file_self_test(void);
#endif
