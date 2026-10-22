#include "steam_startup.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
    bool running, authenticated, open_ok;
    unsigned opens, initializes, shutdowns, waits, ready_after;
};
static bool running(void *p) { return ((struct fixture *)p)->running; }
static bool initialize(void *p) {
    struct fixture *f = p;
    assert(f->running);
    ++f->initializes;
    return f->authenticated;
}
static void shutdown(void *p) { ++((struct fixture *)p)->shutdowns; }
static bool open_steam(void *p) {
    struct fixture *f = p;
    ++f->opens;
    return f->open_ok;
}
static bool wait_for_steam(void *p) {
    struct fixture *f = p;
    if (++f->waits >= 5) return false;
    if (f->ready_after && f->waits == f->ready_after)
        f->running = f->authenticated = true;
    return true;
}
static enum steam_startup_result prepare(struct fixture *f) {
    const struct steam_startup_ops ops = {f, running, initialize, shutdown, open_steam, wait_for_steam};
    return steam_startup_prepare(&ops);
}
int main(void) {
    assert(setenv("SteamAppId", "unrelated", 1) == 0);
    assert(setenv("SteamGameId", "620", 1) == 0);
    assert(steam_startup_set_app_id(550));
    assert(!strcmp(getenv("SteamAppId"), "550"));
    assert(!strcmp(getenv("SteamGameId"), "550"));
    assert(!steam_startup_set_app_id(0));
    assert(!strcmp(getenv("SteamAppId"), "550"));
    assert(!strcmp(getenv("SteamGameId"), "550"));
    assert(steam_startup_set_app_id(620));
    assert(!strcmp(getenv("SteamAppId"), "620"));
    assert(!strcmp(getenv("SteamGameId"), "620"));
    assert(steam_startup_set_app_id(UINT32_MAX));
    assert(!strcmp(getenv("SteamAppId"), "4294967295"));
    assert(!strcmp(getenv("SteamGameId"), "4294967295"));
    struct fixture ready = {.running = true, .authenticated = true};
    assert(prepare(&ready) == STEAM_STARTUP_READY);
    assert(ready.opens == 0 && ready.waits == 0 && ready.shutdowns == 1);
    struct fixture closed = {.open_ok = true, .ready_after = 2};
    assert(prepare(&closed) == STEAM_STARTUP_READY);
    assert(closed.opens == 1 && closed.waits == 2 && closed.initializes == 1 && closed.shutdowns == 1);
    struct fixture login = {.running = true, .open_ok = true, .ready_after = 3};
    assert(prepare(&login) == STEAM_STARTUP_READY);
    assert(login.opens == 1 && login.waits == 3 && login.shutdowns == 1);
    struct fixture missing = {0};
    assert(prepare(&missing) == STEAM_STARTUP_OPEN_FAILED);
    assert(missing.opens == 1 && missing.waits == 0 && missing.shutdowns == 0);
    struct fixture timeout = {.running = true, .open_ok = true};
    assert(prepare(&timeout) == STEAM_STARTUP_TIMEOUT);
    assert(timeout.opens == 1 && timeout.waits == 5 && timeout.shutdowns == 0);
    puts("Steam startup: PASS (game identity, already ready, automatic launch, sign-in wait, launch failure, bounded timeout)");
}
