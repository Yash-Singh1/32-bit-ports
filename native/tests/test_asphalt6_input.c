#include "asphalt6_input.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* Model the recovered queue/commit behavior, including its same-tick
   press/release hazard. The mapper must deliver releases after a real commit. */
struct game { uint32_t held, pressed, released, pending_down, pending_up; };
static void emit(void *context, enum a6_action action, bool down)
{
    struct game *game = context;
    uint32_t bit = 1u << action;
    if (down) {
        if (!(game->held & bit)) game->pending_down |= bit;
    } else if (game->held & bit) game->pending_up |= bit;
}
static void tick(struct a6_input *input, struct game *game)
{
    a6_input_commit(input, emit, game);
    game->held = (game->held | game->pending_down) & ~game->pending_up;
    game->pressed = game->pending_down;
    game->released = game->pending_up;
    game->pending_down = game->pending_up = 0;
}
static void close_to(float actual, float expected)
{
    assert(fabsf(actual - expected) < .00001f);
}
int main(void)
{
    struct a6_input input = {0};
    struct game game = {0};
    a6_input_mode(&input, true);
    /* A trigger already held when the controller connects works immediately. */
    a6_input_sample(&input, A6_RT | A6_LB, 0);
    tick(&input, &game);
    assert(game.held == ((1u << A6_UP) | (1u << A6_NITRO)));
    a6_input_release_pad(&input);
    tick(&input, &game);
    assert(!game.held);

    /* Multiple quick shoulder taps retain both presses and both releases. */
    for (unsigned i = 0; i < 2; ++i) {
        a6_input_sample(&input, A6_LB, 0);
        a6_input_sample(&input, 0, 0);
    }
    for (unsigned i = 0; i < 4; ++i) {
        tick(&input, &game);
        assert(game.held == (i % 2 ? 0 : 1u << A6_NITRO));
        assert(i % 2 ? game.released == (1u << A6_NITRO) :
                       game.pressed == (1u << A6_NITRO));
    }
    /* Releasing either source cannot release the other source's held key. */
    a6_input_key(&input, A6_UP, true);
    a6_input_sample(&input, A6_RT, 0);
    tick(&input, &game);
    a6_input_release_pad(&input);
    tick(&input, &game);
    assert(game.held == (1u << A6_UP) && !game.released);
    a6_input_sample(&input, A6_RT, 0);
    a6_input_key(&input, A6_UP, false);
    tick(&input, &game);
    assert(game.held == (1u << A6_UP));
    a6_input_sample(&input, 0, 0);
    tick(&input, &game);
    assert(!game.held);

    /* Pause releases racing inputs even if physical controls remain held. */
    a6_input_sample(&input, A6_LT | A6_RB | A6_DLEFT, -.5f);
    tick(&input, &game);
    assert(game.held & (1u << A6_DOWN));
    a6_input_mode(&input, false);
    a6_input_sample(&input, A6_LT | A6_RB | A6_DLEFT, -.5f);
    tick(&input, &game);
    assert(!game.held && !input.steering);
    a6_input_mode(&input, true);
    a6_input_sample(&input, A6_LT, 0);
    tick(&input, &game);
    assert(game.held == (1u << A6_DOWN));
    a6_input_release_all(&input);
    tick(&input, &game);
    assert(!game.held);

    /* Camera X/Y cycle presets in opposite directions and wait for release. */
    a6_input_sample(&input, A6_X, 0);
    a6_input_sample(&input, 0, 0);
    tick(&input, &game);
    assert(game.held == (1u << A6_CAMERA2));
    tick(&input, &game);
    assert(!game.held && game.released == (1u << A6_CAMERA2));
    a6_input_sample(&input, A6_Y, 0);
    tick(&input, &game);
    assert(game.held == (1u << A6_CAMERA1));
    tick(&input, &game);
    a6_input_sample(&input, 0, 0);

    /* Different camera requests before a commit must retain their order. */
    a6_input_sample(&input, A6_X, 0);
    a6_input_sample(&input, 0, 0);
    a6_input_sample(&input, A6_X, 0);
    a6_input_sample(&input, 0, 0);
    tick(&input, &game);
    assert(game.held == (1u << A6_CAMERA2));
    tick(&input, &game);
    assert(game.released == (1u << A6_CAMERA2));
    tick(&input, &game);
    assert(game.held == (1u << A6_CAMERA3));
    tick(&input, &game);
    assert(game.released == (1u << A6_CAMERA3));

    /* A quick menu click and Back tap also span two commits. */
    a6_input_mode(&input, false);
    a6_input_sample(&input, A6_A | A6_MENU, 0);
    a6_input_sample(&input, 0, 0);
    tick(&input, &game);
    assert(game.held == ((1u << A6_CLICK) | (1u << A6_BACK)));
    tick(&input, &game);
    assert(!game.held);
    a6_input_sample(&input, A6_A, 0);
    tick(&input, &game);
    a6_input_key(&input, A6_CLICK, true);
    a6_input_release_pad(&input);
    tick(&input, &game);
    assert(game.held == (1u << A6_CLICK));
    a6_input_release_all(&input);
    tick(&input, &game);
    assert(!game.held);

    /* Focus/disconnect cancels a down that never reached a commit. */
    a6_input_sample(&input, A6_MENU, 0);
    a6_input_release_all(&input);
    tick(&input, &game);
    assert(!game.held && !game.pressed);
    /* Excess button history must converge to released, never stick down. */
    for (unsigned i = 0; i < 200; ++i) a6_input_key(&input, A6_BACK, !(i & 1));
    for (unsigned i = 0; i <= A6_EDGE_CAPACITY; ++i) tick(&input, &game);
    assert(!game.held);

    close_to(a6_axis(.18f, .18f), 0);
    close_to(a6_axis(.59f, .18f), .5f);
    close_to(a6_axis(-1, .18f), -1);
    close_to(a6_axis(NAN, .18f), 0);
    float angle, result;
    assert(a6_steering_angles(.5f, .2f, .8f, &angle, &result));
    close_to(angle, 1.57079637f + .4f);
    close_to(result, 1.57079637f + .1f);
    assert(a6_steering_angles(-1, .1f, .8f, &angle, &result));
    close_to(result, 1.57079637f - .1f);
    assert(a6_steering_angles(0, .1f, .8f, &angle, &result));
    close_to(result, 1.57079637f);
    assert(!a6_steering_angles(NAN, .1f, .8f, &angle, &result));
    assert(!a6_steering_angles(1, .1f, 0, &angle, &result));
    /* Cursor speed is measured in seconds, independent of presentation rate. */
    float x30, y30, x120, y120;
    a6_cursor_delta(1, -1, 1. / 30, &x30, &y30);
    a6_cursor_delta(1, -1, 1. / 120, &x120, &y120);
    close_to(x30 * 30, x120 * 120);
    close_to(y30 * 30, y120 * 120);
    a6_cursor_delta(1, 0, 5, &x30, &y30);
    close_to(x30, 42.5f);
    puts("Asphalt controller input tests: PASS");
    return 0;
}
