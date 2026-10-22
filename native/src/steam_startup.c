#include "steam_startup.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

bool steam_startup_set_app_id(uint32_t app_id)
{
    if (!app_id) return false;
    char identity[11];
    snprintf(identity, sizeof(identity), "%" PRIu32, app_id);
    return setenv("SteamAppId", identity, 1) == 0 &&
        setenv("SteamGameId", identity, 1) == 0;
}

static bool ready(const struct steam_startup_ops *ops)
{
    if (!ops->running(ops->context) || !ops->initialize(ops->context)) return false;
    ops->shutdown(ops->context);
    return true;
}

enum steam_startup_result steam_startup_prepare(const struct steam_startup_ops *ops)
{
    if (ready(ops)) return STEAM_STARTUP_READY;
    /* Open Steam itself: Steam's AppID relaunch would select the original
       library executable instead of this user's converted application. */
    if (!ops->open(ops->context)) return STEAM_STARTUP_OPEN_FAILED;
    do {
        if (ready(ops)) return STEAM_STARTUP_READY;
    } while (ops->wait(ops->context));
    return STEAM_STARTUP_TIMEOUT;
}
