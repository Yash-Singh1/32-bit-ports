#include "steam_bridge.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include "crash_trace.h"
#include <dlfcn.h>
#include <ffi/ffi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

struct steam_method { const char *name; char result; const char *args; };
struct steam_interface { const char *version; unsigned first, count; };
#include "steam_abi.inc"
static void *client_library;
struct steam_proxy { uint32_t guest; void *host; const struct steam_interface *interface; };
static struct steam_proxy proxies[128];
static unsigned proxy_count;
static uint32_t method_thunks[sizeof(steam_methods) / sizeof(steam_methods[0])];
static _Atomic uint32_t warning_hook, callback_check_hook;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static bool trace;
static const char *method_filter;
static bool callback_trace;

/* Valve's public isteammatchmaking.h declares HServerListRequest as void*,
   and ISteamMatchmakingServerListResponse has three slots, no destructor.
   Keep request identities separate from native pointers and interface proxies.
   IDs in the reserved f9600001..f96fffff domain are never reused, including
   when Steam reuses a request address. */
struct server_request {
    void **vtable; /* ISteamMatchmakingServerListResponse, three virtual slots */
    struct server_request *next;
    void *owner, *host;
    void *filters, *filter_records;
    uint32_t guest, response;
    unsigned references, native_calls;
    bool active, creating, release_pending;
};
static struct server_request *server_requests;
static uint32_t next_server_request = 0xf9600001;
static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;

static bool server_guest_range(uint32_t pointer, uint64_t size)
{
    return pointer && (uint64_t)pointer + size <= UINT64_C(0x100000000);
}

static void server_request_drop(struct server_request *request)
{
    if (!request) return;
    pthread_mutex_lock(&server_lock);
    bool dispose = --request->references == 0;
    pthread_mutex_unlock(&server_lock);
    if (dispose) { free(request->filter_records); free(request); }
}

/* Called with server_lock held and with a caller reference still alive. */
static void server_request_retire(struct server_request *request)
{
    if (!request->active) return;
    struct server_request **link = &server_requests;
    while (*link && *link != request) link = &(*link)->next;
    if (*link) *link = request->next;
    request->active = false;
    --request->references; /* registry ownership */
}

static struct server_request *server_request_get(void *owner, uint32_t guest, bool release)
{
    struct server_request *found = NULL;
    pthread_mutex_lock(&server_lock);
    for (struct server_request *r = server_requests; r; r = r->next) {
        if (r->owner == owner && r->guest == guest && r->active && r->host) {
            found = r;
            ++r->references;
            if (release) { server_request_retire(r); r->release_pending = true; }
            else ++r->native_calls;
            break;
        }
    }
    pthread_mutex_unlock(&server_lock);
    return found;
}

static void server_request_finish(struct server_request *request, bool native_call)
{
    if (!request) return;
    pthread_mutex_lock(&server_lock);
    if (native_call) --request->native_calls;
    bool release = request->release_pending && !request->native_calls;
    if (release) request->release_pending = false;
    pthread_mutex_unlock(&server_lock);
    /* Never wait for a native call: its callback may have requested release
       on another thread while the native caller is waiting for that thread.
       The last native caller performs the deferred release with its own ref. */
    if (release) ((void (*)(void *, void *))(*(void ***)request->owner)[6])(
        request->owner, request->host);
    server_request_drop(request);
}

static void server_response(struct server_request *request, void *host, unsigned slot, int value)
{
    uint32_t object = 0, guest = 0;
    pthread_mutex_lock(&server_lock);
    /* A callback may arrive synchronously inside RequestLANServerList. Publish
       the returned host handle before allowing nested guest query methods. */
    if (request->active && request->creating && !request->host) request->host = host;
    if (request->active && request->host == host && host && request->response) {
        ++request->references;
        object = request->response;
        guest = request->guest;
    }
    pthread_mutex_unlock(&server_lock);
    if (!object) return;
    uint32_t vtable, function;
    if (!server_guest_range(object, sizeof(vtable))) {
        server_request_drop(request); return;
    }
    memcpy(&vtable, (void *)(uintptr_t)object, sizeof(vtable));
    if (!server_guest_range(vtable, 3 * sizeof(function))) {
        server_request_drop(request); return;
    }
    memcpy(&function, (void *)(uintptr_t)(vtable + slot * 4), sizeof(function));
    uint32_t arguments[] = {object, guest, (uint32_t)value};
    compat_runtime32_call(function, arguments, 3);
    server_request_drop(request);
}
static void server_responded(struct server_request *r, void *h, int server) { server_response(r, h, 0, server); }
static void server_failed(struct server_request *r, void *h, int server) { server_response(r, h, 1, server); }
static void server_complete(struct server_request *r, void *h, int response) { server_response(r, h, 2, response); }
static void *server_response_vtable[] = {server_responded, server_failed, server_complete};

static int invoke_server_method(const struct steam_proxy *proxy, unsigned slot,
                                const uint32_t *args, uint64_t *result)
{
    void *function = (*(void ***)proxy->host)[slot];
    if (slot <= 5) { /* Request*ServerList */
        struct server_request *request = calloc(1, sizeof(*request));
        if (!request) { *result = 0; return 1; }
        request->vtable = server_response_vtable;
        request->owner = proxy->host;
        request->response = args[slot == 1 ? 2 : 4];
        if (request->response) {
            uint32_t vtable;
            if (!server_guest_range(request->response, sizeof(vtable))) {
                free(request); *result = 0; return 1;
            }
            memcpy(&vtable, (void *)(uintptr_t)request->response, sizeof(vtable));
            if (!server_guest_range(vtable, 3 * sizeof(uint32_t))) {
                free(request); *result = 0; return 1;
            }
        }
        if (slot != 1 && args[3]) {
            uint32_t records = 0;
            if (args[3] > 65536 || !server_guest_range(args[2], sizeof(records))) {
                fprintf(stderr, "compat32: invalid Steam server-list filter array\n");
                free(request); *result = 0; return 1;
            }
            memcpy(&records, (void *)(uintptr_t)args[2], sizeof(records));
            size_t size = (size_t)args[3] * 512;
            if (!server_guest_range(records, size)) {
                fprintf(stderr, "compat32: Steam server-list filters exceed the guest address space\n");
                free(request); *result = 0; return 1;
            }
            /* Valve's ** parameter points to ONE pointer cell containing a
               contiguous array of {char key[256], char value[256]} records.
               Verified in L4D2 serverbrowser StartRefresh at original 0x5d03.
               Retain both widened cell and record copy until ReleaseRequest. */
            request->filters = request->filter_records = malloc(size);
            if (!request->filters) { free(request); *result = 0; return 1; }
            memcpy(request->filters, (void *)(uintptr_t)records, size);
        }
        request->references = 2; /* registry plus this native call */
        request->native_calls = 1;
        request->active = request->creating = true;
        pthread_mutex_lock(&server_lock);
        if (next_server_request == 0xf9700000) {
            pthread_mutex_unlock(&server_lock);
            free(request->filter_records); free(request); *result = 0; return 1;
        }
        request->guest = next_server_request++;
        request->next = server_requests;
        server_requests = request;
        pthread_mutex_unlock(&server_lock);
        void *host;
        if (slot == 1) host = ((void *(*)(void *, uint32_t, void *))function)(proxy->host, args[1],
                              request->response ? request : NULL);
        else host = ((void *(*)(void *, uint32_t, void **, uint32_t, void *))function)(
            proxy->host, args[1], args[2] ? &request->filters : NULL, args[3],
            request->response ? request : NULL);
        pthread_mutex_lock(&server_lock);
        bool mismatch = request->host && request->host != host;
        if (mismatch) {
            pthread_mutex_unlock(&server_lock);
            fprintf(stderr, "compat32: Steam server-list callback returned a different request handle\n");
            abort(); /* Do not free an adapter still owned by an unknown request. */
        }
        request->creating = false;
        if (host) request->host = host;
        if (!host || !request->active) server_request_retire(request);
        *result = request->active ? request->guest : 0;
        pthread_mutex_unlock(&server_lock);
        server_request_finish(request, true);
        return 1;
    }
    /* List records and direct Ping/Players/Rules callback interfaces remain
       unsupported. These scalar request controls also accept native NULL. */
    if (slot != 6 && slot != 8 && slot != 9 && slot != 10 && slot != 11 && slot != 12) return 0;
    uint32_t guest = args[1];
    struct server_request *request = guest ? server_request_get(proxy->host, guest, slot == 6) : NULL;
    if (guest && !request) {
        fprintf(stderr, "compat32: rejected unknown or released Steam server-list handle 0x%08x\n", guest);
        *result = 0; return 1;
    }
    if (slot == 6 && request) {
        *result = 0;
        server_request_finish(request, false);
        return 1;
    }
    void *host = request ? request->host : NULL;
    if (slot == 10) *result = ((bool (*)(void *, void *))function)(proxy->host, host);
    else if (slot == 11) *result = (uint32_t)((int (*)(void *, void *))function)(proxy->host, host);
    else {
        if (slot == 12) ((void (*)(void *, void *, int))function)(proxy->host, host, (int)args[2]);
        else ((void (*)(void *, void *))function)(proxy->host, host);
        *result = 0;
    }
    /* Retirement suppresses further guest callbacks immediately. Native
       release waits for prior query/control calls to finish without blocking. */
    server_request_finish(request, true);
    return 1;
}

/* Valve packs the input records and SteamIPAddress_t to one byte on both
   Darwin architectures. Use typed native calls: libffi's natural struct
   layout would round the 13-byte analog result to 16 bytes. */
#pragma pack(push, 1)
struct steam_digital { bool state, active; };
struct steam_analog { int32_t mode; float x, y; bool active; };
struct steam_motion { float value[10]; };
struct steam_ip { uint8_t address[16]; int32_t type; };
#pragma pack(pop)
#pragma pack(push, 4)
struct steam_location { int32_t type; uint64_t id; };
#pragma pack(pop)
_Static_assert(sizeof(struct steam_digital) == 2, "Steam digital ABI");
_Static_assert(sizeof(struct steam_analog) == 13, "Steam analog ABI");
_Static_assert(sizeof(struct steam_motion) == 40, "Steam motion ABI");
_Static_assert(sizeof(struct steam_ip) == 20, "Steam IP ABI");
_Static_assert(sizeof(struct steam_location) == 12, "Steam party ABI");

static bool indirect_result(char result)
{
    return result == 'A' || result == 'M' || result == 'I';
}

int steam_bridge32_stret(const char *name)
{
    const char *prefix = "_lp32_steam_method_";
    if (strncmp(name, prefix, strlen(prefix))) return 0;
    unsigned index = (unsigned)strtoul(name + strlen(prefix), NULL, 10);
    return index < sizeof(steam_methods) / sizeof(steam_methods[0]) &&
        indirect_result(steam_methods[index].result);
}

static uint64_t guest_u64(const uint32_t *words)
{
    return (uint64_t)words[0] | (uint64_t)words[1] << 32;
}

static _Atomic uint32_t input_event_hooks[2];
static void input_event(unsigned slot, const void *event)
{
    uint32_t hook = input_event_hooks[slot];
    if (!hook || !event) return;
    // Packed handle, event kind, and the larger (21-byte) analog union member.
    uint32_t copy = compat_runtime32_allocate(33, 0);
    if (!copy) return;
    memcpy((void *)(uintptr_t)copy, event, 33);
    compat_runtime32_call(hook, &copy, 1);
    compat_runtime32_deallocate(copy);
}
static void input_event005(void *event) { input_event(0, event); }
static void input_event006(void *event) { input_event(1, event); }

static uint32_t string_copy_locked(const char *text)
{
    // Native accessors return borrowed strings; retain a stable guest copy.
    static uint32_t *strings;
    static size_t count, capacity;
    if (!text) return 0;
    for (size_t i = 0; i < count; ++i) if (!strcmp((const char *)(uintptr_t)strings[i], text)) return strings[i];
    if (count == capacity) {
        size_t next = capacity ? capacity * 2 : 64;
        if (next < capacity || next > SIZE_MAX / sizeof(*strings)) return 0;
        uint32_t *resized = realloc(strings, next * sizeof(*strings));
        if (!resized) return 0;
        strings = resized; capacity = next;
    }
    uint32_t result = compat_runtime32_copy_cstring(text);
    if (result) strings[count++] = result;
    return result;
}

static uint32_t proxy_for_locked(void *object, const char *version)
{
    if (trace) fprintf(stderr, "compat32: Steam interface request %s available=%d\n", version ? version : "(null)", object != NULL);
    if (!object || !version) return 0;
    const struct steam_interface *interface = NULL;
    for (unsigned i = 0; i < sizeof(steam_interfaces) / sizeof(steam_interfaces[0]); ++i) {
        if (!strcmp(steam_interfaces[i].version, version)) { interface = &steam_interfaces[i]; break; }
    }
    if (!interface) { fprintf(stderr, "compat32: unsupported native Steam interface %s\n", version); return 0; }
    for (unsigned i = 0; i < proxy_count; ++i) {
        if (proxies[i].host == object && proxies[i].interface == interface) return proxies[i].guest;
    }
    if (proxy_count == 128) return 0;
    uint32_t guest = compat_runtime32_allocate(4 + 4 * interface->count, 1);
    if (!guest) return 0;
    uint32_t *words = (void *)(uintptr_t)guest;
    words[0] = guest + 4;
    for (unsigned i = 0; i < interface->count; ++i) {
        unsigned method = interface->first + i;
        if (!method_thunks[method]) {
            char name[64]; snprintf(name, sizeof(name), "_lp32_steam_method_%u", method);
            method_thunks[method] = compat_runtime32_guest_callback(name);
        }
        if (!(words[i + 1] = method_thunks[method])) return 0;
    }
    proxies[proxy_count++] = (struct steam_proxy){guest, object, interface};
    fprintf(stderr, "compat32: native Steam interface %s\n", version);
    return guest;
}

static uint32_t proxy_for(void *object, const char *version)
{
    pthread_mutex_lock(&cache_lock);
    uint32_t result = proxy_for_locked(object, version);
    pthread_mutex_unlock(&cache_lock);
    return result;
}
static uint32_t string_copy(const char *text)
{
    pthread_mutex_lock(&cache_lock);
    uint32_t result = string_copy_locked(text);
    pthread_mutex_unlock(&cache_lock);
    return result;
}
static bool plain_callback(int id)
{
    for (unsigned i = 0; i < sizeof(steam_plain_callbacks) / sizeof(steam_plain_callbacks[0]); ++i)
        if (steam_plain_callbacks[i] == id) return true;
    return false;
}

uint32_t steam_bridge32_open(const char *path)
{
    if (!lp32_profile()->steam_app_id || !path || path[0] != '/') return 0;
    const char *base = strrchr(path, '/');
    if (!base || strcmp(base + 1, "steamclient.dylib")) return 0;
    if (!client_library) {
        client_library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!client_library) return 0;
        trace = getenv("LP32_TRACE_STEAM") != NULL;
        method_filter = getenv("LP32_TRACE_STEAM_METHODS");
        const char *callbacks = getenv("LP32_TRACE_STEAM_CALLBACKS");
        callback_trace = callbacks && *callbacks && strcmp(callbacks, "0");
        fprintf(stderr, "compat32: using native Steam client through the i386 interface bridge\n");
    }
    return LP32_STEAM_CLIENT_HANDLE;
}

uint32_t steam_bridge32_symbol(const char *symbol)
{
    static const char *names[] = {"CreateInterface", "Steam_BGetCallback", "Steam_FreeLastCallback",
        "Steam_GetAPICallResult", "Steam_ReleaseThreadLocalMemory", "Breakpad_SteamMiniDumpInit",
        "Breakpad_SteamSetAppID", "Breakpad_SteamSetSteamID", "Breakpad_SteamWriteMiniDumpSetComment",
        "Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId"};
    if (!client_library || !symbol || !dlsym(client_library, symbol)) return 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!strcmp(symbol, names[i])) {
            char thunk[128]; snprintf(thunk, sizeof(thunk), "_lp32_steam_%s", symbol);
            return compat_runtime32_guest_callback(thunk);
        }
    }
    fprintf(stderr, "compat32: unsupported native Steam export %s\n", symbol);
    return 0;
}

static void native_warning(int severity, const char *text)
{
    uint32_t hook = warning_hook;
    if (!hook) return;
    uint32_t copy = compat_runtime32_copy_cstring(text ? text : "");
    uint32_t args[] = {(uint32_t)severity, copy};
    compat_runtime32_call(hook, args, 2);
    compat_runtime32_deallocate(copy);
}
static bool native_callback_check(int callback)
{
    uint32_t arg = (uint32_t)callback;
    uint32_t hook = callback_check_hook;
    return hook && compat_runtime32_call(hook, &arg, 1) != 0;
}

static ffi_type *abi_type(char c)
{
    switch (c) {
    case 'v': return &ffi_type_void;
    case 'b': return &ffi_type_uint8;
    case 'u': return &ffi_type_uint16;
    case 'i': return &ffi_type_uint32;
    case 'q': return &ffi_type_uint64;
    case 'f': return &ffi_type_float;
    case 'd': return &ffi_type_double;
    case 'p': case 's': case 'o': case 'W': case 'K': case 'S': case 'T': case 'E':
        return &ffi_type_pointer;
    default: return NULL;
    }
}

static int invoke_method(unsigned index, const uint32_t *args, uint64_t *result)
{
    if (index >= sizeof(steam_methods) / sizeof(steam_methods[0])) return 0;
    const struct steam_method *method = &steam_methods[index];
    uint32_t output = indirect_result(method->result) ? *args++ : 0;
    uint32_t object = args[0];
    const struct steam_proxy *proxy = NULL;
    pthread_mutex_lock(&cache_lock);
    for (unsigned i = 0; i < proxy_count; ++i) if (proxies[i].guest == object) { proxy = &proxies[i]; break; }
    pthread_mutex_unlock(&cache_lock);
    if (!proxy || index < proxy->interface->first || index >= proxy->interface->first + proxy->interface->count) return 0;
    if (trace) fprintf(stderr, "compat32: Steam %s::%s\n", proxy->interface->version, method->name);
    if (!strcmp(proxy->interface->version, "SteamMatchMakingServers002"))
        return invoke_server_method(proxy, index - proxy->interface->first, args, result);
    void **vtable = *(void ***)proxy->host;
    void *function = vtable[index - proxy->interface->first];
    if (method->result == 'D') {
        struct steam_digital value = ((struct steam_digital (*)(void *, uint64_t, uint64_t))function)(
            proxy->host, guest_u64(args + 1), guest_u64(args + 3));
        *result = (unsigned)value.state | (unsigned)value.active << 8;
        return 1;
    }
    if (method->result == 'A') {
        struct steam_analog value = ((struct steam_analog (*)(void *, uint64_t, uint64_t))function)(
            proxy->host, guest_u64(args + 1), guest_u64(args + 3));
        memcpy((void *)(uintptr_t)output, &value, sizeof(value));
        *result = output; return 1;
    }
    if (method->result == 'M') {
        struct steam_motion value = ((struct steam_motion (*)(void *, uint64_t))function)(
            proxy->host, guest_u64(args + 1));
        memcpy((void *)(uintptr_t)output, &value, sizeof(value));
        *result = output; return 1;
    }
    if (method->result == 'I') {
        struct steam_ip value = ((struct steam_ip (*)(void *))function)(proxy->host);
        memcpy((void *)(uintptr_t)output, &value, sizeof(value));
        *result = output; return 1;
    }
    if (!strcmp(method->args, "iIub")) {
        struct steam_ip ip; memcpy(&ip, args + 2, sizeof(ip));
        *result = ((uint32_t (*)(void *, int, struct steam_ip, uint16_t, bool))function)(
            proxy->host, (int)args[1], ip, (uint16_t)args[7], args[8] != 0);
        return 1;
    }
    if (!strcmp(method->args, "Iui")) {
        struct steam_ip ip; memcpy(&ip, args + 1, sizeof(ip));
        *result = ((uint32_t (*)(void *, struct steam_ip, uint16_t, int))function)(
            proxy->host, ip, (uint16_t)args[6], (int)args[7]);
        return 1;
    }
    if (!strcmp(method->args, "Lipi")) {
        struct steam_location location; memcpy(&location, args + 1, sizeof(location));
        *result = ((bool (*)(void *, struct steam_location, int, char *, int))function)(
            proxy->host, location, (int)args[4], (char *)(uintptr_t)args[5], (int)args[6]);
        return 1;
    }
    unsigned count = (unsigned)strlen(method->args);
    if (count > 14 || !abi_type(method->result)) {
        fprintf(stderr, "compat32: unsupported Steam return ABI %s::%s (%c)\n",
                proxy->interface->version, method->name, method->result);
        return 0;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (!abi_type(method->args[i])) {
            fprintf(stderr, "compat32: unsupported Steam argument ABI %s::%s argument %u (%c)\n",
                    proxy->interface->version, method->name, i + 1, method->args[i]);
            return 0;
        }
    }
    ffi_type *types[16]; void *values[16]; uint64_t storage[16] = {0};
    const char *output_strings[16] = {0};
    uint32_t string_cells[16] = {0};
    struct { const char **strings; int32_t count; } string_arrays[16] = {0};
    types[0] = &ffi_type_pointer; storage[0] = (uintptr_t)proxy->host; values[0] = &storage[0];
    unsigned word = 1;
    const char *interface_version = NULL;
    for (unsigned i = 0; i < count; ++i) {
        char code = method->args[i];
        types[i + 1] = abi_type(code);
        storage[i + 1] = args[word++];
        if (code == 'q' || code == 'd') storage[i + 1] |= (uint64_t)args[word++] << 32;
        if (code == 'W') {
            warning_hook = (uint32_t)storage[i + 1]; storage[i + 1] = warning_hook ? (uintptr_t)native_warning : 0;
        } else if (code == 'K') {
            callback_check_hook = (uint32_t)storage[i + 1]; storage[i + 1] = callback_check_hook ? (uintptr_t)native_callback_check : 0;
        } else if (code == 'E') {
            unsigned slot = !strcmp(proxy->interface->version, "SteamInput005") ? 0 : 1;
            input_event_hooks[slot] = (uint32_t)storage[i + 1];
            storage[i + 1] = storage[i + 1] ? (uintptr_t)(slot ? input_event006 : input_event005) : 0;
        } else if (code == 'S' && storage[i + 1]) {
            string_cells[i] = (uint32_t)storage[i + 1];
            storage[i + 1] = (uintptr_t)&output_strings[i];
        } else if (code == 'T' && storage[i + 1]) {
            uint32_t guest[2]; memcpy(guest, (void *)(uintptr_t)storage[i + 1], sizeof(guest));
            int32_t length = (int32_t)guest[1];
            if (length < 0 || length > 65536 || (length && !guest[0])) goto conversion_failed;
            string_arrays[i].count = length;
            if (length) {
                string_arrays[i].strings = calloc((size_t)length, sizeof(char *));
                if (!string_arrays[i].strings) goto conversion_failed;
                for (int32_t j = 0; j < length; ++j) {
                    uint32_t pointer;
                    memcpy(&pointer, (char *)(uintptr_t)guest[0] + j * 4, 4);
                    string_arrays[i].strings[j] = (const char *)(uintptr_t)pointer;
                }
            }
            storage[i + 1] = (uintptr_t)&string_arrays[i];
        }
        values[i + 1] = &storage[i + 1];
        if (method->result == 'o' && i + 1 == count) interface_version = (const char *)(uintptr_t)storage[i + 1];
    }
    if (!strcmp(method->name, "GetAPICallResult") && !plain_callback((int)args[5])) goto unsupported;
    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, count + 1, abi_type(method->result), types) != FFI_OK) goto unsupported;
    uint64_t returned = 0;
    ffi_call(&cif, FFI_FN(vtable[index - proxy->interface->first]), &returned, values);
    if (method->result != 'b' || returned) {
        for (unsigned i = 0; i < count; ++i) {
            if (!string_cells[i]) continue;
            uint32_t copy = string_copy(output_strings[i]);
            if (output_strings[i] && !copy) { returned = 0; break; }
            memcpy((void *)(uintptr_t)string_cells[i], &copy, sizeof(copy));
        }
    }
    if (method->result == 's') returned = string_copy((const char *)(uintptr_t)returned);
    else if (method->result == 'o') returned = proxy_for((void *)(uintptr_t)returned, interface_version);
    else if (method->result == 'f') { float value; memcpy(&value, &returned, 4); returned = compat_runtime32_return_float(value); }
    else if (method->result == 'd') { double value; memcpy(&value, &returned, 8); returned = compat_runtime32_return_double(value); }
    *result = returned;
    for (unsigned i = 0; i < count; ++i) free(string_arrays[i].strings);
    return 1;
conversion_failed:
    fprintf(stderr, "compat32: Steam argument conversion failed %s::%s\n", proxy->interface->version, method->name);
    *result = 0;
    for (unsigned i = 0; i < count; ++i) free(string_arrays[i].strings);
    return 1;
unsupported:
    for (unsigned i = 0; i < count; ++i) free(string_arrays[i].strings);
    return 0;
}

struct native_callback { int user, id; void *data; int size; };

/* Keep tracing outside the dispatcher so typed packed-record calls and nested
   callbacks are covered too. Only log argument words, never pointed-to ticket
   bytes, strings, or credentials. */
static int call_method(unsigned index, const uint32_t *args, uint64_t *result)
{
    if (!method_filter || !*method_filter) return invoke_method(index, args, result);
    if (index >= sizeof(steam_methods) / sizeof(steam_methods[0])) return 0;
    const struct steam_method *method = &steam_methods[index];
    unsigned first = indirect_result(method->result) ? 1 : 0;
    const struct steam_interface *interface = NULL;
    pthread_mutex_lock(&cache_lock);
    for (unsigned i = 0; i < proxy_count; ++i)
        if (proxies[i].guest == args[first]) { interface = proxies[i].interface; break; }
    pthread_mutex_unlock(&cache_lock);
    bool selected = interface && index >= interface->first && index < interface->first + interface->count &&
        (!strcmp(method_filter, "*") || strstr(interface->version, method_filter));
    if (selected) {
        flockfile(stderr);
        fprintf(stderr, "compat32: steam method %s::%s(%s) begin", interface->version, method->name, method->args);
        unsigned word = first + 1;
        for (const char *code = method->args; *code; ++code) {
            unsigned width = (*code == 'q' || *code == 'd') ? 2 : *code == 'I' ? 5 : *code == 'L' ? 3 : 1;
            fprintf(stderr, " %c=", *code);
            for (unsigned i = 0; i < width; ++i) fprintf(stderr, "%s%08x", i ? ":" : "", args[word++]);
        }
        fprintf(stderr, "\n");
        funlockfile(stderr);
    }
    int handled = invoke_method(index, args, result);
    if (selected) {
        flockfile(stderr);
        fprintf(stderr, "compat32: steam method %s::%s end handled=%d result(%c)=0x%llx",
                interface->version, method->name, handled, method->result,
                (unsigned long long)(handled ? *result : 0));
        if (handled && *result && !strcmp(method->name, "SendUserConnectAndAuthenticate_DEPRECATED")) {
            uint64_t id;
            if (lp32_crash_read(args[4], &id, sizeof(id)))
                fprintf(stderr, " steamid=%llu", (unsigned long long)id);
        }
        fprintf(stderr, "\n");
        funlockfile(stderr);
    }
    return handled;
}

static void trace_callback(int pipe, const struct native_callback *cb)
{
    if (!callback_trace) return;
    flockfile(stderr);
    fprintf(stderr, "compat32: steam callback %d user=%d size=%d pipe=%d", cb->id, cb->user, cb->size, pipe);
    if (cb->size >= 8 && (cb->id == 143 || cb->id == 201 || cb->id == 202 || cb->id == 203)) {
        uint64_t id;
        memcpy(&id, cb->data, sizeof(id));
        const char *kind = cb->id == 143 ? "ValidateAuthTicketResponse" : cb->id == 201 ? "GSClientApprove" :
            cb->id == 202 ? "GSClientDeny" : "GSClientKick";
        fprintf(stderr, " %s steamid=%llu", kind, (unsigned long long)id);
        if (cb->id != 201 && cb->size >= 12) {
            uint32_t reason;
            memcpy(&reason, (const char *)cb->data + 8, sizeof(reason));
            fprintf(stderr, " %s=%u", cb->id == 143 ? "response" : "reason", reason);
            if (cb->id == 202 && cb->size > 12)
                fprintf(stderr, " text=\"%.*s\"", cb->size - 12 < 64 ? cb->size - 12 : 64, (const char *)cb->data + 12);
        }
        unsigned owner_offset = cb->id == 143 ? 12 : 8;
        if ((cb->id == 143 || cb->id == 201) && cb->size >= (int)owner_offset + 8) {
            uint64_t owner;
            memcpy(&owner, (const char *)cb->data + owner_offset, sizeof(owner));
            fprintf(stderr, " owner=%llu", (unsigned long long)owner);
        }
    }
    fprintf(stderr, "\n");
    funlockfile(stderr);
}
// Steam dispatches callbacks on the polling thread; retain each pipe's low
// buffer until its next callback without sharing storage across threads.
static _Thread_local struct { int pipe; uint32_t data; size_t capacity; } callback_buffers[16];
int steam_bridge32_dispatch(const char *name, const uint32_t *args, uint64_t *result)
{
    if (strncmp(name, "_lp32_steam_", 12) || !client_library) return 0;
    name += 12;
    if (!strncmp(name, "method_", 7)) return call_method((unsigned)strtoul(name + 7, NULL, 10), args, result);
    if (trace) fprintf(stderr, "compat32: Steam export %s\n", name);
    // Steam's native crash handler cannot unwind the mixed i386/x86_64
    // stacks and would replace the loader's mode-recovery signal handlers.
    // Keep the compatibility crash log instead of installing Breakpad.
    if (!strncmp(name, "Breakpad_", 9)) { *result = 0; return 1; }
    void *function = dlsym(client_library, name);
    if (!function) return 0;
    if (!strcmp(name, "CreateInterface")) {
        void *host = ((void *(*)(const char *, int *))function)((const char *)(uintptr_t)args[0], (int *)(uintptr_t)args[1]);
        *result = proxy_for(host, (const char *)(uintptr_t)args[0]); return 1;
    }
    if (!strcmp(name, "Steam_ReleaseThreadLocalMemory")) { ((void (*)(int))function)(args[0]); *result = 0; return 1; }
    if (!strcmp(name, "Steam_FreeLastCallback")) { ((void (*)(int))function)(args[0]); *result = 0; return 1; }
    if (!strcmp(name, "Steam_BGetCallback")) {
        struct native_callback cb = {0};
        bool ok = false;
        for (unsigned pending = 0; pending < 1024; ++pending) {
            ok = ((bool (*)(int, struct native_callback *))function)(args[0], &cb);
            if (!ok || plain_callback(cb.id)) break;
            // The modern client queues private callbacks absent from the old
            // SDK. Only discard ones no guest has registered to receive.
            if (!callback_check_hook || native_callback_check(cb.id)) {
                fprintf(stderr, "compat32: unsupported registered Steam callback layout %d\n", cb.id);
                return 0;
            }
            if (trace) fprintf(stderr, "compat32: skipped unregistered native Steam callback %d\n", cb.id);
            void (*release)(int) = dlsym(client_library, "Steam_FreeLastCallback");
            if (!release) return 0;
            release(args[0]); ok = false;
        }
        if (ok) {
            if (cb.size < 0 || cb.size > 1024 * 1024 || (cb.size && !cb.data) || !plain_callback(cb.id)) return 0;
            unsigned slot;
            for (slot = 0; slot < 16; ++slot)
                if (!callback_buffers[slot].pipe || callback_buffers[slot].pipe == (int)args[0]) break;
            if (slot == 16) return 0;
            callback_buffers[slot].pipe = (int)args[0];
            if (callback_buffers[slot].capacity < (size_t)cb.size) {
                uint32_t resized = compat_runtime32_reallocate(callback_buffers[slot].data, cb.size);
                if (!resized) return 0;
                callback_buffers[slot].data = resized;
                callback_buffers[slot].capacity = cb.size;
            }
            uint32_t data = callback_buffers[slot].data;
            if (cb.size && !data) return 0;
            if (cb.size) memcpy((void *)(uintptr_t)data, cb.data, cb.size);
            uint32_t words[] = {(uint32_t)cb.user, (uint32_t)cb.id, data, (uint32_t)cb.size};
            memcpy((void *)(uintptr_t)args[1], words, sizeof(words));
            trace_callback((int)args[0], &cb);
        }
        *result = ok; return 1;
    }
    if (!strcmp(name, "Steam_GetAPICallResult")) {
        if (!plain_callback((int)args[5])) return 0;
        *result = ((bool (*)(int, uint64_t, void *, int, int, bool *))function)(args[0],
            (uint64_t)args[1] | (uint64_t)args[2] << 32, (void *)(uintptr_t)args[3], args[4], args[5], (bool *)(uintptr_t)args[6]);
        return 1;
    }
    return 0;
}
