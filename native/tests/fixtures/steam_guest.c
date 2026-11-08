#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#define METHOD(obj, slot, type) ((type)((*(void ***)(obj))[slot]))
static unsigned guest_registrations, guest_unregistrations;
void SteamAPI_RegisterCallback(void *object, int id) { (void)object; (void)id; ++guest_registrations; }
void SteamAPI_UnregisterCallback(void *object) { (void)object; ++guest_unregistrations; }
static int checked_callback;
static bool registered_callback(int callback) {
    (void)getenv("LP32_STEAM_FIXTURE"); // Nested guest import inside a native callback.
    checked_callback = callback;
    return false;
}
int check_steam(void) {
    guest_registrations = guest_unregistrations = 0;
    /* A nested native callback must not reuse this live guest stack range. */
    volatile unsigned char deep_stack[96 * 1024];
    for (unsigned i = 0; i < sizeof(deep_stack); ++i) deep_stack[i] = (unsigned char)(i * 7 + 3);
    const char *number = "0x123456789abcdef0!";
    struct { char *end; uint32_t guard; } parsed = {0, 0xaabbccdd};
    if (strtoull(number, &parsed.end, 0) != UINT64_C(0x123456789abcdef0) ||
        *parsed.end != '!' || parsed.guard != 0xaabbccdd ||
        strtoll("-4294967297", 0, 10) != -INT64_C(4294967297)) return -80;
    void *library = dlopen(getenv("LP32_STEAM_FIXTURE"), RTLD_NOW);
    void *(*create)(const char *, int *) = dlsym(library, "CreateInterface");
    if (!library || !create) return -70;
    int status = -1;
    void *client = create("SteamClient020", &status);
    if (!client || status) return -71;
    typedef void *(*get_fn)(void *, int, int, const char *);
    get_fn get = METHOD(client, 12, get_fn);
    void *user = get(client, 17, 23, "SteamUser021");
    void *utils = get(client, 17, 23, "SteamUtils010");
    void *stats = get(client, 17, 23, "STEAMUSERSTATS_INTERFACE_VERSION012");
    typedef uint64_t (*id_fn)(void *);
    typedef unsigned (*app_fn)(void *);
    typedef const char *(*string_fn)(void *);
    typedef bool (*rate_fn)(void *, const char *, float, double);
    if (!user || !utils || !stats || get(client, 17, 23, "SteamUser021") != user) return -72;
    if (METHOD(user, 2, id_fn)(user) != UINT64_C(0x123456789abcdef0)) return -73;
    /* AppID follows a private virtual slot omitted from the JSON metadata. */
    if (METHOD(utils, 9, app_fn)(utils) != 620 ||
        strcmp(METHOD(utils, 4, string_fn)(utils), "fixture-country")) return -74;
    if (!METHOD(stats, 5, rate_fn)(stats, "rate", 1.25f, 2.5)) return -75;
    void *storage = get(client, 17, 23, "STEAMREMOTESTORAGE_INTERFACE_VERSION016");
    typedef bool (*ugc_fn)(void *, uint64_t, unsigned *, char **, int *, uint64_t *);
    if (!storage) return -84;
    ugc_fn details = METHOD(storage, 26, ugc_fn);
    struct { uint32_t before; char *name; uint32_t after; } filename = {
        0x11223344, 0, 0xaabbccdd
    };
    unsigned ugc_app = 0; int ugc_size = 0; uint64_t ugc_owner = 0;
    if (!details(storage, UINT64_C(0x123456789abcdef0), &ugc_app,
                 &filename.name, &ugc_size, &ugc_owner) ||
        filename.before != 0x11223344 || filename.after != 0xaabbccdd)
        return -85;
    if (!filename.name || (uintptr_t)filename.name >= 0x70000000U ||
        strcmp(filename.name, "workshop/fixture-chamber.bsp") || ugc_app != 620 ||
        ugc_size != 4096 || ugc_owner != UINT64_C(0xfedcba9876543210)) return -86;
    char *saved_name = filename.name;
    if (details(storage, 0, &ugc_app, &filename.name, &ugc_size, &ugc_owner) ||
        filename.name != saved_name || filename.after != 0xaabbccdd) return -87;
    filename.name = 0;
    if (!details(storage, UINT64_C(0x123456789abcdef0), 0, &filename.name, 0, 0) ||
        filename.name != saved_name || filename.after != 0xaabbccdd ||
        !details(storage, UINT64_C(0x123456789abcdef0), 0, 0, 0, 0)) return -88;
    if (!details(storage, 1, 0, &filename.name, 0, 0) || filename.name ||
        filename.after != 0xaabbccdd) return -89;
    /* Native Steam may reuse its string buffer; older guest copies must
       survive subsequent queries and growth beyond the old 1024-entry cache. */
    char *previous_name = saved_name;
    for (unsigned n = 2; n < 1100; ++n) {
        filename.name = previous_name;
        bool ok = details(storage, n, 0, &filename.name, 0, 0);
        if (filename.before != 0x11223344 || filename.after != 0xaabbccdd ||
            strcmp(saved_name, "workshop/fixture-chamber.bsp")) return -90;
        if (!ok) return -91;
        if (!filename.name || filename.name == previous_name ||
            strncmp(filename.name, "workshop/fixture-", 17)) return -92;
        previous_name = filename.name;
    }
    void *legacy = create("SteamClient017", &status);
    typedef void (*frame_fn)(void *);
    if (!legacy || status) return -81;
    METHOD(legacy, 19, frame_fn)(legacy);
    if (METHOD(legacy, 20, app_fn)(legacy) != 1) return -82;
    /* SteamClient017's PS3-only slot is absent from the Mac vtable. */
    void *http = METHOD(legacy, 23, get_fn)(legacy, 17, 23, "STEAMHTTP_INTERFACE_VERSION002");
    typedef unsigned (*http_fn)(void *, int, const char *);
    if (!http || METHOD(http, 0, http_fn)(http, 1, "https://fixture.invalid") != 0x1234abcd) return -93;
    void *old_user = get(client, 17, 23, "SteamUser019");
    void *old_utils = get(client, 17, 23, "SteamUtils009");
    if (!old_user || !old_utils || METHOD(old_user, 2, id_fn)(old_user) != UINT64_C(0x123456789abcdef0) ||
        METHOD(old_utils, 9, app_fn)(old_utils) != 620 ||
        strcmp(METHOD(old_utils, 4, string_fn)(old_utils), "fixture-country")) return -94;
    void *parental = METHOD(legacy, 36, get_fn)(legacy, 17, 23, "STEAMPARENTALSETTINGS_INTERFACE_VERSION001");
    typedef bool (*enabled_fn)(void *);
    if (!parental || !METHOD(parental, 0, enabled_fn)(parental)) return -96;
    void *old_storage = get(client, 17, 23, "STEAMREMOTESTORAGE_INTERFACE_VERSION014");
    typedef uint64_t (*subscribed_fn)(void *, unsigned);
    if (!old_storage || METHOD(old_storage, 43, subscribed_fn)(old_storage, 0) != UINT64_C(0x123456789abcdef0)) return -97;
    void *old_client = create("SteamClient009", &status);
    if (!old_client || status) return -93;
    METHOD(old_client, 19, frame_fn)(old_client);
    if (METHOD(old_client, 20, app_fn)(old_client) != 1) return -94;
    /* The legacy facade has no callback-check hook. Skip the known modern
       private notification, but still deliver public Steam callbacks. */
    if (getenv("LP32_GAME") && !strcmp(getenv("LP32_GAME"), "l4d1")) {
        void *friends = create("SteamFriends005", &status);
        struct friend_info { uint64_t game; uint32_t ip; uint16_t port, query; uint64_t lobby; };
        struct { uint32_t before; struct friend_info info; uint32_t after; } friend_result = {0x11223344, {0}, 0xaabbccdd};
        typedef bool (*friend_fn)(void *, uint64_t, struct friend_info *);
        if (!friends || status || !METHOD(friends, 9, friend_fn)(friends, UINT64_C(0x123456789abcdef0), &friend_result.info) ||
            friend_result.before != 0x11223344 || friend_result.after != 0xaabbccdd ||
            friend_result.info.game != UINT64_C(0x1122334455667788) || friend_result.info.port != 27015 ||
            friend_result.info.lobby != UINT64_C(0xfedcba9876543210)) return -101;
        int object = 0;
        void (*reg)(void *, int) = dlsym(RTLD_DEFAULT, "SteamAPI_RegisterCallback");
        void (*unreg)(void *) = dlsym(RTLD_DEFAULT, "SteamAPI_UnregisterCallback");
        if (!reg || !unreg) return -98;
        reg(&object, 102);
        if (guest_registrations != 1) return -99;
        struct { int user, id; uint32_t *data; int size; } early;
        bool (*poll)(int, void *) = dlsym(library, "Steam_BGetCallback");
        void (*done)(int) = dlsym(library, "Steam_FreeLastCallback");
        if (!poll || !done || !poll(23, &early) || early.id != 102) return -95;
        done(23);
        if (poll(23, &early)) return -96;
        unreg(&object);
        if (guest_unregistrations != 1) return -100;
        if (!create("SteamClient020", &status) || status) return -97;
    }
    typedef void (*check_fn)(void *, bool (*)(int));
    checked_callback = 0;
    METHOD(legacy, 33, check_fn)(legacy, registered_callback);
    if (checked_callback != 1040044) return -95;
    METHOD(client, 34, check_fn)(client, registered_callback);
    checked_callback = 0;
    struct callback { int user, id; uint32_t *data; int size; } cb;
    bool (*next)(int, struct callback *) = dlsym(library, "Steam_BGetCallback");
    void (*release)(int) = dlsym(library, "Steam_FreeLastCallback");
    bool (*get_result)(int, uint64_t, void *, int, int, bool *) = dlsym(library, "Steam_GetAPICallResult");
    if (!next || !release || !get_result || !next(23, &cb) || cb.user != 17 ||
        cb.id != 102 || cb.size != 8 || cb.data[0] != 0x11223344 || cb.data[1] != 0xaabbccdd) return -76;
    if (checked_callback != 1040044) return -83;
    release(23); if (next(23, &cb)) return -77;
    uint32_t data[2]; bool failed = true;
    if (!get_result(23, UINT64_C(0xfedcba9876543210), data, sizeof(data), 102, &failed) ||
        failed || data[0] != 0x11223344 || data[1] != 0xaabbccdd) return -78;
    for (unsigned i = 0; i < sizeof(deep_stack); ++i)
        if (deep_stack[i] != (unsigned char)(i * 7 + 3)) return -102;
    return dlclose(library) ? -79 : 0;
}
