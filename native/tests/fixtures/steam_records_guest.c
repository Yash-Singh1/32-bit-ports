#include "steam_records.h"
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define METHOD(obj, slot, type) ((type)((*(void ***)(obj))[slot]))
extern int check_zlib(void);
typedef Digital (*digital_fn)(void *, uint64_t, uint64_t);
typedef Analog (*analog_fn)(void *, uint64_t, uint64_t);
typedef Motion (*motion_fn)(void *, uint64_t);
typedef void (*event_fn)(void *, void (*)(Event *));
typedef void (*frame_fn)(void *, bool);
static void *input;
static unsigned event_count, other_count;
static void callback(Event *event) {
    if (event->controller != DEVICE || event->kind > 1) { event_count = 10000; return; }
    if (event->kind == 0) {
        if (event->value.digital.action != ACTION || !event->value.digital.data.bState ||
            !event->value.digital.data.bActive) { event_count = 10000; return; }
    } else if (event->value.analog.action != ACTION || event->value.analog.data.eMode != 7 ||
               event->value.analog.data.x != 1.25f || event->value.analog.data.y != -2.5f ||
               !event->value.analog.data.bActive) { event_count = 10000; return; }
    // A nested struct-return import must not damage the outer callback stack.
    Analog a = METHOD(input, 21, analog_fn)(input, DEVICE, ACTION);
    if (a.x != 1.25f || a.y != -2.5f) { event_count = 10000; return; }
    ++event_count;
}
static void other_callback(Event *event) { (void)event; ++other_count; }
int LauncherMain(void) {
    const char *app_identity = getenv("SteamAppId"), *game_identity = getenv("SteamGameId");
    if (!app_identity || !game_identity ||
        strcmp(app_identity, FIXTURE_STRINGIFY(FIXTURE_APP_ID)) ||
        strcmp(game_identity, FIXTURE_STRINGIFY(FIXTURE_APP_ID))) return -21;
    event_count = other_count = 0;
    void *library = dlopen(getenv("LP32_STEAM_FIXTURE"), RTLD_NOW);
    void *(*create)(const char *, int *) = dlsym(library, "CreateInterface");
    if (!library || !create) return -1;
    const char *versions[] = {"SteamInput006", "SteamInput005", "SteamController008"};
    for (unsigned v = 0; v < 3; ++v) {
        void *obj = create(versions[v], 0); if (!obj) return -2;
        unsigned d = v == 2 ? 12 : 17, a = v == 2 ? 15 : 21, m = v == 2 ? 20 : 29;
        for (unsigned repeat = 0; repeat < 2000; ++repeat) {
            Digital digital = METHOD(obj, d, digital_fn)(obj, DEVICE, ACTION + (repeat & 3));
            if (digital.bState != ((repeat & 1) != 0) || digital.bActive != ((repeat & 2) != 0)) return -3;
            struct { uint32_t before; Analog value; uint32_t after; } av = {.before = 0x11223344, .after = 0xaabbccdd};
            av.value = METHOD(obj, a, analog_fn)(obj, DEVICE, ACTION);
            if (av.before != 0x11223344 || av.after != 0xaabbccdd || av.value.eMode != 7 ||
                av.value.x != 1.25f || av.value.y != -2.5f || !av.value.bActive) return -4;
            struct { uint32_t before; Motion value; uint32_t after; } mv = {.before = 0x11223344, .after = 0xaabbccdd};
            mv.value = METHOD(obj, m, motion_fn)(obj, DEVICE);
            if (mv.before != 0x11223344 || mv.after != 0xaabbccdd) return -5;
            for (unsigned j = 0; j < 10; ++j) if (mv.value.values[j] != (float)j + 0.25f) return -6;
        }
    }
    void *timeline = create("STEAMTIMELINE_INTERFACE_V001", 0);
    if (!timeline) return -22;
    typedef void (*timeline_state_fn)(void *, const char *, float);
    typedef void (*timeline_clear_fn)(void *, float);
    typedef void (*timeline_event_fn)(void *, const char *, const char *, const char *, unsigned, float, float, int);
    typedef void (*timeline_mode_fn)(void *, int);
    METHOD(timeline, 0, timeline_state_fn)(timeline, "fixture state", -1.25f);
    METHOD(timeline, 1, timeline_clear_fn)(timeline, 0.5f);
    METHOD(timeline, 2, timeline_event_fn)(timeline, "steam_death", "fixture title", "fixture description",
                                         1000, -3.25f, 20.5f, 3);
    METHOD(timeline, 3, timeline_mode_fn)(timeline, 2);
    input = create("SteamInput006", 0);
    void *input5 = create("SteamInput005", 0);
    METHOD(input, 8, event_fn)(input, callback);
    METHOD(input5, 8, event_fn)(input5, other_callback);
    METHOD(input, 3, frame_fn)(input, false);
    METHOD(input5, 3, frame_fn)(input5, false);
    if (event_count != 2 || other_count != 2) return -7;
    METHOD(input, 8, event_fn)(input, other_callback);
    METHOD(input, 3, frame_fn)(input, false);
    METHOD(input, 8, event_fn)(input, 0);
    METHOD(input, 3, frame_fn)(input, false);
    if (event_count != 2 || other_count != 4) return -8;
    void *storage = create("STEAMREMOTESTORAGE_INTERFACE_VERSION016", 0);
    typedef bool (*details_fn)(void *, uint64_t, unsigned *, char **, int *, uint64_t *);
    details_fn details = METHOD(storage, 26, details_fn);
    struct { uint32_t before; char *name; uint32_t after; } out = {0x11223344, 0, 0xaabbccdd};
    unsigned app = 0; int size = 0; uint64_t owner = 0;
    if (!details(storage, 2, &app, &out.name, &size, &owner) || app != FIXTURE_APP_ID || size != 4096 ||
        owner != DEVICE || strcmp(out.name, "workshop/chamber-2.bsp") ||
        out.before != 0x11223344 || out.after != 0xaabbccdd) return -9;
    char *saved = out.name;
    if (!details(storage, 3, 0, &out.name, 0, 0) || strcmp(out.name, "workshop/chamber-3.bsp") ||
        strcmp(saved, "workshop/chamber-2.bsp")) return -10;
    saved = out.name;
    if (details(storage, 0, 0, &out.name, 0, 0) || out.name != saved) return -11;
    if (!details(storage, 1, 0, &out.name, 0, 0) || out.name ||
        !details(storage, 2, 0, 0, 0, 0) || out.after != 0xaabbccdd) return -12;
    for (unsigned j = 4; j < 1100; ++j) {
        if (!details(storage, j, 0, &out.name, 0, 0) || !out.name ||
            out.before != 0x11223344 || out.after != 0xaabbccdd) return -19;
    }
    if (strcmp(saved, "workshop/chamber-3.bsp")) return -20;
    typedef bool (*tags_fn)(void *, uint64_t, const Tags *);
    const char *strings[] = {"test", "puzzle"}; Tags tags = {strings, 2};
    tags_fn set_tags = METHOD(storage, 37, tags_fn);
    if (!set_tags(storage, ACTION, &tags) || !set_tags(storage, ACTION, 0)) return -13;
    tags.count = 0; if (!set_tags(storage, ACTION, &tags)) return -14;
    tags.count = -1; if (set_tags(storage, ACTION, &tags)) return -15;
    void *network = create("SteamNetworking006", 0);
    IP ip = {.type = 1}; ip.address[15] = 19;
    typedef uint32_t (*listen_fn)(void *, int, IP, uint16_t, bool);
    typedef uint32_t (*connect_fn)(void *, IP, uint16_t, int);
    if (METHOD(network, 8, listen_fn)(network, -7, ip, 65530, true) != 123 ||
        METHOD(network, 10, connect_fn)(network, ip, 65530, -9) != 456) return -16;
    void *party = create("SteamParties002", 0); Location place = {1, DEVICE}; char text[16];
    typedef bool (*location_fn)(void *, Location, int, char *, int);
    if (!METHOD(party, 11, location_fn)(party, place, 3, text, sizeof(text)) || strcmp(text, "fixture-party")) return -17;
    void *server = create("SteamGameServer014", 0);
    typedef IP (*ip_fn)(void *);
    IP received = METHOD(server, 33, ip_fn)(server);
    if (memcmp(&ip, &received, sizeof(ip))) return -18;
    int zlib_error = check_zlib();
    return zlib_error ? zlib_error : 26;
}
