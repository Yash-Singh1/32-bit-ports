#ifndef LP32_ASPHALT6_CONTROLLER_H
#define LP32_ASPHALT6_CONTROLLER_H
#include <stdint.h>

#define kAsphalt6CommitCallback "_lp32_asphalt6_input_commit"
#define kAsphalt6SteeringCallback "_lp32_asphalt6_steering"
int asphalt6_controller32_install(const char *image_path);
void asphalt6_controller32_poll(void);
/* Called by the legacy Cocoa forwarder. Returns 1 for buffered input. */
int asphalt6_controller32_event(void *view, const char *selector, void *event);
void asphalt6_controller32_focus_lost(void);
uint64_t asphalt6_controller32_commit(const uint32_t *arguments);
float asphalt6_controller32_steer(const uint32_t *arguments);
int asphalt6_controller32_float_self_test(void);
#endif
