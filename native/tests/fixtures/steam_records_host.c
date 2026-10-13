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
static struct object input6 = {input6_table}, input5 = {input5_table}, controller = {controller_table};
static struct object storage = {storage_table}, network = {network_table}, party = {party_table}, server = {server_table};
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
    if (self != &server) abort();
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
    if (status) *status = 0;
    if (!strcmp(version, "SteamInput006")) return &input6;
    if (!strcmp(version, "SteamInput005")) return &input5;
    if (!strcmp(version, "SteamController008")) return &controller;
    if (!strcmp(version, "STEAMREMOTESTORAGE_INTERFACE_VERSION016")) return &storage;
    if (!strcmp(version, "SteamNetworking006")) return &network;
    if (!strcmp(version, "SteamParties002")) return &party;
    if (!strcmp(version, "SteamGameServer014")) return &server;
    abort();
}
