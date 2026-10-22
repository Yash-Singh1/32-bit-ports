#ifndef LP32_ASPHALT6_INPUT_H
#define LP32_ASPHALT6_INPUT_H

#include <stdbool.h>
#include <stdint.h>

enum a6_action {
    A6_UP, A6_DOWN, A6_LEFT, A6_RIGHT, A6_NITRO, A6_BACK,
    A6_CAMERA1, A6_CAMERA2, A6_CAMERA3, A6_CLICK, A6_ACTION_COUNT
};
enum a6_button {
    A6_A = 1 << 0, A6_B = 1 << 1, A6_X = 1 << 2, A6_Y = 1 << 3,
    A6_LB = 1 << 4, A6_RB = 1 << 5, A6_MENU = 1 << 6,
    A6_OPTIONS = 1 << 7, A6_RT = 1 << 8, A6_LT = 1 << 9,
    A6_DLEFT = 1 << 10, A6_DRIGHT = 1 << 11,
    A6_DUP = 1 << 12, A6_DDOWN = 1 << 13
};
enum { A6_EDGE_CAPACITY = 32 };
struct a6_edges { uint8_t values[A6_EDGE_CAPACITY], head, count; };
struct a6_command { uint8_t action; bool down; };
struct a6_input {
    uint32_t keyboard, pad, desired, reported, buttons;
    struct a6_edges edges[A6_ACTION_COUNT];
    struct a6_command commands[A6_EDGE_CAPACITY];
    unsigned command_head, command_count;
    float steering;
    unsigned camera;
    bool racing;
};
typedef void (*a6_emit_fn)(void *context, enum a6_action action, bool down);

/* Pure input mapper. One edge per action per commit preserves short taps. */
void a6_input_key(struct a6_input *input, enum a6_action action, bool down);
void a6_input_mode(struct a6_input *input, bool racing);
void a6_input_sample(struct a6_input *input, uint32_t buttons, float stick_x);
void a6_input_release_pad(struct a6_input *input);
void a6_input_release_all(struct a6_input *input);
void a6_input_commit(struct a6_input *input, a6_emit_fn emit, void *context);
float a6_axis(float value, float dead_zone);
void a6_cursor_delta(float x, float y, double seconds, float *dx, float *dy);
/* Keep the normalized car angle and the speed-limited return value distinct. */
bool a6_steering_angles(float axis, float limit, float maximum,
                        float *car_angle, float *result);

#endif
