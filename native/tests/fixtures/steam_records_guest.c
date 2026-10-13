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
typedef void *(*list_request_fn)(void *, unsigned, void *);
typedef void *(*list_filtered_fn)(void *, unsigned, ServerFilter **, unsigned, void *);
typedef void (*list_control_fn)(void *, void *);
typedef bool (*list_refreshing_fn)(void *, void *);
typedef int (*list_count_fn)(void *, void *);
typedef void (*list_server_fn)(void *, void *, int);
struct list_response {
    void **vtable;
    void *handle;
    unsigned responded, failed, completed;
    uint32_t guard;
    bool release_on_complete;
    bool release_on_responded;
    bool release_on_failed;
};
static void *browser;
static unsigned list_errors;
static bool list_callback_valid(struct list_response *self, void *handle) {
    if (self->guard != 0x11223344 || !handle || handle == (void *)0x1110 ||
        (self->handle && self->handle != handle)) { ++list_errors; return false; }
    self->handle = handle;
    /* A nested native call must resolve the same full-width host request. */
    if (METHOD(browser, 11, list_count_fn)(browser, handle) != 9) { ++list_errors; return false; }
    return true;
}
static void list_responded(struct list_response *self, void *handle, int server) {
    if (!list_callback_valid(self, handle) || (server != 7 && server != 321)) { ++list_errors; return; }
    ++self->responded;
    if (self->release_on_responded) METHOD(browser, 6, list_control_fn)(browser, handle);
}
static void list_failed(struct list_response *self, void *handle, int server) {
    if (!list_callback_valid(self, handle) || server != -3) { ++list_errors; return; }
    ++self->failed;
    if (self->release_on_failed) METHOD(browser, 6, list_control_fn)(browser, handle);
}
static void list_complete(struct list_response *self, void *handle, int response) {
    if (!list_callback_valid(self, handle) || response != 2) { ++list_errors; return; }
    ++self->completed;
    if (self->release_on_complete) METHOD(browser, 6, list_control_fn)(browser, handle);
}
static void *list_response_table[] = {list_responded, list_failed, list_complete};
static int check_server_lists(void *(*create)(const char *, int *)) {
    browser = create("SteamMatchMakingServers002", 0);
    if (!browser) return -23;
    list_errors = 0;
    list_request_fn request = METHOD(browser, 1, list_request_fn);
    list_control_fn release = METHOD(browser, 6, list_control_fn), cancel = METHOD(browser, 8, list_control_fn);
    list_control_fn refresh = METHOD(browser, 9, list_control_fn);
    list_refreshing_fn refreshing = METHOD(browser, 10, list_refreshing_fn);
    list_count_fn count = METHOD(browser, 11, list_count_fn);
    list_server_fn refresh_server = METHOD(browser, 12, list_server_fn);
    if (refreshing(browser, 0) || count(browser, 0) || request(browser, 0, 0)) return -24;
    release(browser, 0); cancel(browser, 0); refresh(browser, 0); refresh_server(browser, 0, 321);
    struct list_response first = {list_response_table, 0, 0, 0, 0, 0x11223344, false};
    struct list_response second = {list_response_table, 0, 0, 0, 0, 0x11223344, false};
    void *a = request(browser, FIXTURE_APP_ID, &first);
    void *b = request(browser, FIXTURE_APP_ID, &second);
    if (!a || !b || a == b || a == (void *)0x1110 || b == (void *)0x1110 ||
        first.handle != a || second.handle != b || first.responded != 1 || second.responded != 1 ||
        !refreshing(browser, a) || !refreshing(browser, b)) return -25;
    cancel(browser, a);
    if (refreshing(browser, a) || count(browser, a) != 9) return -26;
    refresh(browser, a);
    if (first.failed != 1 || first.completed != 1 || refreshing(browser, a)) return -27;
    second.release_on_responded = true;
    refresh_server(browser, b, 321);
    if (second.responded != 2 || count(browser, b)) return -28;
    release(browser, a);
    if (refreshing(browser, a) || count(browser, a) || count(browser, (void *)0x1110)) return -29;
    /* The host reuses the first address; the stale guest identity must not. */
    first = (struct list_response){list_response_table, 0, 0, 0, 0, 0x11223344, true};
    void *c = request(browser, FIXTURE_APP_ID, &first);
    if (!c || c == a || c == b || first.handle != c || count(browser, a)) return -30;
    refresh(browser, c); /* release nested in completion */
    if (first.failed != 1 || first.completed != 1 || refreshing(browser, c) || count(browser, c)) return -31;
    release(browser, c); /* rejected stale identity must never reach the host */
    release(browser, b);
    void *d = request(browser, FIXTURE_APP_ID, 0);
    if (!d || d == a || d == b || d == c || !refreshing(browser, d)) return -32;
    release(browser, d);
    ServerFilter filters[2] = {{"map", "c1m1_hotel"}, {"gametagsand", "coop"}};
    struct { uint32_t before; ServerFilter *pointer; uint32_t after; } cell = {0x88776655, filters, 0xaabbccdd};
    unsigned slots[] = {0, 2, 3, 4, 5};
    for (unsigned i = 0; i < 5; ++i) {
        filters[0].key[0] = 'm';
        first = (struct list_response){list_response_table, 0, 0, 0, 0, 0x11223344, false};
        list_filtered_fn filtered = METHOD(browser, slots[i], list_filtered_fn);
        void *e = filtered(browser, FIXTURE_APP_ID, &cell.pointer, 2, &first);
        if (!e || first.handle != e || cell.before != 0x88776655 || cell.after != 0xaabbccdd ||
            cell.pointer != filters) return -34;
        filters[0].key[0] = 'X'; /* native retained records must outlive guest input */
        if (count(browser, e) != 9) return -35;
        release(browser, e);
    }
    list_filtered_fn filtered = METHOD(browser, 0, list_filtered_fn);
    if (filtered(browser, FIXTURE_APP_ID, 0, 2, 0) || filtered(browser, FIXTURE_APP_ID, &cell.pointer, 65537, 0)) return -36;
    if (filtered(browser, FIXTURE_APP_ID, (ServerFilter **)UINT32_C(0xfffffffd), 1, 0)) return -38;
    cell.pointer = (ServerFilter *)UINT32_C(0xfffffff0);
    if (filtered(browser, FIXTURE_APP_ID, &cell.pointer, 1, 0) ||
        cell.before != 0x88776655 || cell.after != 0xaabbccdd) return -39;
    struct list_response invalid = {(void **)UINT32_C(0xfffffff9), 0, 0, 0, 0, 0x11223344, false};
    if (request(browser, FIXTURE_APP_ID, &invalid) ||
        request(browser, FIXTURE_APP_ID, (void *)UINT32_C(0xfffffffd))) return -40;
    cell.pointer = filters;
    void *empty = filtered(browser, FIXTURE_APP_ID, 0, 0, 0);
    if (!empty || count(browser, empty) != 9) return -37;
    release(browser, empty);
    first = (struct list_response){list_response_table, 0, 0, 0, 0, 0x11223344, false, true};
    if (request(browser, FIXTURE_APP_ID, &first) || !first.handle || first.responded != 1 ||
        count(browser, first.handle)) return -41; /* release during creation */
    first = (struct list_response){list_response_table, 0, 0, 0, 0, 0x11223344, false, false, true};
    void *last = request(browser, FIXTURE_APP_ID, &first);
    if (!last) return -42;
    refresh(browser, last); /* release cancels the later completion callback */
    if (first.failed != 1 || first.completed || count(browser, last)) return -43;
    return list_errors ? -33 : 0;
}
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
    typedef void (*heartbeat_interval_fn)(void *, int);
    typedef void (*heartbeat_force_fn)(void *);
    METHOD(server, 42, heartbeat_interval_fn)(server, 30);
    METHOD(server, 43, heartbeat_force_fn)(server);
    METHOD(server, 42, heartbeat_interval_fn)(server, -1);
    METHOD(server, 43, heartbeat_force_fn)(server);
    METHOD(server, 43, heartbeat_force_fn)(server);
    typedef IP (*ip_fn)(void *);
    IP received = METHOD(server, 33, ip_fn)(server);
    if (memcmp(&ip, &received, sizeof(ip))) return -18;
    int list_error = check_server_lists(create);
    if (list_error) return list_error;
    int zlib_error = check_zlib();
    return zlib_error ? zlib_error : 26;
}
