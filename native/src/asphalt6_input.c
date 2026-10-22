#include "asphalt6_input.h"
#include <math.h>
#include <string.h>

static bool command_action(unsigned action)
{
    return action >= A6_BACK && action <= A6_CAMERA3;
}

static void queue_command(struct a6_input *input, unsigned action, bool down)
{
    if (input->command_count == A6_EDGE_CAPACITY) {
        input->command_head = input->command_count = 0;
        for (unsigned i = A6_BACK; i <= A6_CAMERA3; ++i) {
            bool desired = !!(input->desired & (1u << i));
            if (desired != !!(input->reported & (1u << i)))
                input->commands[input->command_count++] = (struct a6_command){i, desired};
        }
        return;
    }
    input->commands[(input->command_head + input->command_count++) % A6_EDGE_CAPACITY] =
        (struct a6_command){action, down};
}

float a6_axis(float value, float dead_zone)
{
    if (!isfinite(value) || fabsf(value) <= dead_zone) return 0;
    float magnitude = fminf(fabsf(value), 1);
    return copysignf((magnitude - dead_zone) / (1 - dead_zone), value);
}

static void set_desired(struct a6_input *input, uint32_t desired)
{
    uint32_t changed = input->desired ^ desired;
    input->desired = desired;
    for (unsigned i = 0; i < A6_ACTION_COUNT; ++i) {
        if (!(changed & (1u << i))) continue;
        if (command_action(i)) {
            queue_command(input, i, !!(desired & (1u << i)));
            continue;
        }
        struct a6_edges *queue = &input->edges[i];
        if (queue->count == A6_EDGE_CAPACITY) {
            /* Drop excess history, then converge to the actual source state.
               Never let an overflowing queue strand a held action. */
            queue->head = queue->count = 0;
            if (!!(input->reported & (1u << i)) == !!(desired & (1u << i)))
                continue;
        }
        queue->values[(queue->head + queue->count++) % A6_EDGE_CAPACITY] =
            !!(desired & (1u << i));
    }
}

void a6_input_key(struct a6_input *input, enum a6_action action, bool down)
{
    if ((unsigned)action >= A6_ACTION_COUNT) return;
    uint32_t bit = 1u << action;
    input->keyboard = down ? input->keyboard | bit : input->keyboard & ~bit;
    set_desired(input, input->keyboard | input->pad);
}

static void cancel_pad_actions(struct a6_input *input, uint32_t mask)
{
    input->pad &= ~mask;
    input->desired = input->keyboard | input->pad;
    struct a6_command retained[A6_EDGE_CAPACITY];
    unsigned count = 0;
    for (unsigned i = 0; i < input->command_count; ++i) {
        struct a6_command command = input->commands[(input->command_head + i) % A6_EDGE_CAPACITY];
        if (!(mask & (1u << command.action))) retained[count++] = command;
    }
    memcpy(input->commands, retained, count * sizeof(*retained));
    input->command_head = 0;
    input->command_count = count;
    for (unsigned i = 0; i < A6_ACTION_COUNT; ++i) {
        uint32_t bit = 1u << i;
        if (!(mask & bit)) continue;
        if (command_action(i)) {
            bool down = !!(input->desired & bit);
            if (down != !!(input->reported & bit)) queue_command(input, i, down);
            continue;
        }
        struct a6_edges *queue = &input->edges[i];
        queue->head = queue->count = 0;
        bool down = !!(input->desired & bit);
        if (down != !!(input->reported & bit)) {
            queue->values[0] = down;
            queue->count = 1;
        }
    }
}

void a6_input_release_pad(struct a6_input *input)
{
    input->buttons = 0;
    input->steering = 0;
    cancel_pad_actions(input, (1u << A6_ACTION_COUNT) - 1);
}

void a6_input_release_all(struct a6_input *input)
{
    input->keyboard = 0;
    a6_input_release_pad(input);
}

void a6_input_mode(struct a6_input *input, bool racing)
{
    if (input->racing == racing) return;
    input->racing = racing;
    input->steering = 0;
    cancel_pad_actions(input, (1u << A6_UP) | (1u << A6_DOWN) |
        (1u << A6_LEFT) | (1u << A6_RIGHT) | (1u << A6_NITRO) |
        (1u << A6_CAMERA1) | (1u << A6_CAMERA2) | (1u << A6_CAMERA3));
}

static void camera_tap(struct a6_input *input, int direction)
{
    input->camera = (input->camera + (direction > 0 ? 1 : 2)) % 3;
    uint32_t bit = 1u << (A6_CAMERA1 + input->camera);
    set_desired(input, input->keyboard | input->pad | bit);
    set_desired(input, input->keyboard | input->pad);
}

void a6_input_sample(struct a6_input *input, uint32_t buttons, float stick_x)
{
    uint32_t rising = buttons & ~input->buttons;
    input->buttons = buttons;
    input->steering = input->racing ? a6_axis(stick_x, .18f) : 0;
    uint32_t pad = 0;
    if (buttons & A6_A) pad |= 1u << A6_CLICK;
    if (buttons & (A6_B | A6_MENU | A6_OPTIONS)) pad |= 1u << A6_BACK;
    if (input->racing) {
        if (buttons & A6_RT) pad |= 1u << A6_UP;
        if (buttons & A6_LT) pad |= 1u << A6_DOWN;
        if (buttons & (A6_LB | A6_RB)) pad |= 1u << A6_NITRO;
        if ((buttons & A6_DLEFT) || input->steering < 0) pad |= 1u << A6_LEFT;
        if ((buttons & A6_DRIGHT) || input->steering > 0) pad |= 1u << A6_RIGHT;
    }
    input->pad = pad;
    set_desired(input, input->keyboard | pad);
    if (input->racing) {
        if (rising & A6_X) camera_tap(input, 1);
        if (rising & A6_Y) camera_tap(input, -1);
    }
}

void a6_input_commit(struct a6_input *input, a6_emit_fn emit, void *context)
{
    for (unsigned i = 0; i < A6_ACTION_COUNT; ++i) {
        struct a6_edges *queue = &input->edges[i];
        if (!queue->count) continue;
        bool down = queue->values[queue->head];
        queue->head = (queue->head + 1) % A6_EDGE_CAPACITY;
        --queue->count;
        uint32_t bit = 1u << i;
        if (down == !!(input->reported & bit)) continue;
        input->reported = down ? input->reported | bit : input->reported & ~bit;
        emit(context, (enum a6_action)i, down);
    }
    /* The game's camera/back handler uses an if/else chain on releases.
       Serialize these actions together so none masks another in one update. */
    if (input->command_count) {
        struct a6_command command = input->commands[input->command_head];
        input->command_head = (input->command_head + 1) % A6_EDGE_CAPACITY;
        --input->command_count;
        uint32_t bit = 1u << command.action;
        if (command.down != !!(input->reported & bit)) {
            input->reported = command.down ? input->reported | bit : input->reported & ~bit;
            emit(context, (enum a6_action)command.action, command.down);
        }
    }
}

void a6_cursor_delta(float x, float y, double seconds, float *dx, float *dy)
{
    if (!isfinite(seconds) || seconds < 0) seconds = 0;
    if (seconds > .05) seconds = .05; /* No jump after loading or activation. */
    *dx = a6_axis(x, .22f) * 850 * seconds;
    *dy = a6_axis(y, .22f) * 850 * seconds;
}

bool a6_steering_angles(float axis, float limit, float maximum,
                        float *car_angle, float *result)
{
    if (!isfinite(axis) || !isfinite(limit) || !isfinite(maximum) ||
        limit < 0 || maximum <= 0) return false;
    axis = fmaxf(-1, fminf(1, axis));
    const float center = 1.5707963705062866f;
    *car_angle = center + axis * maximum;
    *result = center + axis * fminf(limit, maximum);
    return true;
}
