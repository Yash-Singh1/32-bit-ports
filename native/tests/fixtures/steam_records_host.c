#include "steam_records.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
struct object { void **vtable; };
static void *input6_table[47], *input5_table[47], *controller_table[33];
static void *storage_table[59], *network_table[22], *party_table[12], *server_table[44];
static void *timeline_table[4];
static struct object input6 = {input6_table}, input5 = {input5_table}, controller = {controller_table};
static struct object storage = {storage_table}, network = {network_table}, party = {party_table}, server = {server_table};
static struct object timeline = {timeline_table};
static unsigned timeline_calls;
static int heartbeat_interval;
static unsigned heartbeat_calls;
static void heartbeat_interval_set(void *self, int interval) {
    if (self != &server || (interval != 30 && interval != -1)) abort();
    heartbeat_interval = interval;
}
static void heartbeat_force(void *self) {
    if (self != &server || (heartbeat_calls == 0 ? heartbeat_interval != 30 : heartbeat_interval != -1)) abort();
    ++heartbeat_calls;
}
static void *browser_table[17];
static struct object browser = {browser_table};
struct list_request { void *handle, *response; bool active, refreshing; ServerFilter **filters; };
static struct list_request lists[2];
static unsigned zero_queries;
/* Distinct real native pointers with the same low 32 bits detect truncation
   and accidental aliases. The fixture never dereferences these identities. */
static const uintptr_t list_handles[] = {UINT64_C(0x1234567800001110), UINT64_C(0xabcdef0200001110)};
static struct list_request *list_for(void *self, void *handle) {
    if (self != &browser) abort();
    if (!handle) return 0;
    for (unsigned i = 0; i < 2; ++i)
        if (lists[i].active && lists[i].handle == handle) return &lists[i];
    abort();
}
static void list_callback(struct list_request *r, unsigned slot, int value) {
    if (!r->response) return;
    void **vtable = *(void ***)r->response;
    ((void (*)(void *, void *, int))vtable[slot])(r->response, r->handle, value);
}
static void *list_request(void *self, unsigned app, void *response) {
    if (self != &browser) abort();
    if (!app) { if (zero_queries != 3) abort(); zero_queries = 0; return 0; }
    if (app != FIXTURE_APP_ID) abort();
    for (unsigned i = 0; i < 2; ++i) {
        if (lists[i].active) continue;
        struct list_request *r = &lists[i];
        *r = (struct list_request){(void *)list_handles[i], response, true, true};
        /* Deliberately synchronous, before the returned handle is published. */
        list_callback(r, 0, 7);
        if (!r->active) abort(); /* release must await completion of creation */
        return r->handle;
    }
    abort();
}
static void *list_filtered_request(void *self, unsigned app, ServerFilter **filters, unsigned count, void *response) {
    if ((count && (!filters || !*filters)) || (count && count != 2)) abort();
    void *handle = list_request(self, app, response);
    if (handle && count) list_for(self, handle)->filters = filters;
    return handle;
}
static void list_release(void *self, void *handle) {
    struct list_request *r = list_for(self, handle);
    if (r) { r->active = false; r->response = 0; }
}
static void list_cancel(void *self, void *handle) {
    struct list_request *r = list_for(self, handle);
    if (r) r->refreshing = false;
}
static void list_refresh(void *self, void *handle) {
    struct list_request *r = list_for(self, handle);
    if (!r) return;
    r->refreshing = true;
    list_callback(r, 1, -3);
    r->refreshing = false;
    /* The guest may release the request inside this completion callback. */
    list_callback(r, 2, 2);
    if (!r->active) abort(); /* release must await the outer refresh return */
}
static bool list_is_refreshing(void *self, void *handle) {
    struct list_request *r = list_for(self, handle);
    if (!handle) zero_queries |= 1;
    return r && r->refreshing;
}
static int list_count(void *self, void *handle) {
    struct list_request *r = list_for(self, handle);
    if (!handle) zero_queries |= 2;
    if (r && r->filters) {
        ServerFilter *f = *r->filters;
        if (!f || strcmp(f[0].key, "map") || strcmp(f[0].value, "c1m1_hotel") ||
            strcmp(f[1].key, "gametagsand") || strcmp(f[1].value, "coop")) abort();
    }
    return r ? 9 : 0;
}
static void *list_worker(void *opaque) {
    list_callback(opaque, 0, 321);
    return 0;
}
static void list_refresh_server(void *self, void *handle, int server) {
    struct list_request *r = list_for(self, handle);
    if (!r) return;
    if (server != 321) abort();
    pthread_t worker;
    if (pthread_create(&worker, 0, list_worker, r) || pthread_join(worker, 0)) abort();
    if (!r->active) abort(); /* worker callback must not free an in-flight h */
}
static void timeline_state(void *self, const char *description, float offset) {
    if (self != &timeline || strcmp(description, "fixture state") || offset != -1.25f || timeline_calls++ != 0) abort();
}
static void timeline_clear(void *self, float offset) {
    if (self != &timeline || offset != 0.5f || timeline_calls++ != 1) abort();
}
static void timeline_event(void *self, const char *icon, const char *title, const char *description,
                           unsigned priority, float offset, float duration, int clip) {
    if (self != &timeline || strcmp(icon, "steam_death") || strcmp(title, "fixture title") ||
        strcmp(description, "fixture description") || priority != 1000 || offset != -3.25f ||
        duration != 20.5f || clip != 3 || timeline_calls++ != 2) abort();
}
static void timeline_mode(void *self, int mode) {
    if (self != &timeline || mode != 2 || timeline_calls++ != 3) abort();
}
static void check(void *self, uint64_t device) {
    if ((self != &input5 && self != &input6 && self != &controller) || device != DEVICE) abort();
}
static Digital digital(void *self, uint64_t device, uint64_t action) {
    check(self, device);
    if (action < ACTION || action > ACTION + 3) abort();
    return (Digital){(action - ACTION) & 1, ((action - ACTION) >> 1) & 1};
}
static Analog analog(void *self, uint64_t device, uint64_t action) {
    check(self, device); if (action != ACTION) abort();
    return (Analog){7, 1.25f, -2.5f, true};
}
static Motion motion(void *self, uint64_t device) {
    check(self, device); Motion m;
    for (unsigned i = 0; i < 10; ++i) m.values[i] = (float)i + 0.25f;
    return m;
}
static void (*event_hooks[2])(Event *);
static void events(void *self, void (*callback)(Event *)) {
    check(self, DEVICE); event_hooks[self == &input6] = callback;
}
static void *fire_events(void *opaque) {
    unsigned slot = (unsigned)(uintptr_t)opaque;
    Event event = {.controller = DEVICE, .kind = 0};
    event.value.digital.action = ACTION;
    event.value.digital.data = (Digital){true, true};
    if (event_hooks[slot]) event_hooks[slot](&event);
    event.kind = 1;
    event.value.analog.action = ACTION;
    event.value.analog.data = (Analog){7, 1.25f, -2.5f, true};
    if (event_hooks[slot]) event_hooks[slot](&event);
    return 0;
}
static void frame(void *self, bool reserved) {
    (void)reserved; check(self, DEVICE);
    pthread_t worker;
    if (pthread_create(&worker, 0, fire_events, (void *)(uintptr_t)(self == &input6)) || pthread_join(worker, 0)) abort();
}
static char *filename;
static bool details(void *self, uint64_t h, unsigned *app, char **name, int *size, uint64_t *owner) {
    if (self != &storage) abort();
    if (h == 0) { if (name) *name = (char *)UINT64_C(0xdeadbeefbad0); return false; }
    if (h == 1) { if (name) *name = 0; return true; }
    if (!filename) {
        mach_vm_address_t address = UINT64_C(0x1e3f1f000);
        if (mach_vm_allocate(mach_task_self(), &address, 4096, VM_FLAGS_FIXED) != KERN_SUCCESS) abort();
        filename = (char *)(uintptr_t)(address + 0xe67);
    }
    snprintf(filename, 80, "workshop/chamber-%llu.bsp", h);
    if (app) *app = FIXTURE_APP_ID; if (size) *size = 4096; if (owner) *owner = DEVICE;
    if (name) *name = filename; return true;
}
static bool tags(void *self, uint64_t h, const Tags *list) {
    if (self != &storage || h != ACTION) abort();
    if (!list) return true;
    if (!list->count) return true;
    if (list->count != 2 || !list->strings || strcmp(list->strings[0], "test") || strcmp(list->strings[1], "puzzle")) abort();
    return true;
}
static uint32_t listen_socket(void *self, int port, IP ip, uint16_t physical, bool relay) {
    if (self != &network || port != -7 || ip.type != 1 || ip.address[15] != 19 || physical != 65530 || !relay) abort();
    return 123;
}
static uint32_t connection(void *self, IP ip, uint16_t port, int timeout) {
    if (self != &network || ip.type != 1 || ip.address[15] != 19 || port != 65530 || timeout != -9) abort();
    return 456;
}
static bool location(void *self, Location value, int kind, char *out, int length) {
    if (self != &party || value.type != 1 || value.id != DEVICE || kind != 3 || length != 16) abort();
    strcpy(out, "fixture-party"); return true;
}
static IP public_ip(void *self) {
    if (self != &server || heartbeat_interval != -1 || heartbeat_calls != 3) abort();
    IP ip = {.type = 1}; ip.address[15] = 19; return ip;
}
void *CreateInterface(const char *version, int *status) {
    input6_table[17] = input5_table[17] = controller_table[12] = digital;
    input6_table[21] = input5_table[21] = controller_table[15] = analog;
    input6_table[29] = input5_table[29] = controller_table[20] = motion;
    input6_table[8] = input5_table[8] = events;
    input6_table[3] = input5_table[3] = frame;
    storage_table[26] = details; storage_table[37] = tags;
    network_table[8] = listen_socket; network_table[10] = connection;
    party_table[11] = location; server_table[33] = public_ip;
    server_table[42] = heartbeat_interval_set; server_table[43] = heartbeat_force;
    timeline_table[0] = timeline_state; timeline_table[1] = timeline_clear;
    timeline_table[2] = timeline_event; timeline_table[3] = timeline_mode;
    browser_table[1] = list_request; browser_table[6] = list_release;
    browser_table[8] = list_cancel; browser_table[9] = list_refresh;
    browser_table[10] = list_is_refreshing; browser_table[11] = list_count;
    browser_table[12] = list_refresh_server;
    browser_table[0] = browser_table[2] = browser_table[3] = browser_table[4] = browser_table[5] = list_filtered_request;
    if (status) *status = 0;
    if (!strcmp(version, "SteamInput006")) return &input6;
    if (!strcmp(version, "SteamInput005")) return &input5;
    if (!strcmp(version, "SteamController008")) return &controller;
    if (!strcmp(version, "STEAMREMOTESTORAGE_INTERFACE_VERSION016")) return &storage;
    if (!strcmp(version, "SteamNetworking006")) return &network;
    if (!strcmp(version, "SteamParties002")) return &party;
    if (!strcmp(version, "SteamGameServer014")) {
        heartbeat_interval = 0; heartbeat_calls = 0;
        return &server;
    }
    if (!strcmp(version, "SteamMatchMakingServers002")) return &browser;
    if (!strcmp(version, "STEAMTIMELINE_INTERFACE_V001")) {
        timeline_calls = 0;
        return &timeline;
    }
    abort();
}
