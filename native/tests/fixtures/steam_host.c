/* Original mock client: exercises the real 32-to-64-bit bridge without Steam. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
struct object { void **vtable; };
static void *client17_table[37];
static void *parental_table[6];
static struct object parental = {parental_table};
static void *http_table[25];
static struct object http = {http_table};
static void **client09_table[22], *friends05_table[24];
static struct object friends05 = {friends05_table};
static struct object client09 = {client09_table};
static void *client_table[42], *user_table[32], *utils_table[38], *stats_table[45];
static void *storage_table[59];
static struct object client17 = {client17_table};
static struct object client = {client_table}, user = {user_table}, utils = {utils_table}, stats = {stats_table};
static struct object storage = {storage_table};
static char *ugc_name;
static bool ugc_details(void *self, uint64_t content, unsigned *app,
                        char **name, int *size, uint64_t *owner) {
    if (self != &storage) abort();
    if (content == 0) {
        /* Output values on failure are not valid strings to dereference. */
        if (name) *name = (char *)(uintptr_t)0xe3f1fe67;
        return false;
    }
    if (content == 1) { if (name) *name = NULL; return true; }
    if (content >= 2 && content < 1100) {
        static char changing_name[64];
        snprintf(changing_name, sizeof(changing_name), "workshop/fixture-%llu.bsp",
                 (unsigned long long)content);
        if (name) *name = changing_name;
        return true;
    }
    if (content != UINT64_C(0x123456789abcdef0) ||
        (uintptr_t)ugc_name <= UINT32_MAX) abort();
    if (app) *app = 620;
    if (name) *name = ugc_name;
    if (size) *size = 4096;
    if (owner) *owner = UINT64_C(0xfedcba9876543210);
    return true;
}
struct friend_info { uint64_t game; uint32_t ip; uint16_t port, query; uint64_t lobby; };
_Static_assert(sizeof(struct friend_info) == 24, "FriendGameInfo_t layout");
static bool friend_game(void *self, uint64_t id, struct friend_info *out) {
    if (self != &friends05 || id != UINT64_C(0x123456789abcdef0)) abort();
    *out = (struct friend_info){UINT64_C(0x1122334455667788), 0x7f000001, 27015, 27016, UINT64_C(0xfedcba9876543210)};
    return true;
}
static uint64_t steam_id(void *self) { if (self != &user) abort(); return UINT64_C(0x123456789abcdef0); }
static unsigned app_id(void *self) { if (self != &utils) abort(); return 620; }
static const char *country(void *self) { if (self != &utils) abort(); return "fixture-country"; }
static bool rate(void *self, const char *name, float count, double seconds) {
    return self == &stats && !strcmp(name, "rate") && count == 1.25f && seconds == 2.5;
}
static unsigned callback_count, frames;
static void set_check(void *self, bool (*check)(int)) {
    if ((self != &client && self != &client17) || !check) abort();
    (void)check(1040044);
    if (self == &client && getenv("LP32_FIXTURE_NESTED_IMPORT_CRASH"))
        (void)*(volatile unsigned char *)(uintptr_t)1;
}
static void run_frame(void *self) { if (self != &client17 && self != &client09) abort(); ++frames; }
static unsigned ipc_count(void *self) { if (self != &client17 && self != &client09) abort(); return frames; }
static bool parental_enabled(void *self) { if (self != &parental) abort(); return true; }
static uint64_t subscribed_files(void *self, unsigned start) { if (self != &storage || start != 0) abort(); return UINT64_C(0x123456789abcdef0); }
static unsigned http_request(void *self, int method, const char *url) {
    if (self != &http || method != 1 || strcmp(url, "https://fixture.invalid")) abort();
    return 0x1234abcd;
}
static void *get_interface(void *self, int u, int p, const char *version) {
    if ((self != &client && self != &client17) || u != 17 || p != 23) abort();
    if (!strcmp(version, "SteamUser021") || !strcmp(version, "SteamUser019")) return &user;
    if (!strcmp(version, "SteamUtils010") || !strcmp(version, "SteamUtils009")) return &utils;
    if (!strcmp(version, "STEAMHTTP_INTERFACE_VERSION002")) return &http;
    if (!strcmp(version, "STEAMPARENTALSETTINGS_INTERFACE_VERSION001")) return &parental;
    if (!strcmp(version, "STEAMUSERSTATS_INTERFACE_VERSION012")) return &stats;
    if (!strcmp(version, "STEAMREMOTESTORAGE_INTERFACE_VERSION016") || !strcmp(version, "STEAMREMOTESTORAGE_INTERFACE_VERSION014")) return &storage;
    return 0;
}
void *CreateInterface(const char *version, int *result) {
    if (!ugc_name) ugc_name = strdup("workshop/fixture-chamber.bsp");
    callback_count = 0;
    if (!strcmp(version, "SteamFriends005")) {
        friends05_table[9] = friend_game;
        if (result) *result = 0;
        return &friends05;
    }
    if (!strcmp(version, "SteamClient009")) {
        frames = 0; client09_table[19] = run_frame; client09_table[20] = ipc_count;
        if (result) *result = 0;
        return &client09;
    }
    if (!strcmp(version, "SteamClient017")) {
        frames = 0; client17_table[36] = get_interface; parental_table[0] = parental_enabled; client17_table[23] = get_interface; client17_table[33] = set_check;
        http_table[0] = http_request; client17_table[19] = run_frame; client17_table[20] = ipc_count;
        if (result) *result = 0;
        return &client17;
    }
    if (result) *result = strcmp(version, "SteamClient020") != 0;
    client_table[34] = set_check; client_table[12] = get_interface; user_table[2] = steam_id;
    utils_table[4] = country; utils_table[9] = app_id; stats_table[5] = rate;
    storage_table[26] = ugc_details; storage_table[43] = subscribed_files;
    return !strcmp(version, "SteamClient020") ? &client : 0;
}
struct callback { int user, id; void *data; int size; };
static const uint32_t payload[] = {0x11223344, 0xaabbccdd};
bool Steam_BGetCallback(int pipe, struct callback *cb) {
    if (pipe != 23 || callback_count >= 2) return false;
    if (!callback_count++) { *cb = (struct callback){17, 1040044, (void *)payload, sizeof(payload)}; return true; }
    *cb = (struct callback){17, 102, (void *)payload, sizeof(payload)}; return true;
}
void Steam_FreeLastCallback(int pipe) { if (pipe != 23) abort(); }
bool Steam_GetAPICallResult(int pipe, uint64_t call, void *data, int size, int id, bool *failed) {
    if (pipe != 23 || call != UINT64_C(0xfedcba9876543210) || size != 8 || id != 102) abort();
    memcpy(data, payload, size); *failed = false; return true;
}
