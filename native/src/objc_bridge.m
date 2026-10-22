#import <CoreText/CoreText.h>
#import <Security/Security.h>
#include "objc_bridge.h"
#include "focus_policy.h"
#include "gl_core_bridge.h"
#include "hid_bridge.h"
#include "dispatch_bridge.h"
#include "blocks_bridge.h"
#include "objc_legacy_bridge.h"
#include "carbon_bridge.h"
#include "cfnetwork_bridge.h"
#include "hitch_recorder.h"
#include "arb_program_guard.h"
#include "audio_bridge.h"
#include "arb_sampler_usage.h"
#include "compat_runtime.h"
#include "controller_bridge.h"
#include "game_profile.h"
#include "name_match.h"

#define GL_SILENCE_DEPRECATION 1
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <Carbon/Carbon.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <objc/objc-sync.h>
#import <malloc/malloc.h>
#import <dispatch/dispatch.h>
#include <ffi/ffi.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>

/* These GL 3.0 access flags are absent from Apple's legacy <OpenGL/gl.h>. */
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#endif

enum {
    kProxyBase = 0x7f018000,
    kProxyLimit = 0x7f020000,
    kProxyStride = 16,
    kInlineProxyCapacity = (kProxyLimit - kProxyBase) / kProxyStride,
    /* Newer Cocoa launchers legitimately keep thousands of resource strings
       alive. Preserve the original token range and use a disjoint overflow
       range, rather than colliding with the import thunks above it. */
    kOverflowProxyBase = 0x71100000,
    kProxyCapacity = 65536,
    /*
     * NSEvent and values derived from an event (most notably -characters)
     * have event-loop lifetime.  Giving every one a permanent bridge proxy
     * exhausted kProxyCapacity after a few thousand key-repeat events.  Keep
     * them in bounded per-thread rings. In-flight callbacks pin their handles:
     * a nested resize event or another producer must not replace the message
     * whose data fields the game is still reading. This opaque token range
     * lies between the guest heap and its thread stacks.
     */
    kEventProxyBase = 0x71000000,
    kEventProxyCapacity = 64,
};

struct proxy_entry {
    uint32_t handle;
    id object;
    uint32_t cf_owners;
    bool pinned;
};

static _Thread_local bool creating_cf_object;
static _Thread_local const char *proxy_origin;
static struct proxy_entry proxies[kProxyCapacity];
/* Publish canonical handles after retaining their objects. Lookups borrow
 * the pointer, just as the locked path does; callers must hold ownership
 * across use. Clear this slot before releasing or recycling an owned entry.
 * This avoids a global mutex for every dispatch semaphore poll. */
static uintptr_t published_proxy_objects[kProxyCapacity];
static void publish_proxy(uint32_t slot) {
    uint32_t canonical=slot<kInlineProxyCapacity?kProxyBase+slot*kProxyStride:
        kOverflowProxyBase+(slot-kInlineProxyCapacity)*kProxyStride;
    uintptr_t object=proxies[slot].handle==canonical ?
        (uintptr_t)proxies[slot].object:0;
    __atomic_store_n(&published_proxy_objects[slot],object,__ATOMIC_RELEASE);
}
static uint32_t proxy_count;
static pthread_mutex_t proxy_mutex = PTHREAD_MUTEX_INITIALIZER;
struct guest_autorelease_entry { uint32_t token; struct guest_autorelease_entry *next; };
struct guest_autorelease_scope {
    struct guest_autorelease_entry *objects;
    struct guest_autorelease_scope *parent;
};
static _Thread_local struct guest_autorelease_scope *guest_autorelease_scope;
struct event_pool {
    struct proxy_entry entries[kEventProxyCapacity];
    uint32_t count, cursor;
    NSEvent *current;
};
static struct event_pool event_pools[1024];
static uint32_t next_event_pool;
static _Thread_local uint32_t event_pool_index=UINT32_MAX;
static struct event_pool *event_pool(void) {
    if(event_pool_index==UINT32_MAX)event_pool_index=__atomic_fetch_add(&next_event_pool,1,__ATOMIC_RELAXED);
    if(event_pool_index>=1024)abort();
    return &event_pools[event_pool_index];
}
#define event_proxies (event_pool()->entries)
#define event_proxy_count (event_pool()->count)
#define current_proxy_event (event_pool()->current)
static bool logged_proxy_exhaustion;
static uint64_t objc_bridge_swap_count;
@class OpenGLView;
static OpenGLView *presenting_view;
static NSScreen *preferred_game_screen(void);
static void place_test_window(NSWindow *window)
{
    if (!getenv("LP32_TEST_DISPLAY") || window.sheetParent) return;
    NSRect frame=window.frame, screen=preferred_game_screen().visibleFrame;
    if(frame.size.width<=0 || frame.size.height<=0 || NSContainsRect(screen,frame))return;
    frame.size.width=MIN(frame.size.width,screen.size.width);
    frame.size.height=MIN(frame.size.height,screen.size.height);
    frame.origin=NSMakePoint(screen.origin.x+(screen.size.width-frame.size.width)/2,
                             screen.origin.y+(screen.size.height-frame.size.height)/2);
    [window setFrame:frame display:NO];
}

/* NSFastEnumerationState contains native pointers and NSUInteger fields;
 * never allow AppKit to write that 64-byte host record into a 32-byte guest
 * stack record. Keep host iteration state until the enumeration finishes. */
struct enumeration_context {
    uint32_t address, mutation;
    id collection;
    NSFastEnumerationState state;
};
static struct enumeration_context enumerations[128];
static void refresh_enumeration_mutations(void)
{
    for (unsigned i=0;i<128;++i) {
        struct enumeration_context *e=&enumerations[i];
        if(e->collection && e->mutation && e->state.mutationsPtr)
            *(uint32_t *)(uintptr_t)e->mutation=(uint32_t)*e->state.mutationsPtr;
    }
}
static bool enumerate_guest(id collection,const uint32_t *args,uint64_t *result);


/* Section ranges of the loaded guest image (class-name refs, CF constants). */
#define bridge_image (compat_runtime32_image())
static GLuint trace_vertex_program;
static GLuint trace_fragment_program;

struct gl_frame_diagnostics {
    uint64_t draw_calls;
    uint64_t clear_calls;
    uint64_t framebuffer_binds;
    uint64_t framebuffer_attachments;
    uint64_t framebuffer_blits;
    uint64_t texture_binds;
    uint64_t texture_uploads;
    uint64_t program_binds;
    uint64_t parameter_uploads;
};

static struct gl_frame_diagnostics gl_frame_diagnostics;
static volatile sig_atomic_t pending_gl_trace_arm;
static FILE *gl_trace_file;
static bool gl_trace_output_initialized;
static char gl_program_dump_directory[PATH_MAX];

static void arm_pending_gl_trace(void);
static void trace_gl_frame_boundary(void);

/*
 * Finder launches discard stderr.  Rendering diagnostics therefore need a
 * file of their own; trying to redirect a live process with a debugger proved
 * both fragile and intrusive.  This file is deliberately separate from the
 * async-signal-safe crash log maintained by game_loader.c.
 */
static void initialize_gl_trace_output(void)
{
    if (gl_trace_output_initialized) return;
    gl_trace_output_initialized = true;

    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    char directory[PATH_MAX];
    int length = snprintf(directory, sizeof(directory),
                          "%s/Library/Logs/%s", home,
                          lp32_profile()->log_directory);
    if (length < 0 || (size_t)length >= sizeof(directory)) return;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return;

    char path[PATH_MAX];
    length = snprintf(path, sizeof(path), "%s/gl-render.log", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) return;
    /* Preserve earlier clean/bad captures across diagnostic self-tests and
     * relaunches; each session and armed segment already has a marker. */
    gl_trace_file = fopen(path, "a");
    if (gl_trace_file) {
        setvbuf(gl_trace_file, NULL, _IOLBF, 0);
        fprintf(gl_trace_file,
                "compat32: GL diagnostic session pid=%ld\n", (long)getpid());
    }

    length = snprintf(gl_program_dump_directory,
                      sizeof(gl_program_dump_directory),
                      "%s/gl-programs", directory);
    if (length < 0 || (size_t)length >= sizeof(gl_program_dump_directory)) {
        gl_program_dump_directory[0] = '\0';
    }
}

static void gl_trace_printf(const char *format, ...)
{
    initialize_gl_trace_output();
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    if (gl_trace_file) {
        vfprintf(gl_trace_file, format, copy);
        fflush(gl_trace_file);
    }
    va_end(copy);
}

/*
 * A few Cg-generated material programs always execute optional texture
 * lookups even when the engine leaves that sampler bound to texture object 0.
 * The trace can identify Cg-generated programs which execute optional texture
 * lookups while the corresponding unit is bound to object 0.  An off-screen
 * probe confirms that Apple's current driver returns the specified opaque-
 * black incomplete-texture value, so explicit substitution is diagnostic and
 * opt-in rather than a normal compatibility repair.
 */
enum {
    kSamplerProgramCapacity = 4096,
    kSamplerFallbackShareCapacity = 32,
    kSamplerFallbackUnitCapacity = 32,
};

struct sampler_program_entry {
    CGLShareGroupObj share_group;
    GLuint program;
    struct arb_sampler_usage usage;
    unsigned char state; /* 0 empty, 1 occupied, 2 tombstone */
};

struct sampler_fallback_share_entry {
    CGLShareGroupObj share_group;
    GLuint texture_2d;
    bool occupied;
};

struct sampler_fallback_restore {
    GLint active_texture;
    uint32_t changed_2d;
};

static struct sampler_program_entry
    sampler_programs[kSamplerProgramCapacity];
static struct sampler_fallback_share_entry
    sampler_fallback_shares[kSamplerFallbackShareCapacity];
static pthread_mutex_t sampler_program_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sampler_fallback_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int sampler_fallback_reports;

static size_t sampler_program_slot(CGLShareGroupObj share_group,
                                   GLuint program)
{
    return ((size_t)program * 2654435761u +
            ((uintptr_t)share_group >> 4)) % kSamplerProgramCapacity;
}

static void remember_fragment_program_samplers(GLuint program,
                                               const void *source,
                                               size_t source_size)
{
    if (!program || !source) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    struct arb_sampler_usage usage;
    arb_sampler_usage_parse(source, source_size, &usage);

    pthread_mutex_lock(&sampler_program_mutex);
    size_t start = sampler_program_slot(share_group, program);
    size_t tombstone = SIZE_MAX;
    for (size_t probe = 0; probe < kSamplerProgramCapacity; ++probe) {
        size_t slot = (start + probe) % kSamplerProgramCapacity;
        struct sampler_program_entry *entry = &sampler_programs[slot];
        if (entry->state == 2 && tombstone == SIZE_MAX) tombstone = slot;
        if (entry->state == 1 && entry->share_group == share_group &&
            entry->program == program) {
            entry->usage = usage;
            pthread_mutex_unlock(&sampler_program_mutex);
            return;
        }
        if (entry->state != 0) continue;
        if (tombstone != SIZE_MAX) entry = &sampler_programs[tombstone];
        entry->share_group = share_group;
        entry->program = program;
        entry->usage = usage;
        entry->state = 1;
        pthread_mutex_unlock(&sampler_program_mutex);
        return;
    }
    pthread_mutex_unlock(&sampler_program_mutex);
    gl_trace_printf("compat32: fragment sampler table exhausted\n");
}

static bool fragment_program_sampler_usage(
    GLuint program, struct arb_sampler_usage *usage)
{
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    bool found = false;
    memset(usage, 0, sizeof(*usage));
    pthread_mutex_lock(&sampler_program_mutex);
    size_t start = sampler_program_slot(share_group, program);
    for (size_t probe = 0; probe < kSamplerProgramCapacity; ++probe) {
        struct sampler_program_entry *entry =
            &sampler_programs[(start + probe) % kSamplerProgramCapacity];
        if (entry->state == 0) break;
        if (entry->state == 1 && entry->share_group == share_group &&
            entry->program == program) {
            *usage = entry->usage;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&sampler_program_mutex);
    return found;
}

static void forget_fragment_program_samplers(GLsizei count,
                                             const GLuint *programs)
{
    if (count <= 0 || !programs) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    pthread_mutex_lock(&sampler_program_mutex);
    for (size_t slot = 0; slot < kSamplerProgramCapacity; ++slot) {
        struct sampler_program_entry *entry = &sampler_programs[slot];
        if (entry->state != 1 || entry->share_group != share_group) continue;
        for (GLsizei index = 0; index < count; ++index) {
            if (entry->program == programs[index]) {
                memset(&entry->usage, 0, sizeof(entry->usage));
                entry->state = 2;
                break;
            }
        }
    }
    pthread_mutex_unlock(&sampler_program_mutex);
}

static GLuint sampler_fallback_texture_2d(void)
{
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    pthread_mutex_lock(&sampler_fallback_mutex);
    struct sampler_fallback_share_entry *available = NULL;
    for (size_t index = 0; index < kSamplerFallbackShareCapacity; ++index) {
        struct sampler_fallback_share_entry *entry =
            &sampler_fallback_shares[index];
        if (entry->occupied && entry->share_group == share_group) {
            GLuint texture = entry->texture_2d;
            pthread_mutex_unlock(&sampler_fallback_mutex);
            return texture;
        }
        if (!entry->occupied && !available) available = entry;
    }
    if (!available) {
        pthread_mutex_unlock(&sampler_fallback_mutex);
        return 0;
    }

    GLint previous_texture = 0;
    const GLubyte black_opaque[4] = {0, 0, 0, 255};
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, black_opaque);
        glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
        available->share_group = share_group;
        available->texture_2d = texture;
        available->occupied = true;
    }
    pthread_mutex_unlock(&sampler_fallback_mutex);
    return texture;
}

static bool repair_unbound_fragment_samplers(
    struct sampler_fallback_restore *restore)
{
    memset(restore, 0, sizeof(*restore));
    static int repair_enabled = -1;
    if (repair_enabled < 0) {
        repair_enabled = getenv("LP32_ENABLE_EXPLICIT_UNBOUND_SAMPLER") != NULL &&
                         getenv("LP32_DISABLE_UNBOUND_SAMPLER_REPAIR") == NULL;
    }
    if (!repair_enabled || !glIsEnabled(GL_FRAGMENT_PROGRAM_ARB)) {
        return false;
    }

    GLint program = (GLint)trace_fragment_program;
    struct arb_sampler_usage usage;
    if (program <= 0 ||
        !fragment_program_sampler_usage((GLuint)program, &usage) ||
        !usage.texture_2d) {
        return false;
    }

    GLint maximum_units = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maximum_units);
    if (maximum_units < 0) maximum_units = 0;
    if (maximum_units > kSamplerFallbackUnitCapacity) {
        maximum_units = kSamplerFallbackUnitCapacity;
    }
    glGetIntegerv(GL_ACTIVE_TEXTURE, &restore->active_texture);
    for (GLint unit = 0; unit < maximum_units; ++unit) {
        uint32_t bit = UINT32_C(1) << unit;
        if (!(usage.texture_2d & bit)) continue;
        glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
        GLint binding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        if (binding) continue;
        GLuint fallback = sampler_fallback_texture_2d();
        if (!fallback) continue;
        glBindTexture(GL_TEXTURE_2D, fallback);
        restore->changed_2d |= bit;
        if (sampler_fallback_reports < 32) {
            gl_trace_printf(
                "compat32: repaired unbound ARB sampler fp=%d unit=%d "
                "target=2D fallback=%u swap=%llu\n",
                program, unit, fallback,
                (unsigned long long)objc_bridge_swap_count);
            ++sampler_fallback_reports;
        }
    }
    glActiveTexture((GLenum)restore->active_texture);
    return restore->changed_2d != 0;
}

static void restore_unbound_fragment_samplers(
    const struct sampler_fallback_restore *restore)
{
    if (!restore->changed_2d) return;
    for (unsigned int unit = 0; unit < kSamplerFallbackUnitCapacity; ++unit) {
        if (!(restore->changed_2d & (UINT32_C(1) << unit))) continue;
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture((GLenum)restore->active_texture);
}

/* Borrowed strings (GL driver strings, selectors and NSString C strings)
   are not freed by the guest. Intern copies by content so repeated queries
   retain stable pointers without growing the low heap. Keep distinct values
   alive even after their host object or GL context is destroyed. */
static uint32_t intern_guest_cstring(const char *value)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct guest_cstring_entry {
        uint32_t guest;
        struct guest_cstring_entry *next;
    } *strings;
    if (!value) return 0;
    pthread_mutex_lock(&lock);
    for (struct guest_cstring_entry *entry = strings; entry; entry = entry->next) {
        if (strcmp((const char *)(uintptr_t)entry->guest, value) == 0) {
            uint32_t guest = entry->guest;
            pthread_mutex_unlock(&lock);
            return guest;
        }
    }
    struct guest_cstring_entry *entry = malloc(sizeof(*entry));
    uint32_t guest = entry ? compat_runtime32_copy_cstring(value) : 0;
    if (guest) {
        entry->guest = guest;
        entry->next = strings;
        strings = entry;
    } else {
        free(entry);
    }
    pthread_mutex_unlock(&lock);
    return guest;
}

static id object_for_receiver(uint32_t receiver);

static bool proxy_object_is_retained(id object)
{
    return object && ![object isKindOfClass:[NSAutoreleasePool class]];
}

static void reset_event_proxies(void)
{
    for (uint32_t index = 0; index < event_proxy_count; ++index) {
        if (proxy_object_is_retained(event_proxies[index].object)) {
            [event_proxies[index].object release];
        }
        event_proxies[index].handle = 0;
        event_proxies[index].object = nil;
    }
    event_proxy_count = 0;
    current_proxy_event = nil;
}

static bool is_event_proxy_object(id object)
{
    if (!object) return false;
    for (uint32_t index = 0; index < event_proxy_count; ++index) {
        if (event_proxies[index].object == object) return true;
    }
    return false;
}

static uint32_t event_proxy_for_object(id object)
{
    if (!object) return 0;
    if ([object isKindOfClass:[NSEvent class]] && object != current_proxy_event) {
        current_proxy_event = (NSEvent *)object;
    }
    for (uint32_t index = 0; index < event_proxy_count; ++index) {
        if (event_proxies[index].object == object) {
            return event_proxies[index].handle;
        }
    }
    struct event_pool *pool=event_pool();
    uint32_t slot=pool->count;
    if(slot==kEventProxyCapacity) {
        bool found=false;
        for(uint32_t i=0;i<kEventProxyCapacity;++i) {
            slot=(pool->cursor+i)%kEventProxyCapacity;
            if(!pool->entries[slot].cf_owners){found=true;break;}
        }
        if(!found){fprintf(stderr,"compat32: all event proxies are in use\n");return 0;}
    } else ++pool->count;
    id previous=pool->entries[slot].object;
    uint32_t handle=kEventProxyBase+(event_pool_index*kEventProxyCapacity+slot)*kProxyStride;
    pool->entries[slot]=(struct proxy_entry){handle,object,0,false};
    pool->cursor=(slot+1)%kEventProxyCapacity;
    if(proxy_object_is_retained(object))[object retain];
    if(proxy_object_is_retained(previous))[previous release];
    return handle;
}

void objc_bridge32_pin_event(uint32_t token, int pin) {
    if(token<kEventProxyBase || token>=kOverflowProxyBase || (token-kEventProxyBase)%kProxyStride)return;
    uint32_t index=(token-kEventProxyBase)/kProxyStride;
    struct proxy_entry *entry=&event_pools[index/kEventProxyCapacity].entries[index%kEventProxyCapacity];
    if(entry->handle!=token || !entry->object)return;
    if(pin)++entry->cf_owners;else if(entry->cf_owners)--entry->cf_owners;
}

static uint32_t proxy_for_object(id object)
{
    if (!object) return 0;
    uint32_t legacy = objc_legacy32_token(object);
    if (legacy) return legacy;
    if ([object isKindOfClass:[NSEvent class]]) {
        return event_proxy_for_object(object);
    }
    pthread_mutex_lock(&proxy_mutex);
    for (uint32_t index = 0; index < proxy_count; ++index) {
        if (proxies[index].object == object) {
            if (creating_cf_object) ++proxies[index].cf_owners;
            else proxies[index].pinned = true;
            publish_proxy(index);
            uint32_t handle = proxies[index].handle;
            pthread_mutex_unlock(&proxy_mutex);
            return handle;
        }
    }
    uint32_t slot = 0;
    while (slot < proxy_count && proxies[slot].object) ++slot;
    if (slot >= kProxyCapacity) {
        if (!logged_proxy_exhaustion) {
            fprintf(stderr,
                    "compat32: persistent Objective-C proxy pool exhausted "
                    "at swap %llu for %s (capacity=%u origin=%s)\n",
                    (unsigned long long)objc_bridge_swap_count,
                    class_getName(object_getClass(object)),kProxyCapacity,
                    proxy_origin?proxy_origin:"bridge");
            logged_proxy_exhaustion = true;
        }
        pthread_mutex_unlock(&proxy_mutex);
        return 0;
    }
    uint32_t handle = slot < kInlineProxyCapacity ? kProxyBase + slot * kProxyStride :
        kOverflowProxyBase + (slot - kInlineProxyCapacity) * kProxyStride;
    proxies[slot] = (struct proxy_entry){handle, object,
                                       creating_cf_object ? 1u : 0u,
                                       !creating_cf_object};
    static int trace_allocations=-1;
    if(trace_allocations<0)trace_allocations=getenv("LP32_TRACE_PROXY_ALLOC")!=NULL;
    if(trace_allocations)fprintf(stderr,
        "compat32: proxy allocation swap=%llu slot=%u class=%s origin=%s owned=%d\n",
        (unsigned long long)objc_bridge_swap_count,slot,class_getName(object_getClass(object)),
        proxy_origin?proxy_origin:"bridge",creating_cf_object);
    if (proxy_object_is_retained(object)) [object retain];
    publish_proxy(slot);
    if (slot == proxy_count) ++proxy_count;
    pthread_mutex_unlock(&proxy_mutex);
    return handle;
}

/* Native per-import pools are shorter lived than guest pools. Hold fresh
 * autoreleased values until the guest drains its corresponding scope. */
static void guest_autorelease_push(void)
{
    struct guest_autorelease_scope *scope=calloc(1,sizeof(*scope));
    if(!scope)abort();
    scope->parent=guest_autorelease_scope;guest_autorelease_scope=scope;
}
static void guest_autorelease_add(uint32_t token)
{
    if(!guest_autorelease_scope || !token)return;
    struct guest_autorelease_entry *entry=malloc(sizeof(*entry));
    if(!entry)abort();
    *entry=(struct guest_autorelease_entry){token,guest_autorelease_scope->objects};
    guest_autorelease_scope->objects=entry;
}
static void guest_autorelease_pop(void)
{
    struct guest_autorelease_scope *scope=guest_autorelease_scope;
    if(!scope)return;
    guest_autorelease_scope=scope->parent;
    while(scope->objects){
        struct guest_autorelease_entry *entry=scope->objects;
        scope->objects=entry->next;
        uint64_t ignored;objc_bridge32_dispatch("_CFRelease",&entry->token,&ignored);
        free(entry);
    }
    free(scope);
}
static uint32_t proxy_for_autoreleased_object(id object)
{
    if(!guest_autorelease_scope)return proxy_for_object(object);
    bool previous=creating_cf_object;creating_cf_object=true;
    uint32_t token=proxy_for_object(object);creating_cf_object=previous;
    guest_autorelease_add(token);return token;
}
static bool proxy_uses_guest_ownership(uint32_t token)
{
    bool managed=false;
    pthread_mutex_lock(&proxy_mutex);
    for(uint32_t i=0;i<proxy_count;++i)if(proxies[i].handle==token){
        managed=proxies[i].object && (!proxies[i].pinned || proxies[i].cf_owners);break;
    }
    pthread_mutex_unlock(&proxy_mutex);return managed;
}

/* Cocoa method families carry ownership independently of their concrete
 * NSString class (tagged strings and heap strings must behave identically).
 * Keep a bridge reference, transfer native +1 results into guest ownership,
 * and schedule +0 string results on the guest's autorelease scope. */
static bool objc_method_family(const char *selector,const char *family)
{
    while(*selector=='_')++selector;
    size_t n=strlen(family);
    return !strncmp(selector,family,n) && !(selector[n]>='a' && selector[n]<='z');
}
static uint32_t proxy_for_owned_object(id object)
{
    bool previous=creating_cf_object;creating_cf_object=true;
    uint32_t token=proxy_for_object(object);creating_cf_object=previous;
    return token;
}
uint32_t objc_bridge32_guest_owned_object(void *object) {
    return proxy_for_owned_object((id)object);
}
@interface LP32BorrowedProxy : NSObject { @public uint32_t token; id value; }
@end
@implementation LP32BorrowedProxy
- (void)dealloc {
    uint64_t ignored;objc_bridge32_dispatch("_CFRelease",&token,&ignored);[super dealloc];
}
@end
uint32_t objc_bridge32_borrowed_object(void *raw,void *object,unsigned slot)
{
    static char keys[3];id owner=(id)raw;
    if(!owner || !object || slot>=3)return 0;
    @synchronized(owner) {
        LP32BorrowedProxy *entry=objc_getAssociatedObject(owner,&keys[slot]);
        if(!entry || entry->value!=(id)object) {
            entry=[[[LP32BorrowedProxy alloc] init] autorelease];
            entry->value=(id)object;entry->token=proxy_for_owned_object((id)object);
            objc_setAssociatedObject(owner,&keys[slot],entry,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        return entry->token;
    }
}
static bool managed_string_init(id receiver,uint32_t token,const char *selector)
{
    return objc_method_family(selector,"init") &&
        [receiver isKindOfClass:[NSString class]] && proxy_uses_guest_ownership(token);
}
static uint32_t proxy_for_string_result(id receiver,uint32_t receiver_token,
                                        const char *selector,id value,bool initialized)
{
    if(initialized) {
        /* init may replace a class-cluster placeholder and consume its +1.
         * The invocation receives a separate retain, leaving our mapping safe
         * until we transfer the guest's ownership to the initialized object. */
        if(value==receiver) { [value release];return receiver_token; }
        uint32_t token=proxy_for_owned_object(value);
        [value release];
        uint64_t ignored;objc_bridge32_dispatch("_CFRelease",&receiver_token,&ignored);
        return token;
    }
    if(objc_method_family(selector,"alloc") || objc_method_family(selector,"new") ||
       objc_method_family(selector,"copy") || objc_method_family(selector,"mutableCopy")) {
        uint32_t token=proxy_for_owned_object(value);
        [value release];
        return token;
    }
    if(objc_method_family(selector,"init"))return proxy_for_object(value);
    /* A self-returning getter does not make a managed value immortal. */
    if(value==receiver && proxy_uses_guest_ownership(receiver_token))return receiver_token;
    return proxy_for_autoreleased_object(value);
}

static bool enumerate_guest(id collection,const uint32_t *args,uint64_t *result)
{
    uint32_t *guest=(void *)(uintptr_t)args[2];
    uint32_t *buffer=(void *)(uintptr_t)args[3];
    NSUInteger capacity=args[4];
    if(!guest || !buffer || capacity>4096)return false;
    struct enumeration_context *e=NULL;
    for(unsigned i=0;i<128;++i)if(enumerations[i].address==args[2]){e=&enumerations[i];break;}
    if(!e)for(unsigned i=0;i<128;++i)if(!enumerations[i].collection){e=&enumerations[i];break;}
    if(!e)return false;
    if(!guest[0] || e->collection!=collection){
        [e->collection release];if(e->mutation)compat_runtime32_deallocate(e->mutation);
        memset(e,0,sizeof(*e));e->address=args[2];e->collection=[collection retain];
        e->mutation=compat_runtime32_allocate(4,1);
    }
    id *objects=calloc(capacity?capacity:1,sizeof(id));if(!objects)return false;
    NSUInteger count=[collection countByEnumeratingWithState:&e->state objects:objects count:capacity];
    if(count>capacity){free(objects);return false;}
    for(NSUInteger i=0;i<count;++i)buffer[i]=proxy_for_object(e->state.itemsPtr[i]);
    guest[0]=count?1:0;guest[1]=args[3];guest[2]=e->mutation;
    refresh_enumeration_mutations();free(objects);*result=count;
    if(!count){[e->collection release];compat_runtime32_deallocate(e->mutation);memset(e,0,sizeof(*e));}
    return true;
}

static id object_for_argument(uint32_t value);

/* CFAllocatorContext uses nine 32-bit words in the guest. Host CF calls
 * receive widened sizes and bridge each supplied callback back to i386. */
struct allocator_context32 { uint32_t version, info, retain, release, describe,
    allocate, reallocate, deallocate, preferred; };
static const void *allocator_retain32(const void *raw) {
    struct allocator_context32 *c = malloc(sizeof(*c));
    if (!c) return NULL;
    memcpy(c, raw, sizeof(*c));
    if (c->retain) c->info = compat_runtime32_call(c->retain, &c->info, 1);
    return c;
}
static void allocator_release32(const void *raw) {
    const struct allocator_context32 *c = raw;
    if (c->release) compat_runtime32_call(c->release, &c->info, 1);
    free((void *)c);
}
static CFStringRef allocator_describe32(const void *raw) {
    const struct allocator_context32 *c = raw;
    if (!c->describe) return NULL;
    id description = object_for_argument(compat_runtime32_call(c->describe, &c->info, 1));
    return description ? CFRetain((CFStringRef)description) : NULL;
}
static void *allocator_allocate32(CFIndex size, CFOptionFlags flags, void *raw) {
    struct allocator_context32 *c = raw;
    if (!c->allocate || size < 0 || size > INT32_MAX) return NULL;
    uint32_t a[] = {(uint32_t)size, (uint32_t)flags, c->info};
    return (void *)(uintptr_t)compat_runtime32_call(c->allocate, a, 3);
}
static void *allocator_reallocate32(void *pointer, CFIndex size, CFOptionFlags flags, void *raw) {
    struct allocator_context32 *c = raw;
    if (!c->reallocate || (uintptr_t)pointer > UINT32_MAX || size < 0 || size > INT32_MAX) return NULL;
    uint32_t a[] = {(uint32_t)(uintptr_t)pointer, (uint32_t)size, (uint32_t)flags, c->info};
    return (void *)(uintptr_t)compat_runtime32_call(c->reallocate, a, 4);
}
static void allocator_deallocate32(void *pointer, void *raw) {
    struct allocator_context32 *c = raw;
    if (c->deallocate && (uintptr_t)pointer <= UINT32_MAX) {
        uint32_t a[] = {(uint32_t)(uintptr_t)pointer, c->info};
        compat_runtime32_call(c->deallocate, a, 2);
    }
}
static CFIndex allocator_preferred32(CFIndex size, CFOptionFlags flags, void *raw) {
    struct allocator_context32 *c = raw;
    if (!c->preferred || size < 0 || size > INT32_MAX) return size;
    uint32_t a[] = {(uint32_t)size, (uint32_t)flags, c->info};
    return (int32_t)compat_runtime32_call(c->preferred, a, 3);
}

static uint32_t proxy_for_returned_object(id receiver, id object)
{
    return is_event_proxy_object(receiver) ? event_proxy_for_object(object) :
                                             proxy_for_object(object);
}

/* CFDataGetBytePtr borrows its buffer from the data object. Tie the low-address
   copy to that object too; Marvel reads its cached data on every draw. */
@interface LP32DataBuffer : NSObject {
@public
    uint32_t guest;
    NSUInteger capacity;
}
@end
@implementation LP32DataBuffer
- (void)dealloc
{
    if (guest) compat_runtime32_deallocate(guest);
    [super dealloc];
}
@end

static uint32_t guest_bytes_for_data(NSData *data)
{
    static char buffer_key;
    if (!data) return 0;
    @synchronized(data) {
        LP32DataBuffer *buffer = objc_getAssociatedObject(data, &buffer_key);
        if (!buffer) {
            buffer = [[[LP32DataBuffer alloc] init] autorelease];
            objc_setAssociatedObject(data, &buffer_key, buffer,
                                      OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        NSUInteger length = [data length];
        NSUInteger needed = length ? length : 1;
        if (buffer->capacity < needed) {
            uint32_t guest = compat_runtime32_reallocate(buffer->guest, needed);
            if (!guest) return 0;
            buffer->guest = guest;
            buffer->capacity = needed;
        }
        /* Refresh in place for NSMutableData as well. */
        if (length) memcpy((void *)(uintptr_t)buffer->guest, [data bytes], length);
        return buffer->guest;
    }
}

static char borrowed_bytes_keys[4];
uint32_t objc_bridge32_borrowed_bytes(void *raw,const void *bytes,size_t length,unsigned slot)
{
    id owner=(id)raw;
    if(!owner || !bytes || slot>=4 || length>UINT32_MAX)return 0;
    @synchronized(owner) {
        LP32DataBuffer *buffer=objc_getAssociatedObject(owner,&borrowed_bytes_keys[slot]);
        if(!buffer){buffer=[[LP32DataBuffer alloc] init];
            objc_setAssociatedObject(owner,&borrowed_bytes_keys[slot],buffer,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [buffer release];}
        if(buffer->capacity<(length?length:1)) {
            uint32_t p=compat_runtime32_reallocate(buffer->guest,length?length:1);
            if(!p)return 0;buffer->guest=p;buffer->capacity=length?length:1;
        }
        if(length)memcpy((void *)(uintptr_t)buffer->guest,bytes,length);
        return buffer->guest;
    }
}
void objc_bridge32_commit_borrowed_bytes(void *raw,void *bytes,size_t length,unsigned slot)
{
    id owner=(id)raw;if(!owner||!bytes||slot>=4)return;
    @synchronized(owner) {
        LP32DataBuffer *buffer=objc_getAssociatedObject(owner,&borrowed_bytes_keys[slot]);
        if(buffer && length<=buffer->capacity)memcpy(bytes,(void *)(uintptr_t)buffer->guest,length);
    }
}
void objc_bridge32_release_borrowed_bytes(void *raw,unsigned slot)
{
    id owner=(id)raw;if(!owner||slot>=4)return;
    @synchronized(owner) {
        objc_setAssociatedObject(owner,&borrowed_bytes_keys[slot],nil,
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
}

/* On the x86_64 host, exhausted register banks make va_arg consume this
 * widened overflow area. i386 arguments always arrive as stack words. */
static CFStringRef format_cf_string32(CFStringRef format, const uint32_t *guest) {
    const char *text = [(NSString *)format UTF8String];
    if (!text) return NULL;
    uint64_t values[128]; size_t count = 0;
    for (const char *p = text; *p; ++p) {
        if (*p != '%') continue;
        if (*++p == '%') continue;
        while (*p && strchr("-+ #0'", *p)) ++p;
        for (unsigned field = 0; field < 2; ++field) {
            if (*p == '*') {
                if (count == 128) return NULL;
                values[count++] = (int32_t)*guest++; ++p;
            } else while (*p >= '0' && *p <= '9') ++p;
            if (*p == '$') return NULL; /* Positional formats need an index map. */
            if (*p != '.') break;
            ++p;
        }
        unsigned longs = 0;
        if (*p == 'l') { ++longs; ++p; if (*p == 'l') { ++longs; ++p; } }
        else if (*p == 'h') { ++p; if (*p == 'h') ++p; }
        else if (*p == 'q' || *p == 'j') { longs = 2; ++p; }
        else if (*p == 'z' || *p == 't') { longs = 1; ++p; }
        if (!*p || count == 128 || *p == 'n' || *p == 'L') return NULL;
        uint64_t value;
        if (strchr("aAeEfFgG", *p) || longs == 2) { memcpy(&value, guest, 8); guest += 2; }
        else if (*p == '@') value = (uintptr_t)object_for_argument(*guest++);
        else if (strchr("diD", *p)) value = (int64_t)(int32_t)*guest++;
        else value = *guest++;
        values[count++] = value;
    }
    struct { unsigned gp_offset, fp_offset; void *overflow, *registers; } state = {48, 304, values, NULL};
    va_list args;
    _Static_assert(sizeof(args) == sizeof(state), "x86_64 va_list layout");
    memcpy(args, &state, sizeof(state));
    return CFStringCreateWithFormatAndArguments(NULL, NULL, format, args);
}

/* A private test bundle must remain silent even when Launch Services starts
 * it without the shell environment. Run before loading any guest code. */
void objc_bridge32_initialize_test_mode(void)
{
    @autoreleasepool {
        id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"LP32SilentTest"];
        if ([value isKindOfClass:[NSNumber class]] && [value boolValue]) {
            if (setenv("LP32_MUTE_AUDIO", "1", 1) ||
                setenv("LP32_BACKGROUND_TEST", "1", 1)) abort();
            fputs("compat32: silent test bundle: audio muted before guest initialization\n", stderr);
        }
    }
}

void *objc_bridge32_host_object(uint32_t token) { return object_for_argument(token); }
/* Opaque pointer handles represent an address, not ownership of the pointed-to
 * resource. Intern their NSValue boxes: Cocoa allocates a fresh box on every
 * valueWithPointer:, which otherwise consumes a persistent proxy per query.
 * Address reuse is safe here because no context state or resource is cached.
 * The table cannot fill before the shared proxy table (at most 50% load). */
static struct { void *pointer; uint32_t token; } pointer_proxies[kProxyCapacity * 2];
static pthread_mutex_t pointer_proxy_mutex = PTHREAD_MUTEX_INITIALIZER;
uint32_t objc_bridge32_guest_pointer(void *pointer)
{
    if (!pointer) return 0;
    uintptr_t hash = (uintptr_t)pointer;
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    size_t slot = hash & (kProxyCapacity * 2 - 1);
    pthread_mutex_lock(&pointer_proxy_mutex);
    while (pointer_proxies[slot].pointer && pointer_proxies[slot].pointer != pointer)
        slot = (slot + 1) & (kProxyCapacity * 2 - 1);
    uint32_t token = pointer_proxies[slot].token;
    if (!token) {
        NSValue *box = [[NSValue alloc] initWithBytes:&pointer objCType:@encode(void *)];
        bool previous = creating_cf_object;
        creating_cf_object = false; /* The box is borrowed; it owns no resource. */
        token = proxy_for_object(box);
        creating_cf_object = previous;
        [box release];
        if (token) {
            pointer_proxies[slot].pointer = pointer;
            pointer_proxies[slot].token = token;
        }
    }
    pthread_mutex_unlock(&pointer_proxy_mutex);
    return token;
}
uint32_t objc_bridge32_guest_object(void *object) { return proxy_for_object((id)object); }

static void *event_proxy_test_worker(void *unused) {
    (void)unused;
    @autoreleasepool {
        NSEvent *event=[NSEvent otherEventWithType:NSEventTypeApplicationDefined
            location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
            context:nil subtype:2 data1:456 data2:789];
        return (void *)(uintptr_t)event_proxy_for_object(event);
    }
}

int objc_bridge32_run_proxy_self_test(void)
{
    @autoreleasepool {
        NSString *persistent = [[[NSString alloc]
            initWithUTF8String:"persistent-proxy"] autorelease];
        uint32_t persistent_handle = proxy_for_object(persistent);
        if (!persistent_handle || object_for_receiver(persistent_handle) != persistent) {
            fputs("compat32: Objective-C proxy self-test could not create "
                  "persistent handle\n", stderr);
            return -1;
        }
        /* A pinned lookup must not need the mutable ownership-map lock. */
        dispatch_semaphore_t looked_up=dispatch_semaphore_create(0);
        __block id concurrent=nil;
        pthread_mutex_lock(&proxy_mutex);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{
            concurrent=object_for_receiver(persistent_handle);
            dispatch_semaphore_signal(looked_up);
        });
        long blocked=dispatch_semaphore_wait(looked_up,dispatch_time(0,1000000000));
        pthread_mutex_unlock(&proxy_mutex);
        if(blocked)dispatch_semaphore_wait(looked_up,DISPATCH_TIME_FOREVER);
        dispatch_release(looked_up);
        if(blocked || concurrent!=persistent)return -1;

        /* Owned semaphores use the same read path, including slot reuse.
           Hold a guest reference while another thread resolves the token. */
        for(unsigned pass=0;pass<3;++pass) {
            uint32_t zero=0;uint64_t value=0;
            if(!objc_bridge32_dispatch("_dispatch_semaphore_create",&zero,&value))return -1;
            uint32_t token=(uint32_t)value;
            id expected=object_for_receiver(token);
            looked_up=dispatch_semaphore_create(0);concurrent=nil;
            pthread_mutex_lock(&proxy_mutex);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{
                concurrent=object_for_receiver(token);dispatch_semaphore_signal(looked_up);
            });
            blocked=dispatch_semaphore_wait(looked_up,dispatch_time(0,1000000000));
            pthread_mutex_unlock(&proxy_mutex);
            if(blocked)dispatch_semaphore_wait(looked_up,DISPATCH_TIME_FOREVER);
            dispatch_release(looked_up);
            if(blocked || !expected || concurrent!=expected)return -1;
            objc_bridge32_dispatch("_dispatch_release",&token,&value);
            if(object_for_receiver(token))return -1;
        }

        uint32_t before_cf_count = proxy_count;
        uint32_t text_buffer = compat_runtime32_allocate(96, 1);
        if (!text_buffer) return -1;
        for (unsigned i = 0; i < 10000; ++i) {
            int length = snprintf((char *)(uintptr_t)text_buffer, 96, "temporary CF object %u", i);
            uint32_t create[] = {0, text_buffer, (uint32_t)length, kCFStringEncodingUTF8, 0};
            uint64_t value = 0;
            if (!objc_bridge32_dispatch("_CFStringCreateWithBytes", create, &value) || !value) return -1;
            uint32_t token = (uint32_t)value;
            objc_bridge32_dispatch("_CFRetain", &token, &value);
            objc_bridge32_dispatch("_CFRelease", &token, &value);
            objc_bridge32_dispatch("_CFStringGetLength", &token, &value);
            if (value != (uint32_t)length) return -1;
            objc_bridge32_dispatch("_CFRelease", &token, &value);
            for (uint32_t j = 0; j < proxy_count; ++j)
                if (proxies[j].handle == token && proxies[j].object) return -1;
        }
        compat_runtime32_deallocate(text_buffer);
        if (proxy_count > before_cf_count + 1) return -1;
        guest_autorelease_push();
        uint32_t deadline=proxy_for_autoreleased_object([NSDate dateWithTimeIntervalSince1970:12345]);
        uint64_t ignored;
        objc_bridge32_dispatch("_CFRetain",&deadline,&ignored);
        for(unsigned i=0;i<100000;++i) {
            @autoreleasepool {
                guest_autorelease_push();
                uint32_t date=proxy_for_autoreleased_object([NSDate dateWithTimeIntervalSince1970:20000+i]);
                if(!date || !object_for_receiver(date))return -1;
                guest_autorelease_pop();
            }
        }
        guest_autorelease_pop();
        if([(NSDate *)object_for_receiver(deadline) timeIntervalSince1970]!=12345 ||
           proxy_count>before_cf_count+2)return -1;
        objc_bridge32_dispatch("_CFRelease",&deadline,&ignored);
        /* Exercise actual Cocoa selectors with long, heap-backed strings;
         * short tagged strings can conceal leaks by reusing their identity. */
        uint32_t string_class=proxy_for_object((id)[NSString class]);
        const char *string_methods[]={"alloc","initWithUTF8String:","release",
            "stringWithUTF8String:","retain","copy","mutableCopy","appendString:","description","autorelease",
            "initWithFormat:","stringWithFormat:"};
        uint32_t string_selectors[12];
        for(unsigned i=0;i<12;++i)string_selectors[i]=compat_runtime32_copy_cstring(string_methods[i]);
        uint32_t string_format=proxy_for_object(@"%s");
        uint32_t string_bytes=compat_runtime32_allocate(128,1);
        uint32_t string_slots=proxy_count;
        /* Copying an already pinned, borrowed string still returns guest +1. */
        uint32_t pinned_copy_args[]={persistent_handle,string_selectors[5]};uint64_t pinned_copy;
        if(!objc_bridge32_dispatch("_objc_msgSend",pinned_copy_args,&pinned_copy) || !pinned_copy)return -1;
        pinned_copy_args[0]=(uint32_t)pinned_copy;pinned_copy_args[1]=string_selectors[2];
        objc_bridge32_dispatch("_objc_msgSend",pinned_copy_args,&ignored);
        if(proxy_uses_guest_ownership(persistent_handle) || object_for_receiver(persistent_handle)!=persistent)return -1;
        /* A failing class-cluster initializer consumes the allocated object. */
        uint32_t failed_init[]={string_class,string_selectors[0],string_bytes};uint64_t failed_result;
        objc_bridge32_dispatch("_objc_msgSend",failed_init,&failed_result);
        failed_init[0]=(uint32_t)failed_result;failed_init[1]=string_selectors[1];
        memcpy((void *)(uintptr_t)string_bytes,"\xff",2);
        if(!objc_bridge32_dispatch("_objc_msgSend",failed_init,&failed_result) || failed_result)return -1;
        for(unsigned i=0;i<100000;++i) {
            @autoreleasepool {
                snprintf((char *)(uintptr_t)string_bytes,128,"heap-backed temporary string with unique contents %u",i);
                uint32_t a[4]={string_class,string_selectors[0],0,0};uint64_t result;
                if(!objc_bridge32_dispatch("_objc_msgSend",a,&result) || !result)return -1;
                a[0]=(uint32_t)result;a[1]=string_selectors[1];a[2]=string_bytes;
                if(i%2){a[1]=string_selectors[10];a[2]=string_format;a[3]=string_bytes;}
                if(!objc_bridge32_dispatch("_objc_msgSend",a,&result) || !result)return -1;
                uint32_t owned=(uint32_t)result;
                a[0]=owned;a[1]=string_selectors[2];
                objc_bridge32_dispatch("_objc_msgSend",a,&result);
                guest_autorelease_push();
                a[0]=string_class;a[1]=string_selectors[3];a[2]=string_bytes;
                if(i%2){a[1]=string_selectors[11];a[2]=string_format;a[3]=string_bytes;}
                if(!objc_bridge32_dispatch("_objc_msgSend",a,&result) || !result)return -1;
                uint32_t temporary=(uint32_t)result;
                a[0]=temporary;a[1]=string_selectors[4];objc_bridge32_dispatch("_objc_msgSend",a,&result);
                a[1]=string_selectors[5];objc_bridge32_dispatch("_objc_msgSend",a,&result);
                uint32_t copy=(uint32_t)result;
                a[1]=string_selectors[6];objc_bridge32_dispatch("_objc_msgSend",a,&result);
                uint32_t mutable=(uint32_t)result;
                a[0]=mutable;a[1]=string_selectors[7];a[2]=temporary;objc_bridge32_dispatch("_objc_msgSend",a,&result);
                a[0]=temporary;a[1]=string_selectors[8];objc_bridge32_dispatch("_objc_msgSend",a,&result);
                if(result!=temporary)return -1;
                guest_autorelease_pop();
                NSString *value=object_for_receiver(temporary);
                if(![value isEqualToString:[NSString stringWithUTF8String:(char *)(uintptr_t)string_bytes]] ||
                   [(NSString *)object_for_receiver(mutable) length]!=value.length*2)return -1;
                a[0]=copy;a[1]=string_selectors[2];objc_bridge32_dispatch("_objc_msgSend",a,&result);
                a[0]=mutable;objc_bridge32_dispatch("_objc_msgSend",a,&result);
                guest_autorelease_push();
                a[0]=temporary;a[1]=string_selectors[9];objc_bridge32_dispatch("_objc_msgSend",a,&result);
                guest_autorelease_pop();
                if(proxy_count>string_slots+4) {
                    fprintf(stderr,"compat32: string proxy lifetime regression at iteration %u: slots=%u baseline=%u\n",i,proxy_count,string_slots);
                    return -1;
                }
            }
        }
        compat_runtime32_deallocate(string_bytes);
        for(unsigned i=0;i<12;++i)compat_runtime32_deallocate(string_selectors[i]);
        uint32_t word = compat_runtime32_copy_cstring("world");
        uint32_t format_args[] = {persistent_handle, word, (uint32_t)-123, 0x89abcdef, 0x12345678, 0, 0x40040000};
        CFStringRef formatted = format_cf_string32(CFSTR("%@ %s %ld %llx %.2f"), format_args);
        BOOL format_ok = formatted && [(NSString *)formatted isEqualToString:@"persistent-proxy world -123 1234567889abcdef 2.50"];
        if (formatted) CFRelease(formatted);
        compat_runtime32_deallocate(word);
        if (!format_ok) return -1;

        /* Exercise the real NSInvocation dispatch boundary.  Without the
           per-call autorelease pool this loop retains about 6 MiB of
           NSInvocation objects and return buffers in the outer pool. */
        uint32_t selector = compat_runtime32_copy_cstring("length");
        if (!selector) {
            fputs("compat32: Objective-C proxy self-test could not allocate "
                  "selector\n", stderr);
            return -1;
        }
        const uint32_t message_arguments[] = {persistent_handle, selector};
        malloc_statistics_t before = {0};
        malloc_statistics_t after = {0};
        malloc_zone_statistics(malloc_default_zone(), &before);
        for (uint32_t iteration = 0; iteration < 10000; ++iteration) {
            uint64_t result = 0;
            if (!objc_bridge32_dispatch("_objc_msgSend", message_arguments,
                                        &result) ||
                result != [persistent length]) {
                fprintf(stderr,
                        "compat32: Objective-C invocation self-test failed "
                        "at message %u\n", iteration);
                compat_runtime32_deallocate(selector);
                return -1;
            }
        }
        malloc_zone_statistics(malloc_default_zone(), &after);
        compat_runtime32_deallocate(selector);
        size_t invocation_growth = after.size_in_use > before.size_in_use ?
            after.size_in_use - before.size_in_use : 0;
        if (invocation_growth > 2 * 1024 * 1024) {
            fprintf(stderr,
                    "compat32: Objective-C invocation self-test retained "
                    "%zu bytes\n", invocation_growth);
            return -1;
        }

        uint32_t utf8_selector = compat_runtime32_copy_cstring("UTF8String");
        const uint32_t string_args[] = {persistent_handle, utf8_selector};
        uint64_t first_string = 0;
        if (!utf8_selector || !objc_bridge32_dispatch("_objc_msgSend", string_args,
                                                       &first_string) ||
            !first_string) return -1;
        for (unsigned query = 0; query < 10000; ++query) {
            uint64_t string = 0;
            if (!objc_bridge32_dispatch("_objc_msgSend", string_args, &string) ||
                string != first_string ||
                strcmp((const char *)(uintptr_t)string, "persistent-proxy")) {
                fputs("compat32: NSString C-string lifetime self-test failed\n", stderr);
                return -1;
            }
        }
        compat_runtime32_deallocate(utf8_selector);

        NSMutableData *data = [NSMutableData dataWithBytes:"data" length:4];
        const uint32_t data_args[] = {proxy_for_object(data)};
        uint64_t first_bytes = 0;
        if (!objc_bridge32_dispatch("_CFDataGetBytePtr", data_args, &first_bytes) ||
            !first_bytes) return -1;
        for (unsigned query = 0; query < 10000; ++query) {
            uint64_t bytes = 0;
            if (!objc_bridge32_dispatch("_CFDataGetBytePtr", data_args, &bytes) ||
                bytes != first_bytes || memcmp((void *)(uintptr_t)bytes, "data", 4)) {
                fputs("compat32: CFData byte-pointer lifetime self-test failed\n", stderr);
                return -1;
            }
        }
        [data appendBytes:" appended" length:9];
        uint64_t grown_bytes = 0;
        if (!objc_bridge32_dispatch("_CFDataGetBytePtr", data_args, &grown_bytes) ||
            !grown_bytes || memcmp((void *)(uintptr_t)grown_bytes, "data appended", 13)) {
            fputs("compat32: mutable CFData refresh self-test failed\n", stderr);
            return -1;
        }

        uint32_t persistent_count = proxy_count;
        for (uint32_t iteration = 0; iteration < 10000; ++iteration) {
            @autoreleasepool {
                NSString *characters = (iteration & 1) ? @"w" : @"s";
                NSEvent *event = [NSEvent
                    keyEventWithType:(iteration & 1) ? NSEventTypeKeyDown :
                                                          NSEventTypeKeyUp
                             location:NSZeroPoint
                        modifierFlags:0
                            timestamp:(NSTimeInterval)iteration / 1000.0
                         windowNumber:0
                              context:nil
                           characters:characters
          charactersIgnoringModifiers:characters
                            isARepeat:NO
                              keyCode:(iteration & 1) ? 13 : 1];
                uint32_t event_handle = proxy_for_object(event);
                uint32_t characters_handle =
                    proxy_for_returned_object(event, [event characters]);
                if (!event_handle || !characters_handle ||
                    object_for_receiver(event_handle) != event ||
                    ![object_for_receiver(characters_handle)
                        isEqualToString:characters]) {
                    fprintf(stderr,
                            "compat32: Objective-C proxy self-test failed at "
                            "event %u\n", iteration);
                    return -1;
                }
            }
        }

        if (proxy_count != persistent_count ||
            object_for_receiver(persistent_handle) != persistent) {
            fprintf(stderr,
                    "compat32: Objective-C proxy self-test leaked event "
                    "objects into persistent table (persistent=%u event=%u)\n",
                    proxy_count, event_proxy_count);
            return -1;
        }
        /* A Cocoa resource catalog may hold more objects than the original
           inline range. Overflow handles must remain distinct and reversible. */
        for (unsigned index = 0; index < 4096; ++index) {
            NSNumber *number = [NSNumber numberWithUnsignedInt:index + 100000];
            uint32_t handle = proxy_for_object(number);
            if (!handle || object_for_receiver(handle) != number ||
                !((handle >= kProxyBase && handle < kProxyLimit) ||
                  (handle >= kOverflowProxyBase && handle < kOverflowProxyBase + (kProxyCapacity-kInlineProxyCapacity)*kProxyStride))) return -1;
        }
        if (object_for_receiver(persistent_handle) != persistent) return -1;
        NSEvent *message=[NSEvent otherEventWithType:NSEventTypeApplicationDefined
            location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
            context:nil subtype:1 data1:123 data2:321];
        uint32_t message_token=event_proxy_for_object(message);
        objc_bridge32_pin_event(message_token,1);
        for(unsigned i=0;i<256;++i) {
            NSEvent *nested=[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
                context:nil subtype:2 data1:i data2:0];
            if(!event_proxy_for_object(nested))return -1;
        }
        if([(NSEvent *)object_for_receiver(message_token) data2]!=321)return -1;
        objc_bridge32_pin_event(message_token,0);
        pthread_t producer;
        if(pthread_create(&producer,NULL,event_proxy_test_worker,NULL))return -1;
        void *other=NULL;
        if(pthread_join(producer,&other) || (uintptr_t)other==message_token ||
           [(NSEvent *)object_for_receiver(message_token) data1]!=123 ||
           [(NSEvent *)object_for_receiver((uint32_t)(uintptr_t)other) data1]!=456)return -1;
        reset_event_proxies();
        printf("Objective-C proxy self-test: PASS "
               "(10000 CF ownership cycles; 100000 scoped dates; 100000 owned/autoreleased string cycles; mixed CF varargs; 10000 messages, +%zu bytes; stable CFData; 10000 events, persistent=%u, "
               "event=%u)\n", invocation_growth, proxy_count,
               event_proxy_count);
        return 0;
    }
}

static uint32_t register_proxy_with_handle(id object, uint32_t handle)
{
    if (!object || !handle) return 0;
    pthread_mutex_lock(&proxy_mutex);
    for (uint32_t index = 0; index < proxy_count; ++index) {
        if (proxies[index].object == object || proxies[index].handle == handle) {
            proxies[index].object = object;
            proxies[index].handle = handle;
            proxies[index].pinned = true;
            publish_proxy(index);
            pthread_mutex_unlock(&proxy_mutex);
            return handle;
        }
    }
    if (proxy_count >= kProxyCapacity) {
        if (!logged_proxy_exhaustion) {
            fprintf(stderr,
                    "compat32: persistent Objective-C proxy pool exhausted "
                    "while registering guest handle 0x%08x\n", handle);
            logged_proxy_exhaustion = true;
        }
        pthread_mutex_unlock(&proxy_mutex);
        return 0;
    }
    proxies[proxy_count].handle = handle;
    proxies[proxy_count].object = object;
    proxies[proxy_count].pinned = true;
    if (proxy_object_is_retained(object)) [object retain];
    publish_proxy(proxy_count);
    ++proxy_count;
    pthread_mutex_unlock(&proxy_mutex);
    return handle;
}

static id object_for_receiver(uint32_t receiver)
{
    if (!receiver) return nil;
    /* Native/event handles occupy reserved ranges outside guest objects.
       Resolve them before consulting the legacy instance identity map: that
       unrelated map takes a contended mutex in Complete Saga's render loop. */
    if(receiver>=kEventProxyBase && receiver<kOverflowProxyBase) {
        uint32_t offset=(receiver-kEventProxyBase)/kProxyStride;
        struct proxy_entry *entry=&event_pools[offset/kEventProxyCapacity].entries[offset%kEventProxyCapacity];
        return entry->handle==receiver?entry->object:nil;
    }
    uint32_t slot = UINT32_MAX;
    if (receiver >= kProxyBase && receiver < kProxyLimit &&
        (receiver - kProxyBase) % kProxyStride == 0)
        slot = (receiver - kProxyBase) / kProxyStride;
    else if (receiver >= kOverflowProxyBase &&
             receiver < kOverflowProxyBase + (kProxyCapacity-kInlineProxyCapacity)*kProxyStride &&
             (receiver - kOverflowProxyBase) % kProxyStride == 0)
        slot = kInlineProxyCapacity + (receiver - kOverflowProxyBase) / kProxyStride;
    if (slot != UINT32_MAX) {
        id published=(id)__atomic_load_n(&published_proxy_objects[slot],__ATOMIC_ACQUIRE);
        if(published)return published;
        pthread_mutex_lock(&proxy_mutex);
        if (slot < proxy_count && proxies[slot].handle == receiver) {
            id object = proxies[slot].object;
            pthread_mutex_unlock(&proxy_mutex);
            return object;
        }
        pthread_mutex_unlock(&proxy_mutex);
    }
    id legacy = objc_legacy32_object(receiver);
    if (legacy) return legacy;
    pthread_mutex_lock(&proxy_mutex);
    for (uint32_t index = 0; index < proxy_count; ++index) {
        if (proxies[index].handle == receiver) {
            id object = proxies[index].object;
            pthread_mutex_unlock(&proxy_mutex);
            return object;
        }
    }
    pthread_mutex_unlock(&proxy_mutex);
    /* Old 32-bit CF constant strings are four-word records in __DATA. */
    if (!bridge_image) return nil;
    if (receiver >= bridge_image->cfstring_start &&
        receiver < bridge_image->cfstring_end) {
        const uint32_t *constant = (const void *)(uintptr_t)receiver;
        uint32_t bytes_address = constant[2];
        uint32_t length = constant[3];
        if (bytes_address >= UINT32_C(0x00001000) &&
            bytes_address < bridge_image->max_address && length < UINT32_C(0x100000)) {
            NSString *string = [[[NSString alloc]
                initWithBytes:(const void *)(uintptr_t)bytes_address
                       length:length
                     encoding:NSUTF8StringEncoding] autorelease];
            if (!string) {
                string = [[[NSString alloc]
                    initWithBytes:(const void *)(uintptr_t)bytes_address
                           length:length
                         encoding:NSISOLatin1StringEncoding] autorelease];
            }
            return string;
        }
    }

    /* Legacy class references contain the class name rather than a Class. */
    if (receiver >= bridge_image->cstring_start &&
        receiver < bridge_image->cstring_end) {
        const char *class_name = (const char *)(uintptr_t)receiver;
        return (id)objc_legacy32_class(class_name);
    }
    return nil;
}

static const char *skip_type_qualifiers(const char *type)
{
    while (*type && strchr("rnNoORV", *type)) ++type;
    return type;
}

static id object_for_argument(uint32_t value)
{
    return object_for_receiver(value);
}

/* An i386 @? argument is a guest block layout, not an Objective-C object
 * token. AppKit copies and later invokes a native 64-bit block. Keep the
 * guest captures alive until the monitor is removed. Local monitors run on
 * the main event thread, including removal from inside their own callback. */
@interface LP32GuestBlock : NSObject { @public uint32_t guestBlock; }
@end
@implementation LP32GuestBlock
- (void)dealloc { blocks_bridge32_release(guestBlock); [super dealloc]; }
@end
struct cf_notification32 { uint32_t observer, callback; };
static void cf_notification_callback(CFNotificationCenterRef center, void *raw,
                                     CFNotificationName name, const void *object,
                                     CFDictionaryRef user_info)
{
    const struct cf_notification32 *notification = raw;
    uint32_t args[] = {objc_bridge32_guest_object((void *)center),
                       notification->observer,
                       objc_bridge32_guest_object((void *)name),
                       objc_bridge32_guest_object((void *)object),
                       objc_bridge32_guest_object((void *)user_info)};
    compat_runtime32_call(notification->callback, args, 5);
}
static char event_monitor_owner_key;
static id add_guest_event_monitor(NSEventMask mask, uint32_t block)
{
    LP32GuestBlock *owner=[[LP32GuestBlock alloc] init];
    owner->guestBlock=blocks_bridge32_copy(block);
    if (!owner->guestBlock) { [owner release]; return nil; }
    id monitor=[NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *event) {
        uint32_t copied=blocks_bridge32_copy(owner->guestBlock);
        if (!copied) return event;
        uint32_t token=event_proxy_for_object(event);
        objc_bridge32_pin_event(token,1);
        uint32_t args[]={copied,token};
        uint32_t invoke=((const uint32_t *)(uintptr_t)copied)[3];
        uint32_t returned=compat_runtime32_call(invoke,args,2);
        NSEvent *result=object_for_argument(returned);
        objc_bridge32_pin_event(token,0);
        blocks_bridge32_release(copied);
        return result;
    }];
    if (monitor) objc_setAssociatedObject(monitor,&event_monitor_owner_key,owner,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [owner release];
    return monitor;
}
static void remove_guest_event_monitor(id monitor)
{
    [NSEvent removeMonitor:monitor];
    LP32GuestBlock *owner=objc_getAssociatedObject(monitor,&event_monitor_owner_key);
    if (owner) {
        uint32_t block=owner->guestBlock; owner->guestBlock=0;
        blocks_bridge32_release(block);
        objc_setAssociatedObject(monitor,&event_monitor_owner_key,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
}

int objc_bridge32_run_event_monitor_self_test(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        unsigned char *code=mmap((void *)0x76000000,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                                MAP_PRIVATE|MAP_ANON,-1,0);
        if(code==MAP_FAILED || (uintptr_t)code>UINT32_MAX)return -1;
        /* i386 block callbacks increment their copied capture, then either
           return the event or consume it. Exercise AppKit's real observer. */
        const unsigned char identity[]={0x8b,0x44,0x24,0x04,0xff,0x40,0x14,0x8b,0x44,0x24,0x08,0xc3};
        const unsigned char consume[]={0x8b,0x44,0x24,0x04,0xff,0x40,0x14,0x31,0xc0,0xc3};
        memcpy(code,identity,sizeof(identity));memcpy(code+32,consume,sizeof(consume));
        uint32_t storage=compat_runtime32_allocate(40,1);
        uint32_t *block=(void *)(uintptr_t)storage;
        block[4]=storage+32;block[9]=24; /* descriptor: reserved, size */
        uint32_t selector=compat_runtime32_copy_cstring("addLocalMonitorForEventsMatchingMask:handler:");
        uint32_t remove=compat_runtime32_copy_cstring("removeMonitor:");
        NSEvent *event=[NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint
            modifierFlags:0 timestamp:0 windowNumber:0 context:nil characters:@"\r"
            charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36];
        for(unsigned pass=0;pass<2;++pass) {
            block[3]=(uint32_t)(uintptr_t)(code+32*pass);block[5]=0;
            uint32_t a[]={proxy_for_object([NSEvent class]),selector,NSEventMaskKeyUp,storage};uint64_t result=0;
            if(!objc_bridge32_dispatch("_objc_msgSend",a,&result) || !result)return -1;
            id monitor=object_for_argument((uint32_t)result);
            LP32GuestBlock *owner=[objc_getAssociatedObject(monitor,&event_monitor_owner_key) retain];
            if(!owner || owner->guestBlock==storage)return -1;
            [NSApp sendEvent:event];[NSApp sendEvent:event];
            if(((uint32_t *)(uintptr_t)owner->guestBlock)[5]!=2 || block[5])return -1;
            uint32_t r[]={a[0],remove,(uint32_t)result};
            if(!objc_bridge32_dispatch("_objc_msgSend",r,&result) || owner->guestBlock)return -1;
            [NSApp sendEvent:event];
            [owner release];
        }
        compat_runtime32_deallocate(selector);compat_runtime32_deallocate(remove);
        compat_runtime32_deallocate(storage);munmap(code,4096);
        puts("event-monitor-selftest: PASS (AppKit key-up delivery, copied i386 captures, returned/consumed events, removal)");
        return 0;
    }
}

static volatile uint32_t app_event_test_received;
static volatile uint32_t app_event_test_last;
static void app_event_test_send_event(id self, SEL command, NSEvent *event)
{
    if (event.type == NSEventTypeApplicationDefined && event.subtype == 0x5a17) {
        __atomic_store_n(&app_event_test_last, (uint32_t)event.data1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&app_event_test_received, 1, __ATOMIC_ACQ_REL);
        return;
    }
    struct objc_super super = {self, class_getSuperclass(object_getClass(self))};
    ((void (*)(struct objc_super *, SEL, NSEvent *))objc_msgSendSuper)(&super, command, event);
}

static uint32_t app_event_test_post(uint32_t app, uint32_t selector, long value)
{
    NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
        location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
        context:nil subtype:0x5a17 data1:value data2:0];
    uint32_t a[] = {app, selector, proxy_for_object(event), 0};
    uint64_t result = 0;
    return objc_bridge32_dispatch("_objc_msgSend", a, &result);
}

/* Game threads post application-defined wake-ups to NSApp and wait for the
   main thread's -sendEvent:.  They must arrive promptly and in order, while
   a guest's own -nextEventMatchingMask: loop still dequeues them itself. */
int objc_bridge32_run_app_event_delivery_self_test(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        Class base = object_getClass(NSApp);
        Class probe = objc_allocateClassPair(base, "LP32AppEventProbe", 0);
        if (!probe) return -1;
        class_addMethod(probe, @selector(sendEvent:), (IMP)app_event_test_send_event, "v@:@");
        objc_registerClassPair(probe);
        object_setClass(NSApp, probe);
        uint32_t app = proxy_for_object(NSApp);
        uint32_t post = compat_runtime32_copy_cstring("postEvent:atStart:");
        enum { rounds = 120 };
        static double latency[rounds];
        __block bool ordered = true, worker_done = false;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
            for (int i = 0; i < rounds; ++i) {
                uint32_t before = __atomic_load_n(&app_event_test_received, __ATOMIC_ACQUIRE);
                CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
                if (!app_event_test_post(app, post, i + 1)) { ordered = false; break; }
                while (__atomic_load_n(&app_event_test_received, __ATOMIC_ACQUIRE) == before &&
                       CFAbsoluteTimeGetCurrent() - start < 1.0) {}
                latency[i] = CFAbsoluteTimeGetCurrent() - start;
                if (__atomic_load_n(&app_event_test_last, __ATOMIC_ACQUIRE) != (uint32_t)(i + 1))
                    ordered = false;
            }
            __atomic_store_n(&worker_done, true, __ATOMIC_RELEASE);
        });
        CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 30;
        while (!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) && CFAbsoluteTimeGetCurrent() < deadline) {
            @autoreleasepool {
                NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                    untilDate:[NSDate dateWithTimeIntervalSinceNow:0.25]
                    inMode:NSDefaultRunLoopMode dequeue:YES];
                if (event) [NSApp sendEvent:event];
            }
        }
        if (!worker_done || !ordered || app_event_test_received != rounds) {
            fprintf(stderr, "app-event-selftest: received %u/%d ordered=%d\n",
                    app_event_test_received, rounds, ordered);
            return -1;
        }
        double sorted[rounds];
        memcpy(sorted, latency, sizeof(sorted));
        for (int i = 1; i < rounds; ++i)
            for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; --j) {
                double t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
            }
        double median = sorted[rounds / 2] * 1e3, p90 = sorted[rounds * 9 / 10] * 1e3;

        /* A guest pumping events itself must still receive the wake-up. */
        __block bool posted = false;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
            usleep(20000);
            posted = app_event_test_post(app, post, 0x7777);
        });
        uint32_t next = compat_runtime32_copy_cstring("nextEventMatchingMask:untilDate:inMode:dequeue:");
        uint32_t pump[] = {app, next, (uint32_t)NSEventMaskApplicationDefined,
            proxy_for_object([NSDate dateWithTimeIntervalSinceNow:2]),
            proxy_for_object(NSDefaultRunLoopMode), 1};
        uint64_t pumped = 0;
        uint32_t received_before = app_event_test_received;
        bool pump_ok = objc_bridge32_dispatch("_objc_msgSend", pump, &pumped) && pumped;
        NSEvent *guest_event = pump_ok ? object_for_argument((uint32_t)pumped) : nil;
        pump_ok = pump_ok && posted && guest_event.subtype == 0x5a17 &&
                  guest_event.data1 == 0x7777 && app_event_test_received == received_before;
        object_setClass(NSApp, base);
        compat_runtime32_deallocate(post);
        compat_runtime32_deallocate(next);
        printf("app-event-selftest: median %.2f ms p90 %.2f ms over %d round trips; guest pump %s\n",
               median, p90, rounds, pump_ok ? "dequeued" : "FAILED");
        if (!pump_ok || median > 4.0) return -1;
        puts("app-event-selftest: PASS");
        return 0;
    }
}

static NSBundle *legacy_game_bundle(void)
{
    static NSBundle *bundle;
    if (!bundle) {
        bundle = [[NSBundle mainBundle] retain];
    }
    return bundle;
}

static size_t guest_words_for_type(const char *type)
{
    type = skip_type_qualifiers(type);
    switch (*type) {
        /* NSInteger/NSUInteger are 32-bit in the guest even though the host
           method signature encodes them as q/Q.  Explicit LongLong selectors
           are handled separately by the invocation bridge. */
        case 'q': case 'Q': return 1;
        case 'd': return 2;
        case '{': {
            if(!strncmp(type,@encode(CMTimeRange),strlen(@encode(CMTimeRange))))return sizeof(CMTimeRange)/4;
            if(!strncmp(type,@encode(CMTime),strlen(@encode(CMTime))))return sizeof(CMTime)/4;
            /* The legacy ABI uses 32-bit CGFloat, so count encoded floats. */
            size_t words = 0;
            for (const char *cursor = type; *cursor; ++cursor) {
                if (*cursor == 'f' || *cursor == 'i' || *cursor == 'I') ++words;
                if (*cursor == '}') break;
            }
            return words ? words : 1;
        }
        default: return 1;
    }
}

static ffi_type *ffi_objc_type(const char *type)
{
    type = skip_type_qualifiers(type);
    static ffi_type *pair_fields[] = {&ffi_type_double, &ffi_type_double, NULL};
    static ffi_type *rect_fields[] = {&ffi_type_double, &ffi_type_double, &ffi_type_double, &ffi_type_double, NULL};
    static ffi_type pair = {0,0,FFI_TYPE_STRUCT,pair_fields};
    static ffi_type rect = {0,0,FFI_TYPE_STRUCT,rect_fields};
    switch (*type) {
        case 'v': return &ffi_type_void;
        case '@': case '#': case ':': case '*': case '^': return &ffi_type_pointer;
        case 'c': return &ffi_type_sint8; case 'C': case 'B': return &ffi_type_uint8;
        case 's': return &ffi_type_sint16; case 'S': return &ffi_type_uint16;
        case 'i': return &ffi_type_sint32; case 'I': return &ffi_type_uint32;
        case 'l': return &ffi_type_slong; case 'L': return &ffi_type_ulong;
        case 'q': return &ffi_type_sint64; case 'Q': return &ffi_type_uint64;
        case 'f': return &ffi_type_float; case 'd': return &ffi_type_double;
        case '{':
            if (strstr(type,"Rect=")) return &rect;
            if (strstr(type,"Point=") || strstr(type,"Size=")) return &pair;
    }
    return NULL;
}
static bool invoke_using_imp(NSInvocation *invocation, IMP imp)
{
    NSMethodSignature *signature=invocation.methodSignature;
    NSUInteger count=signature.numberOfArguments;
    if(count>64 || signature.methodReturnLength>64)return false;
    ffi_type *types[64], *ret=ffi_objc_type(signature.methodReturnType);
    union {long double align;unsigned char bytes[64];} values[64], result;
    void *args[64]; memset(values,0,sizeof(values));memset(&result,0,sizeof(result));
    if(!ret)return false;
    for(NSUInteger i=0;i<count;++i){
        types[i]=ffi_objc_type([signature getArgumentTypeAtIndex:i]);if(!types[i])return false;
        [invocation getArgument:values[i].bytes atIndex:i];args[i]=values[i].bytes;
    }
    ffi_cif cif;if(ffi_prep_cif(&cif,FFI_DEFAULT_ABI,(unsigned)count,ret,types)!=FFI_OK)return false;
    ffi_call(&cif,FFI_FN(imp),result.bytes,args);
    if(signature.methodReturnLength)[invocation setReturnValue:result.bytes];
    return true;
}

static bool invoke_simple_message(id receiver, const char *selector_name,
                                  const uint32_t *guest_arguments,
                                  uint64_t *guest_result, Method super_method, void *guest_struct_result)
{
    SEL selector = sel_registerName(selector_name);
    if (![receiver respondsToSelector:selector]) return false;
    NSMethodSignature *signature = super_method ?
        [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(super_method)] :
        [receiver methodSignatureForSelector:selector];
    if (!signature) return false;

    NSInvocation *invocation = [NSInvocation invocationWithMethodSignature:signature];
    [invocation setTarget:receiver];
    [invocation setSelector:selector];

    size_t word = 2;
    id object_outputs[64]={nil};
    uint32_t *guest_object_outputs[64]={NULL};
    if(signature.numberOfArguments>64)return false;
    bool explicit_long_long = strstr(selector_name, "LongLong") != NULL;
    for (NSUInteger index = 2; index < [signature numberOfArguments]; ++index) {
        const char *type = skip_type_qualifiers([signature getArgumentTypeAtIndex:index]);
        switch (*type) {
            case '@': case '#': {
                id value = object_for_argument(guest_arguments[word]);
                [invocation setArgument:&value atIndex:index];
                break;
            }
            case ':': {
                const char *name = (const char *)(uintptr_t)guest_arguments[word];
                SEL value = name ? sel_registerName(name) : (SEL)0;
                [invocation setArgument:&value atIndex:index];
                break;
            }
            case '*': {
                char *value = (char *)(uintptr_t)guest_arguments[word];
                [invocation setArgument:&value atIndex:index];
                break;
            }
            case '^': {
                uint32_t token=guest_arguments[word];
                if(type[1]=='@' && index+1==signature.numberOfArguments &&
                   (strstr(selector_name,"error:") || strstr(selector_name,"Error:"))) {
                    guest_object_outputs[index]=(void *)(uintptr_t)token;
                    id *pointer=token?&object_outputs[index]:NULL;
                    [invocation setArgument:&pointer atIndex:index];
                    break;
                }
                id object=object_for_argument(token);
                void *value=object ? ([object isKindOfClass:[NSValue class]] ?
                    [(NSValue *)object pointerValue] : (void *)object) : (void *)(uintptr_t)token;
                [invocation setArgument:&value atIndex:index];
                break;
            }
            case 'c': { int8_t value = (int8_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'C': case 'B': { uint8_t value = (uint8_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 's': { int16_t value = (int16_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'S': { uint16_t value = (uint16_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'i': case 'l': { int32_t value = (int32_t)guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'I': case 'L': { uint32_t value = guest_arguments[word]; [invocation setArgument:&value atIndex:index]; break; }
            case 'q': { int64_t value = (int32_t)guest_arguments[word]; if (explicit_long_long) memcpy(&value, guest_arguments + word, sizeof(value)); [invocation setArgument:&value atIndex:index]; break; }
            case 'Q': { uint64_t value = guest_arguments[word]; if (explicit_long_long) memcpy(&value, guest_arguments + word, sizeof(value)); [invocation setArgument:&value atIndex:index]; break; }
            case 'f': { float value; memcpy(&value, guest_arguments + word, sizeof(value)); [invocation setArgument:&value atIndex:index]; break; }
            case 'd': { double value; memcpy(&value, guest_arguments + word, sizeof(value)); [invocation setArgument:&value atIndex:index]; break; }
            case '{': {
                /* i386 AppKit geometry is float-based and passed by value on
                   the stack; widen to the host's CGFloat layout. */
                const float *fields = (const float *)(guest_arguments + word);
                if(!strncmp(type,@encode(CMTimeRange),strlen(@encode(CMTimeRange))) || !strncmp(type,@encode(CMTime),strlen(@encode(CMTime)))) {
                    size_t size=!strncmp(type,@encode(CMTimeRange),strlen(@encode(CMTimeRange)))?sizeof(CMTimeRange):sizeof(CMTime);
                    [invocation setArgument:(void *)(guest_arguments+word) atIndex:index];
                    word+=size/4;
                } else if (strncmp(type, "{CGRect=", 8) == 0) {
                    NSRect value = NSMakeRect(fields[0], fields[1], fields[2], fields[3]);
                    [invocation setArgument:&value atIndex:index];
                    word += 4;
                } else if (strncmp(type, "{CGPoint=", 9) == 0) {
                    NSPoint value = NSMakePoint(fields[0], fields[1]);
                    [invocation setArgument:&value atIndex:index];
                    word += 2;
                } else if (strncmp(type, "{CGSize=", 8) == 0) {
                    NSSize value = NSMakeSize(fields[0], fields[1]);
                    [invocation setArgument:&value atIndex:index];
                    word += 2;
                } else {
                    return false;
                }
                continue;
            }
            default:
                return false;
        }
        word += ((*type == 'q' || *type == 'Q') && explicit_long_long) ?
                2 : guest_words_for_type(type);
    }

    bool string_init=managed_string_init(receiver,guest_arguments[0],selector_name);
    if(string_init)[receiver retain];
    @try {
        if (super_method) {
            if (!invoke_using_imp(invocation, method_getImplementation(super_method))) return false;
        } else [invocation invoke];
        for(NSUInteger i=2;i<signature.numberOfArguments;++i)
            if(guest_object_outputs[i])*guest_object_outputs[i]=proxy_for_object(object_outputs[i]);
    } @catch (NSException *exception) {
        fprintf(stderr, "compat32: host Objective-C message %s raised %s\n",
                selector_name, [[exception reason] UTF8String]);
        return false;
    }

    const char *return_type = skip_type_qualifiers([signature methodReturnType]);
    switch (*return_type) {
        case 'v': *guest_result = 0; return true;
        case '@': case '#': {
            id value = nil;
            [invocation getReturnValue:&value];
            if(string_init || ([value isKindOfClass:[NSString class]] &&
                ([receiver isKindOfClass:[NSString class]] ||
                 (class_isMetaClass(object_getClass(receiver)) && [(Class)receiver isSubclassOfClass:[NSString class]])))) {
                *guest_result=proxy_for_string_result(receiver,guest_arguments[0],selector_name,value,string_init);
                return true;
            }
            bool temporary = ([value isKindOfClass:[NSDate class]] &&
                (!strncmp(selector_name,"date",4) || !strcmp(selector_name,"distantPast") || !strcmp(selector_name,"distantFuture"))) ||
                (!strcmp(selector_name,"deviceDescription") && [receiver isKindOfClass:[NSScreen class]]);
            *guest_result = temporary ? proxy_for_autoreleased_object(value) :
                proxy_for_returned_object(receiver, value);
            return true;
        }
        case ':': {
            SEL value = (SEL)0;
            [invocation getReturnValue:&value];
            *guest_result = value ? objc_bridge32_guest_selector(sel_getName(value)) : 0;
            return true;
        }
        case '*': {
            const char *value = NULL;
            [invocation getReturnValue:&value];
            *guest_result = [receiver isKindOfClass:[NSString class]] ?
                intern_guest_cstring(value) : compat_runtime32_copy_cstring(value);
            return true;
        }
        case '^': {
            void *value = NULL;
            [invocation getReturnValue:&value];
            uintptr_t pointer = (uintptr_t)value;
            if (pointer > UINT32_MAX) {
                if(strstr(return_type,"CGImage=") || strstr(return_type,"CGContext=") || strstr(return_type,"CGColorSpace="))
                    *guest_result=proxy_for_object((id)value);
                else if(strstr(return_type,"CGL") || !strcmp(return_type,"^v"))
                    *guest_result=objc_bridge32_guest_pointer(value);
                else return false;
                return true;
            }
            *guest_result = (uint32_t)pointer;
            return true;
        }
        case 'c': { int8_t value = 0; [invocation getReturnValue:&value]; *guest_result = (uint32_t)(int32_t)value; return true; }
        case 'C': case 'B': { uint8_t value = 0; [invocation getReturnValue:&value]; *guest_result = value; return true; }
        case 's': { int16_t value = 0; [invocation getReturnValue:&value]; *guest_result = (uint32_t)(int32_t)value; return true; }
        case 'S': { uint16_t value = 0; [invocation getReturnValue:&value]; *guest_result = value; return true; }
        case 'i': case 'l': { int32_t value = 0; [invocation getReturnValue:&value]; *guest_result = (uint32_t)value; return true; }
        case 'I': case 'L': { uint32_t value = 0; [invocation getReturnValue:&value]; *guest_result = value; return true; }
        case 'q': case 'Q': { uint64_t value = 0; [invocation getReturnValue:&value]; *guest_result = value; return true; }
        case '{': {
            bool registers = guest_struct_result == NULL;
            uint64_t pair_result = 0;
            if (registers) guest_struct_result = &pair_result;
            if(!strncmp(return_type,@encode(CMTimeRange),strlen(@encode(CMTimeRange))) || !strncmp(return_type,@encode(CMTime),strlen(@encode(CMTime)))) {
                if(registers)return false;
                [invocation getReturnValue:guest_struct_result];
            } else if (strstr(return_type, "Rect=")) {
                if (registers) return false;
                NSRect rect; [invocation getReturnValue:&rect];
                float f[] = {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
                memcpy(guest_struct_result, f, sizeof(f));
            } else if (strstr(return_type, "Point=") || strstr(return_type, "Size=")) {
                NSSize pair; [invocation getReturnValue:&pair];
                float f[] = {pair.width, pair.height}; memcpy(guest_struct_result, f, sizeof(f));
            } else if (strstr(return_type, "Range=")) {
                NSRange range; [invocation getReturnValue:&range];
                uint32_t r[] = {(uint32_t)range.location, (uint32_t)range.length};
                memcpy(guest_struct_result, r, sizeof(r));
            } else return false;
            *guest_result = registers ? pair_result : 0; return true;
        }
        case 'f': { float value = 0; [invocation getReturnValue:&value]; *guest_result = compat_runtime32_return_double(value); return true; }
        case 'd': { double value = 0; [invocation getReturnValue:&value]; *guest_result = compat_runtime32_return_double(value); return true; }
        default: return false;
    }
}

@interface NSFullScreenWindow : NSWindow
@end

@implementation NSFullScreenWindow
- (BOOL)canBecomeMainWindow { return YES; }
- (BOOL)canBecomeKeyWindow { return YES; }
@end

/* Explicit compatibility/test overrides are separate from native semantics:
   NSScreen.mainScreen follows AppKit's keyboard focus, while CGMainDisplayID
   identifies the desktop's primary display. Re-resolve each time, including
   overrides, so focus changes and disconnected displays cannot leave a stale
   NSScreen cached for the lifetime of the process. */
static CGDirectDisplayID display_id_for_screen(NSScreen *screen)
{
    NSNumber *number = screen ?
        [[screen deviceDescription] objectForKey:@"NSScreenNumber"] : nil;
    return number ? (CGDirectDisplayID)[number unsignedIntValue] : 0;
}

static NSScreen *screen_for_display(CGDirectDisplayID display)
{
    for (NSScreen *screen in NSScreen.screens)
        if (display_id_for_screen(screen) == display) return screen;
    return nil;
}

static NSScreen *explicit_game_screen(void)
{
    const char *test_display = getenv("LP32_TEST_DISPLAY");
    const char *index_text = getenv("LP32_DISPLAY_INDEX");
    bool secondary = getenv("LP32_USE_SECONDARY_DISPLAY") != NULL;
    /* Ordinary Quartz calls must not initialize AppKit or enumerate screens. */
    if ((!test_display || !*test_display) && (!index_text || !*index_text) && !secondary) return nil;
    NSArray *screens = [NSScreen screens];
    NSUInteger count = [screens count];
    CGDirectDisplayID primary_id = CGMainDisplayID();
    if (test_display && test_display[0]) {
        CGDirectDisplayID requested=(CGDirectDisplayID)strtoul(test_display,NULL,0);
        for(NSScreen *candidate in screens) {
            if(display_id_for_screen(candidate)==requested)return candidate;
        }
    }
    if (index_text && index_text[0]) {
        char *end = NULL;
        unsigned long index = strtoul(index_text, &end, 10);
        if (end && *end == '\0' && index < count) {
            return [screens objectAtIndex:(NSUInteger)index];
        }
    }
    if (secondary) {
        for (NSScreen *candidate in screens) {
            CGDirectDisplayID candidate_id = display_id_for_screen(candidate);
            if (candidate_id && candidate_id != primary_id) {
                return candidate;
            }
        }
    }
    return nil;
}

static NSScreen *preferred_game_screen(void)
{
    return explicit_game_screen() ?: [NSScreen mainScreen];
}

static CGDirectDisplayID preferred_game_display_id(void)
{
    CGDirectDisplayID display = display_id_for_screen(explicit_game_screen());
    return display ? display : CGMainDisplayID();
}

static NSScreen *screen_for_foreground_window(NSArray *windows, pid_t owner)
{
    /* Quartz window bounds and CGDisplayBounds use the same desktop space,
       including displays to the left/above the primary one. No screenshot,
       window title or Accessibility permission is needed for these keys. */
    for (NSDictionary *window in windows) {
        if ([window[(id)kCGWindowOwnerPID] intValue] != owner ||
            [window[(id)kCGWindowLayer] intValue] != 0 ||
            [window[(id)kCGWindowAlpha] doubleValue] <= 0) continue;
        CGRect bounds;
        if (!CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)window[(id)kCGWindowBounds], &bounds)) continue;
        NSScreen *best = nil;
        CGFloat area = 0;
        for (NSScreen *screen in NSScreen.screens) {
            CGRect overlap = CGRectIntersection(bounds, CGDisplayBounds(display_id_for_screen(screen)));
            CGFloat size = CGRectIsNull(overlap) ? 0 : overlap.size.width * overlap.size.height;
            if (size > area) { area = size; best = screen; }
        }
        if (best) return best;
    }
    return nil;
}

static NSScreen *initial_game_screen(void)
{
    NSScreen *screen = explicit_game_screen();
    if (screen) return screen;
    pid_t owner = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    if (getenv("LP32_TRACE_DISPLAY"))
        fprintf(stderr, "compat32: game launch foreground=%d self=%d\n", owner, getpid());
    if (owner && owner != getpid()) {
        CFArrayRef windows = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        screen = screen_for_foreground_window((NSArray *)windows, owner);
        if (getenv("LP32_TRACE_DISPLAY"))
            fprintf(stderr, "compat32: game foreground window display=%u\n", display_id_for_screen(screen));
        if (windows) CFRelease(windows);
        if (screen) return screen;
    }
    CGEventRef event = CGEventCreate(NULL);
    if (event) {
        CGPoint pointer = CGEventGetLocation(event);
        if (getenv("LP32_TRACE_DISPLAY"))
            fprintf(stderr, "compat32: game launch pointer=%s\n", NSStringFromPoint(pointer).UTF8String);
        CFRelease(event);
        for (NSScreen *candidate in NSScreen.screens)
            if (CGRectContainsPoint(CGDisplayBounds(display_id_for_screen(candidate)), pointer)) return candidate;
    }
    return [NSScreen mainScreen];
}

/* Launch placement is a compatibility-window policy, separate from both
   NSScreen.mainScreen and Quartz's primary display. Remember the display ID,
   not an NSScreen object, and follow the actual game window after placement. */
static CGDirectDisplayID launch_display;
void objc_bridge32_capture_launch_display(void)
{
    @autoreleasepool {
        launch_display = display_id_for_screen(initial_game_screen());
        if (getenv("LP32_TRACE_DISPLAY"))
            fprintf(stderr, "compat32: captured launch display=%u primary=%u\n", launch_display, CGMainDisplayID());
    }
}

static NSScreen *game_window_screen(void)
{
    NSScreen *screen = explicit_game_screen();
    if (screen) return screen;
    if (presenting_view && [NSThread isMainThread]) {
        screen = [[(NSView *)presenting_view window] screen];
        if (screen) return screen;
    }
    if (!launch_display) launch_display = display_id_for_screen(initial_game_screen());
    return screen_for_display(launch_display) ?: [NSScreen mainScreen];
}

static NSScreen *game_screen_for_window(NSWindow *window)
{
    return explicit_game_screen() ?: window.screen ?: game_window_screen();
}

static CGDirectDisplayID game_window_display_id(void)
{
    CGDirectDisplayID display = display_id_for_screen(game_window_screen());
    return display ?: CGMainDisplayID();
}

/*
 * Emulated display mode switch.  Clone Wars' Feral layer runs the game at the
 * resolution from pcconfig.txt (1024x768 unless changed in Options) by
 * capturing the display and switching its mode; the game then sizes its GL
 * view and viewport to that mode and lays the frame out for it.  Modern macOS
 * keeps the desktop resolution, so the bridge keeps the borderless window at
 * the display's real size and instead renders the requested mode into a
 * fixed-size backing surface (kCGLCPSurfaceBackingSize) that CGL scales to an
 * aspect-correct rectangle centred in the window.  While a mode is active the
 * legacy CG "current mode" and the NSScreen frame report it too, so every
 * dimension the guest sees agrees.  Zero width means no switch is in effect.
 */
static NSSize guest_display_mode;
static CGDirectDisplayID guest_display_mode_display;

static NSSize display_native_size(CGDirectDisplayID display)
{
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (!current) return [screen_for_display(display) frame].size;
    NSSize size = NSMakeSize((CGFloat)CGDisplayModeGetWidth(current),
                             (CGFloat)CGDisplayModeGetHeight(current));
    CFRelease(current);
    return size;
}

float objc_bridge32_preferred_display_aspect_ratio(void)
{
    NSSize size = display_native_size(game_window_display_id());
    if (size.width <= 0 || size.height <= 0) return 0;
    return (float)(size.width / size.height);
}

/* Guest objects may live in the image or the guest heap, below the stacks. */
static bool saga_guest_pointer(uint32_t address, uint32_t length)
{
    return address >= UINT32_C(0x1000) && address < UINT32_C(0x7c000000) - length;
}

/* The automatic HUD camera reads a 16:9 constant even when the render mode
   is 16:10. Keep that input in step with the aspect of the mode that CGL
   actually presents. The renderer already stores the mode's correct ratio. */
static void set_saga_automatic_hud_aspect(uint32_t address, float aspect)
{
    static bool failed;
    if (failed) return;
    float current;
    memcpy(&current, (const void *)(uintptr_t)address, sizeof(current));
    if (current == aspect) return;
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = (uintptr_t)address & ~(page_size - 1);
    if (mprotect((void *)page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        fprintf(stderr, "compat32: Saga HUD aspect page protection failed: %s\n",
                strerror(errno));
        failed = true;
        return;
    }
    memcpy((void *)(uintptr_t)address, &aspect, sizeof(aspect));
    if (mprotect((void *)page, page_size, PROT_READ | PROT_EXEC)) {
        fprintf(stderr, "compat32: Saga HUD aspect page restore failed: %s\n",
                strerror(errno));
        failed = true;
        return;
    }
    fprintf(stderr, "compat32: Saga automatic HUD aspect %.6f -> %.6f\n",
            current, aspect);
}

/* Saga's InitPanel selects only a 16:9 or 4:3 scale for 3D HUD models.
   It can replace an output changed after presentation. Update both source
   presets for the active mode so subsequent InitPanel calls retain it. */
static void correct_saga_panel_scale(const struct lp32_screen_aspect_patch *patch,
                                     float mode_ratio)
{
    static float original[4];
    static bool original_saved;
    static bool failed;
    if (failed) return;
    if (getenv("LP32_DISABLE_PANEL_SCALE_FIX") ||
        !isfinite(mode_ratio) || mode_ratio < 1.0f || mode_ratio > 2.4f) return;
    float *table = (void *)(uintptr_t)patch->panel_scale_table;
    if (!original_saved) {
        memcpy(original, table, sizeof(original));
        original_saved = true;
    }
    float blend = (mode_ratio - 4.0f / 3.0f) /
                  (16.0f / 9.0f - 4.0f / 3.0f);
    float wanted_x = original[1] + (original[0] - original[1]) * blend;
    float wanted_y = original[3] + (original[2] - original[3]) * blend;
    if (fabsf(table[0] - wanted_x) > 0.00001f ||
        fabsf(table[1] - wanted_x) > 0.00001f ||
        fabsf(table[2] - wanted_y) > 0.00001f ||
        fabsf(table[3] - wanted_y) > 0.00001f) {
        size_t page_size = (size_t)getpagesize();
        uintptr_t page = (uintptr_t)table & ~(page_size - 1);
        if (mprotect((void *)page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC)) {
            fprintf(stderr, "compat32: Saga panel scale page protection failed: %s\n",
                    strerror(errno));
            failed = true;
            return;
        }
        table[0] = table[1] = wanted_x;
        table[2] = table[3] = wanted_y;
        if (mprotect((void *)page, page_size, PROT_READ | PROT_EXEC)) {
            fprintf(stderr, "compat32: Saga panel scale page restore failed: %s\n",
                    strerror(errno));
            failed = true;
            return;
        }
        fprintf(stderr,
                "compat32: Saga 3D panel presets %.6f %.6f (aspect %.6f)\n",
                wanted_x, wanted_y, mode_ratio);
    }
    volatile float *panel_x = (void *)(uintptr_t)patch->panel_scale_x;
    volatile float *panel_y = (void *)(uintptr_t)patch->panel_scale_y;
    if (fabsf(*panel_x - wanted_x) < 0.00001f &&
        fabsf(*panel_y - wanted_y) < 0.00001f) return;
    *panel_x = wanted_x;
    *panel_y = wanted_y;
}

/* The shipped score and heart rows are one original row span apart. At the
   corrected 3D scale their models nearly touch. Reserve another third of
   that span between the rows, in panel coordinates, so the gap scales with
   the HUD at each render resolution. The join prompt uses the heart row. */
static void correct_saga_panel_row_spacing(const struct lp32_screen_aspect_patch *patch)
{
    if (getenv("LP32_DISABLE_PANEL_ROW_SPACING")) return;
    const float coin_y = *(const float *)(uintptr_t)patch->panel_coin_y;
    volatile float *heart_y = (void *)(uintptr_t)patch->panel_heart_y;
    const float original_heart_y = -0.045f;
    float wanted = coin_y - (coin_y - original_heart_y) * 4.0f / 3.0f;
    if (!isfinite(coin_y) || !isfinite(*heart_y) ||
        (fabsf(*heart_y - original_heart_y) > 0.00001f &&
         fabsf(*heart_y - wanted) > 0.00001f))
        return;
    if (fabsf(*heart_y - wanted) < 0.00001f) return;
    *heart_y = wanted;
    fprintf(stderr, "compat32: Saga panel heart row %.6f -> %.6f\n",
            original_heart_y, wanted);
}

int objc_bridge32_run_panel_scale_self_test(const struct lp32_screen_aspect_patch *patch)
{
    if (!patch) return -1;
    const float *table = (const void *)(uintptr_t)patch->panel_scale_table;
    const volatile float *panel_x = (const void *)(uintptr_t)patch->panel_scale_x;
    const volatile float *panel_y = (const void *)(uintptr_t)patch->panel_scale_y;
    correct_saga_panel_scale(patch, 16.0f / 10.0f);
    float blend = (16.0f / 10.0f - 4.0f / 3.0f) /
                  (16.0f / 9.0f - 4.0f / 3.0f);
    float x = 0.52373159f + (0.60556465f - 0.52373159f) * blend;
    float y = 0.39126638f + (0.33842796f - 0.39126638f) * blend;
    for (unsigned i = 0; i < 2; ++i) {
        if (fabsf(table[i] - x) > 0.00001f ||
            fabsf(table[i + 2] - y) > 0.00001f) return -1;
    }
    if (fabsf(*panel_x - x) > 0.00001f ||
        fabsf(*panel_y - y) > 0.00001f) return -1;
    correct_saga_panel_row_spacing(patch);
    if (fabsf(*(const float *)(uintptr_t)patch->panel_heart_y + 0.075f) > 0.00001f)
        return -1;
    correct_saga_panel_scale(patch, 4.0f / 3.0f);
    return fabsf(table[0] - 0.52373159f) < 0.00001f &&
           fabsf(table[1] - 0.52373159f) < 0.00001f &&
           fabsf(table[2] - 0.39126638f) < 0.00001f &&
           fabsf(table[3] - 0.39126638f) < 0.00001f ? 0 : -1;
}

static void correct_saga_automatic_camera_ratio(uint64_t swap_count)
{
    (void)swap_count;
    const struct lp32_screen_aspect_patch *patch =
        lp32_profile()->screen_aspect_patch;
    if (!patch || getenv("LP32_DISABLE_SCREEN_ASPECT_PATCH")) return;

    uint32_t renderer_address =
        *(const uint32_t *)(uintptr_t)patch->renderer_pointer_slot;
    uint32_t option_address =
        *(const uint32_t *)(uintptr_t)patch->option_pointer_slot;
    if (renderer_address < UINT32_C(0x1000) ||
        renderer_address > bridge_image->max_address - 0x634 ||
        option_address < UINT32_C(0x1000) ||
        option_address > bridge_image->max_address - 0x428) return;

    const volatile uint32_t *option = (const void *)(uintptr_t)option_address;
    volatile uint32_t *renderer = (void *)(uintptr_t)renderer_address;
    uint32_t width = renderer[0x138 / 4];
    uint32_t height = renderer[0x13c / 4];
    if (getenv("LP32_TRACE_ASPECT_STATE")) {
        static bool logged_state;
        if (!logged_state && width >= 320 && height >= 200) {
            float ratio;
            memcpy(&ratio, (const void *)(renderer + 0x630 / 4), sizeof(ratio));
            fprintf(stderr,
                    "compat32: Saga aspect state option=%u mode=%ux%u camera=%.6f\n",
                    option[0x424 / 4], width, height, ratio);
            logged_state = true;
        }
    }
    if (width < 320 || width > 8192 || height < 200 || height > 8192) return;
    float mode_ratio = (float)width / (float)height;
    if (!isfinite(mode_ratio) || mode_ratio <= 0) return;
    correct_saga_panel_scale(patch, mode_ratio);
    correct_saga_panel_row_spacing(patch);
    if (option[0x424 / 4] != 3 || getenv("LP32_DISABLE_SCREEN_MODE_DATA")) {
        set_saga_automatic_hud_aspect(patch->hud_default_scale_address,
                                      9.0f / 16.0f);
        return;
    }
    float wanted = (float)height / (float)width;
    set_saga_automatic_hud_aspect(patch->hud_default_scale_address, wanted);

    /* The camera's automatic option initially uses the 16:9 constant. Match
       its aspect and derived fov to the render mode on the next frame. */
    uint32_t camera = *(const volatile uint32_t *)(uintptr_t)patch->camera_pointer;
    if (saga_guest_pointer(camera, 0x48)) {
        volatile float *fields = (void *)(uintptr_t)(camera + 0x40);
        float fov = fields[0], aspect = fields[1];
        if (aspect == 9.0f / 16.0f && wanted != aspect && isfinite(fov) && fov > 0) {
            fields[0] = fabsf(fov - (aspect + 0.75f) * 0.5f) < 0.0001f ?
                (wanted + 0.75f) * 0.5f : fov * wanted / aspect;
            fields[1] = wanted;
        }
    }
    float old_ratio;
    memcpy(&old_ratio, (const void *)(renderer + 0x630 / 4), sizeof(old_ratio));
    if (!isfinite(old_ratio) || old_ratio <= 0 ||
        fabsf(old_ratio - mode_ratio) < 0.0001f) return;
    memcpy((void *)(renderer + 0x630 / 4), &mode_ratio,
           sizeof(mode_ratio));
    static bool logged;
    if (!logged || getenv("LP32_TRACE_DISPLAY")) {
        fprintf(stderr,
                "compat32: Saga automatic camera ratio %.6f -> %.6f (mode %ux%u)\n",
                old_ratio, mode_ratio, width, height);
        logged = true;
    }
}

static bool guest_display_mode_active(void)
{
    return guest_display_mode.width > 0 && guest_display_mode.height > 0;
}

/* Largest rectangle with the guest mode's aspect ratio that fits in bounds,
   centred (letterboxed/pillarboxed against the window's black background). */
static NSRect guest_display_mode_rect(NSRect bounds)
{
    if (!guest_display_mode_active() || bounds.size.width <= 0 ||
        bounds.size.height <= 0) {
        return bounds;
    }
    CGFloat scale = fmin(bounds.size.width / guest_display_mode.width,
                         bounds.size.height / guest_display_mode.height);
    NSSize size = NSMakeSize(floor(guest_display_mode.width * scale),
                             floor(guest_display_mode.height * scale));
    return NSMakeRect(bounds.origin.x + floor((bounds.size.width - size.width) / 2),
                      bounds.origin.y + floor((bounds.size.height - size.height) / 2),
                      size.width, size.height);
}

static void set_guest_display_mode(NSSize requested, CGDirectDisplayID display)
{
    NSSize native = display_native_size(display);
    NSSize previous = guest_display_mode;
    if (requested.width <= 0 || requested.height <= 0 ||
        (requested.width == native.width && requested.height == native.height)) {
        guest_display_mode = NSZeroSize;
        guest_display_mode_display = 0;
    } else {
        guest_display_mode = requested;
        guest_display_mode_display = display;
    }
    if (!NSEqualSizes(previous, guest_display_mode)) {
        fprintf(stderr,
                "compat32: guest display mode %.0fx%.0f on a %.0fx%.0f display -> %s\n",
                requested.width, requested.height, native.width, native.height,
                guest_display_mode_active() ? "scaled backing surface" : "native");
    }
}

/*
 * Test-only input bridge.  AppKit events must be created on the main thread,
 * so the async-signal-safe handlers merely publish a key code.  swapBuffers
 * consumes it on the game's main thread.  Nothing is installed unless the
 * launcher explicitly sets LP32_ENABLE_TEST_INPUT.
 */
static volatile sig_atomic_t pending_test_key_plus_one;
static bool test_input_wasd;

static void test_input_signal_handler(int signal_number)
{
    unsigned short key_code;
    switch (signal_number) {
        case SIGUSR1: key_code = 36; break;  /* Return */
        case SIGUSR2: key_code = 49; break;  /* Space */
        case SIGINFO: key_code = test_input_wasd ? 1 : 125; break; /* S/Down */
        case SIGHUP: key_code = test_input_wasd ? 13 : 126; break; /* W/Up */
        case SIGWINCH: key_code = 53; break; /* Escape */
        case SIGURG: key_code = test_input_wasd ? 0 : 123; break;  /* A/Left */
        case SIGALRM: key_code = test_input_wasd ? 2 : 124; break; /* D/Right */
        case SIGPROF: key_code = 51; break;  /* Backspace */
        case SIGXCPU: key_code = 32; break;  /* U (skip cutscene) */
        default: return;
    }
    pending_test_key_plus_one = (sig_atomic_t)key_code + 1;
}

static void install_test_input_signals(void)
{
    const int signals[] = {
        SIGUSR1, SIGUSR2, SIGINFO, SIGHUP, SIGWINCH, SIGURG, SIGALRM,
        SIGPROF, SIGXCPU,
    };
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = test_input_signal_handler;
    sigemptyset(&action.sa_mask);
    for (size_t index = 0; index < sizeof(signals) / sizeof(signals[0]); ++index) {
        sigaction(signals[index], &action, NULL);
    }
}

/*
 * Test-only scripted input.  LP32_TEST_KEY_FIFO names a FIFO (mkfifo) from
 * which swapBuffers reads one "keycode [hold_seconds]" line per frame while no
 * other test key is active.  Unlike the signal bridge this can express any
 * key code and a hold duration, which is what walking a character needs.
 */
static int test_key_fifo_fd = -2;
static char test_key_fifo_buffer[256];
static size_t test_key_fifo_length;

static int read_test_key_fifo(double *hold_seconds)
{
    if (test_key_fifo_fd == -2) {
        const char *path = getenv("LP32_TEST_KEY_FIFO");
        test_key_fifo_fd = path && path[0] ?
            open(path, O_RDONLY | O_NONBLOCK) : -1;
        if (path && path[0] && test_key_fifo_fd < 0) {
            perror("compat32: open LP32_TEST_KEY_FIFO");
        }
    }
    if (test_key_fifo_fd < 0) return -1;

    char *newline = memchr(test_key_fifo_buffer, '\n', test_key_fifo_length);
    if (!newline) {
        ssize_t count = read(test_key_fifo_fd,
                             test_key_fifo_buffer + test_key_fifo_length,
                             sizeof(test_key_fifo_buffer) - 1 -
                                 test_key_fifo_length);
        if (count > 0) test_key_fifo_length += (size_t)count;
        newline = memchr(test_key_fifo_buffer, '\n', test_key_fifo_length);
        if (!newline) {
            if (test_key_fifo_length == sizeof(test_key_fifo_buffer) - 1) {
                test_key_fifo_length = 0;
            }
            return -1;
        }
    }
    *newline = '\0';
    char *end = NULL;
    long key_code = strtol(test_key_fifo_buffer, &end, 10);
    double hold = end ? strtod(end, NULL) : 0.0;
    size_t consumed = (size_t)(newline + 1 - test_key_fifo_buffer);
    memmove(test_key_fifo_buffer, newline + 1,
            test_key_fifo_length - consumed);
    test_key_fifo_length -= consumed;
    if (end == test_key_fifo_buffer || key_code < 0 ||
        (key_code > 127 && !(getenv("LP32_TEST_HID_BUTTONS") &&
            ((key_code>200 && key_code<=232) || (key_code>=240 && key_code<=247) ||
             key_code==260 || (key_code>=270 && key_code<=274))))) {
        return -1;
    }
    *hold_seconds = hold;
    return (int)key_code;
}

static void gl_trace_signal_handler(int signal_number)
{
    (void)signal_number;
    pending_gl_trace_arm = 1;
}

static void install_gl_trace_signal(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = gl_trace_signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGQUIT, &action, NULL);
}

static NSString *characters_for_test_key(unsigned short key_code)
{
    unichar character;
    switch (key_code) {
        case 36: return @"\r";
        case 49: return @" ";
        case 53: return @"\x1b";
        case 32: return @"u";
        case 1: return @"s";
        case 2: return @"d";
        case 13: return @"w";
        case 123: character = NSLeftArrowFunctionKey; break;
        case 124: character = NSRightArrowFunctionKey; break;
        case 125: character = NSDownArrowFunctionKey; break;
        case 126: character = NSUpArrowFunctionKey; break;
        case 0: return @"a";
        default: return @"";
    }
    return [NSString stringWithCharacters:&character length:1];
}

static unsigned char test_key_states[128];
int objc_bridge32_test_key_down(unsigned key) {
    return key<128 ? __atomic_load_n(&test_key_states[key],__ATOMIC_RELAXED) : 0;
}
static int saga_test_pad_named(const char *part)
{
    unsigned count=hid_bridge32_saga_pad_count();
    for(unsigned index=0;index<count;++index){
        const char *name=hid_bridge32_saga_pad_name(index);
        if(name && strstr(name,part))return (int)index;
    }
    return -1;
}
static void post_test_key_event(NSWindow *window, unsigned short key_code,
                                BOOL is_down)
{
    if (!window) return;
    if(key_code>200 && key_code<=232){
        int delivered=hid_bridge32_test_button(key_code-200,is_down);
        fprintf(stderr,"compat32: scripted HID button %u down=%d delivered=%d\n",key_code-200,is_down,delivered);
        return;
    }
    if(key_code>=240 && key_code<=247){
        int value=is_down?key_code-240:8;
        int delivered=hid_bridge32_test_hat(value);
        fprintf(stderr,"compat32: scripted HID hat value=%d delivered=%d\n",value,delivered);
        return;
    }
    if(key_code==260){
        int delivered=hid_bridge32_test_named_button("DUALSHOCK",2,is_down);
        fprintf(stderr,"compat32: scripted DualShock face button delivered=%d\n",delivered);
        return;
    }
    if(key_code>=270 && key_code<=274){
        int index=saga_test_pad_named("Xbox");
        unsigned page=key_code==270?9:1;
        unsigned usage=key_code==270?10:(key_code<=272?48:49);
        int value=key_code==270?!!is_down:(is_down?(key_code==271||key_code==274?255:0):128);
        int delivered=index>=0 && hid_bridge32_saga_emit((unsigned)index,page,usage,value);
        fprintf(stderr,"compat32: scripted Saga semantic page=%u usage=%u value=%d delivered=%d\n",
            page,usage,value,delivered);
        return;
    }
    if(key_code<128)__atomic_store_n(&test_key_states[key_code],is_down,__ATOMIC_RELAXED);
    NSString *characters = characters_for_test_key(key_code);
    NSTimeInterval timestamp = [[NSProcessInfo processInfo] systemUptime];
    NSEvent *event = [NSEvent
        keyEventWithType:is_down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                 location:NSZeroPoint
            modifierFlags:0
                timestamp:timestamp
             windowNumber:[window windowNumber]
                  context:nil
               characters:characters
      charactersIgnoringModifiers:characters
                isARepeat:NO
                  keyCode:key_code];
    if (getenv("LP32_BACKGROUND_TEST") && !getenv("LP32_TEST_APP_EVENT_QUEUE") && objc_legacy32_token(window))
        [window sendEvent:event];
    else [NSApp postEvent:event atStart:NO];
}

static void install_window_test_input(NSWindow *window)
{
    if (!window || !getenv("LP32_TEST_KEY_FIFO")) return;
    /* Saga polls its own CGL frame path. Two FIFO readers can route a test
       key to the launcher window instead of the game. */
    if (lp32_profile()->title == LP32_TITLE_COMPLETE_SAGA) return;
    static NSHashTable *installed;
    if (!installed) installed=[[NSHashTable weakObjectsHashTable] retain];
    if ([installed containsObject:window]) return;
    [installed addObject:window];
    __block int key=-1;
    __block CFAbsoluteTime release_time=0;
    NSTimer *timer=[NSTimer timerWithTimeInterval:0.02 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        CFAbsoluteTime now=CFAbsoluteTimeGetCurrent();
        if(key>=0 && now>=release_time) {
            post_test_key_event(window,(unsigned short)key,NO);key=-1;
            release_time=now+0.10; /* Let polling guests observe key-up. */
        }
        if(key<0 && now>=release_time && window.isVisible) {
            double hold=0;int next=read_test_key_fifo(&hold);
            if(next>=0) {
                key=next;release_time=now+(hold>0?hold:0.15);
                post_test_key_event(window,(unsigned short)key,YES);
                fprintf(stderr,"compat32: scripted window key %d\n",key);
            }
        }
    }];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
}

/* The Steam Saga build presents through CGLFlushDrawable, so it never reaches
 * the NSOpenGLView swapBuffers test input path. Keep its scripted key state in
 * sync with the Carbon GetKeys and CGEventSourceKeyState bridges. */
static void poll_cgl_test_input(void)
{
    if (!getenv("LP32_TEST_KEY_FIFO")) return;
    static int active_key = -1;
    static CFAbsoluteTime release_time;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    NSWindow *window = [NSApp keyWindow] ?: [NSApp mainWindow] ?: [NSApp.windows firstObject];
    if (active_key >= 0 && now >= release_time) {
        post_test_key_event(window, (unsigned short)active_key, NO);
        active_key = -1;
        release_time = now + 0.10;
    }
    if (active_key < 0 && now >= release_time && window) {
        double hold = 0;
        int next = read_test_key_fifo(&hold);
        if (next >= 0) {
            active_key = next;
            release_time = now + (hold > 0 ? hold : 0.15);
            post_test_key_event(window, (unsigned short)next, YES);
            fprintf(stderr, "compat32: scripted CGL key %d\n", next);
        }
    }
}

static void trace_saga_aspect_state(uint64_t frame)
{
    if (!getenv("LP32_TRACE_ASPECT") ||
        !lp32_profile()->screen_aspect_patch || frame % 120) return;
    /* PIC globals used by the Saga camera routine at guest 0x001a1ec1. */
    uint32_t display = *(volatile uint32_t *)(uintptr_t)0x00b2800c;
    uint32_t settings = *(volatile uint32_t *)(uintptr_t)0x00b28014;
    if (display < 0x1000 || display + 0x634 >= bridge_image->max_address ||
        settings < 0x1000 || settings + 0x428 >= bridge_image->max_address)
        return;
    float ratio = *(volatile float *)(uintptr_t)(display + 0x630);
    int32_t derived_mode = *(volatile int32_t *)(uintptr_t)(display + 0x62c);
    int32_t mode = *(volatile int32_t *)(uintptr_t)(settings + 0x424);
    fprintf(stderr, "compat32: aspect frame=%llu mode=%d derived=%d ratio=%.6f display=%08x settings=%08x\n",
            (unsigned long long)frame, mode, derived_mode, ratio, display, settings);
}

/*
 * Test-only guest memory probe.  LP32_DEBUG_MEM_FIFO names a FIFO read once
 * per presented frame, since lldb cannot attach to this process.  Commands:
 *   r ADDR COUNT        dump COUNT dwords (hex and float)
 *   p SLOT OFF COUNT    dump COUNT dwords at *(SLOT) + OFF
 *   w ADDR FLOAT        write a float once
 *   k ADDR FLOAT        write a float after every frame (k 0 0 clears all)
 */
static bool debug_read_guest(uint32_t address, void *out, size_t length)
{
    mach_vm_size_t copied = 0;
    return address >= UINT32_C(0x1000) &&
        mach_vm_read_overwrite(mach_task_self(), address, length,
                               (mach_vm_address_t)(uintptr_t)out,
                               &copied) == KERN_SUCCESS && copied == length;
}

static void debug_write_guest_float(uint32_t address, float value)
{
    float current;
    if (!debug_read_guest(address, &current, sizeof(current))) return;
    if (mach_vm_write(mach_task_self(), address, (vm_offset_t)&value,
                      sizeof(value)) == KERN_SUCCESS) return;
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = address & ~(page_size - 1);
    mprotect((void *)page, page_size * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy((void *)(uintptr_t)address, &value, sizeof(value));
}

static void debug_dump_guest(uint32_t address, unsigned count)
{
    if (count > 64) count = 64;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t word;
        float value;
        if (!debug_read_guest(address + i * 4, &word, 4)) {
            fprintf(stderr, "compat32: mem %08x unreadable\n", address + i * 4);
            return;
        }
        memcpy(&value, &word, 4);
        fprintf(stderr, "compat32: mem %08x = %08x %g\n", address + i * 4, word,
                value);
    }
}

static void poll_debug_mem_fifo(void)
{
    static int fd = -2;
    static char buffer[1024];
    static size_t length;
    static struct { uint32_t address; float value; } kept[16];
    static unsigned kept_count;
    if (fd == -2) {
        const char *path = getenv("LP32_DEBUG_MEM_FIFO");
        fd = path && path[0] ? open(path, O_RDONLY | O_NONBLOCK) : -1;
    }
    if (fd < 0) return;
    ssize_t count = read(fd, buffer + length, sizeof(buffer) - 1 - length);
    if (count > 0) length += (size_t)count;
    char *newline;
    while ((newline = memchr(buffer, '\n', length))) {
        *newline = '\0';
        char op = 0;
        unsigned a = 0, b = 0, c = 0;
        float value = 0;
        if (sscanf(buffer, " %c", &op) == 1) {
            if (op == 'r' && sscanf(buffer, " r %x %u", &a, &b) == 2) {
                debug_dump_guest(a, b);
            } else if (op == 'p' && sscanf(buffer, " p %x %x %u", &a, &b, &c) == 3) {
                uint32_t base;
                if (debug_read_guest(a, &base, 4)) {
                    fprintf(stderr, "compat32: mem *%08x = %08x\n", a, base);
                    debug_dump_guest(base + b, c);
                }
            } else if (op == 'w' && sscanf(buffer, " w %x %f", &a, &value) == 2) {
                debug_write_guest_float(a, value);
            } else if (op == 'k' && sscanf(buffer, " k %x %f", &a, &value) == 2) {
                if (!a) kept_count = 0;
                else if (kept_count < 16) {
                    kept[kept_count].address = a;
                    kept[kept_count++].value = value;
                }
            }
        }
        size_t consumed = (size_t)(newline + 1 - buffer);
        memmove(buffer, newline + 1, length - consumed);
        length -= consumed;
    }
    if (length == sizeof(buffer) - 1) length = 0;
    for (unsigned i = 0; i < kept_count; ++i)
        debug_write_guest_float(kept[i].address, kept[i].value);
}

/*
 * ScreenWidth, ScreenHeight, and ScreenRefreshRate are populated from
 * pcconfig.txt by the recovered setters at 0x252d40, 0x252d10, and 0x252c80.
 * A zero width/height is the old game's "current display" sentinel.  Modern
 * display APIs no longer turn that sentinel into a concrete menu selection,
 * so resolve it before the guest builds its cached Video Settings model.
 */
static void materialize_automatic_display_mode(void)
{
    const struct lp32_display_layout *display = lp32_profile()->display;
    if (!display || !display->screen_width) return;
    volatile int32_t *legacy_screen_width =
        (void *)(uintptr_t)display->screen_width;
    volatile int32_t *legacy_screen_height =
        (void *)(uintptr_t)display->screen_height;
    volatile int32_t *legacy_refresh_rate =
        (void *)(uintptr_t)display->refresh_rate;
    if (getenv("LP32_TRACE_DISPLAY")) {
        fprintf(stderr, "compat32: guest display config %dx%d@%d\n",
                *legacy_screen_width, *legacy_screen_height,
                *legacy_refresh_rate);
    }
    if (*legacy_screen_width != 0 && *legacy_screen_height != 0 &&
        *legacy_refresh_rate != 0) {
        return;
    }

    CGDisplayModeRef current_mode =
        CGDisplayCopyDisplayMode(game_window_display_id());
    if (!current_mode) return;
    size_t width = CGDisplayModeGetWidth(current_mode);
    size_t height = CGDisplayModeGetHeight(current_mode);
    double refresh_value = CGDisplayModeGetRefreshRate(current_mode);
    if (refresh_value < 1.0) refresh_value = 60.0;
    if (*legacy_screen_width == 0 && width <= INT32_MAX) {
        *legacy_screen_width = (int32_t)width;
    }
    if (*legacy_screen_height == 0 && height <= INT32_MAX) {
        *legacy_screen_height = (int32_t)height;
    }
    if (*legacy_refresh_rate == 0 && refresh_value <= INT32_MAX) {
        *legacy_refresh_rate = (int32_t)llround(refresh_value);
    }
    static bool logged_resolution_materialization;
    if (!logged_resolution_materialization) {
        fprintf(stderr,
                "compat32: materialized automatic display mode as %dx%d@%d\n",
                *legacy_screen_width, *legacy_screen_height,
                *legacy_refresh_rate);
        logged_resolution_materialization = true;
    }
    CFRelease(current_mode);
}

static void trace_guest_display_selection(void)
{
    if (!getenv("LP32_TRACE_RESOLUTION")) return;
    const struct lp32_display_layout *display = lp32_profile()->display;
    if (!display || !display->renderer_display_slot) return;

    uint32_t renderer_address =
        *(volatile uint32_t *)(uintptr_t)display->renderer_display_slot;
    if (renderer_address < UINT32_C(0x00001000) ||
        renderer_address >= bridge_image->max_address) {
        return;
    }
    const volatile uint8_t *renderer =
        (const void *)(uintptr_t)renderer_address;
    int32_t count = *(const volatile int32_t *)(renderer + 0x64);
    uint32_t modes_address =
        *(const volatile uint32_t *)(renderer + 0x68);
    int32_t selected = *(const volatile int32_t *)(renderer + 0xb4);
    static bool logged_raw_renderer;
    if (!logged_raw_renderer) {
        fprintf(stderr,
                "compat32: raw renderer display object slot=0x%08x "
                "value=0x%08x count=%d modes=0x%08x index=%d\n",
                display->renderer_display_slot, renderer_address, count,
                modes_address, selected);
        logged_raw_renderer = true;
    }
    if (count < 1 || count > 32 || modes_address < UINT32_C(0x00001000) ||
        modes_address >= bridge_image->max_address || selected < 0 ||
        selected >= count) {
        return;
    }
    const volatile int32_t *mode =
        (const void *)(uintptr_t)(modes_address + (uint32_t)selected * 12);
    static int32_t previous_selected = INT32_MIN;
    static int32_t previous_width = INT32_MIN;
    static int32_t previous_height = INT32_MIN;
    if (selected != previous_selected || mode[0] != previous_width ||
        mode[1] != previous_height) {
        fprintf(stderr,
                "compat32: renderer display selection object=0x%08x "
                "count=%d index=%d mode=%dx%d@%d config=%dx%d@%d\n",
                renderer_address, count, selected, mode[0], mode[1], mode[2],
                *(volatile int32_t *)(uintptr_t)display->screen_width,
                *(volatile int32_t *)(uintptr_t)display->screen_height,
                *(volatile int32_t *)(uintptr_t)display->refresh_rate);
        previous_selected = selected;
        previous_width = mode[0];
        previous_height = mode[1];
    }
}

/*
 * Frame pacing.
 *
 * The game asks for a CGL swap interval of 1 (pcconfig VerticalSync=1).  On a
 * double-buffered context that quantises presentation to whole refresh
 * periods: a 13 ms frame costs 16.7 ms and an 18 ms frame costs 33 ms, so a
 * scene hovering around the refresh budget alternates between 60 and 30 fps
 * and reads as constant frame drops.  The window server composites the
 * window at the display's refresh anyway, so the bridge runs the context
 * with swap interval 0 and paces presentation itself.  Frames then present
 * as soon as they are ready, and a slow frame costs only its own time.
 *
 * The target is the display's refresh rate capped at 60.  The engine's
 * simulation is tied to the frame rate (the Windows build is documented to
 * break puzzles and physics above 60), so the pacer never runs the game
 * faster than that by default even on a 120 Hz panel.
 * LP32_HONOR_SWAP_INTERVAL keeps the game's setting; LP32_MAX_FPS=<n>
 * overrides the pacing target (0 = uncapped, at the player's own risk).
 */
enum { kFramePacerDefaultCap = 60 };
/* Pacing is on from the first frame: Clone Wars ships without a pcconfig
   VerticalSync entry, asks for swap interval 0, and would otherwise run its
   frame-rate-bound simulation at 1000+ fps on this hardware. */
static bool frame_pacer_enabled = true;
static int frame_pacer_requested_interval;
static uint64_t frame_pacer_interval_ns;

static bool frame_pacer_takes_over_swap_interval(GLint requested)
{
    static int honor = -1;
    if (honor < 0) honor = getenv("LP32_HONOR_SWAP_INTERVAL") != NULL;
    frame_pacer_requested_interval = requested;
    if (honor) {
        frame_pacer_enabled = false;
        return false;
    }
    frame_pacer_enabled = true;
    if (getenv("LP32_TRACE_DISPLAY")) {
        fprintf(stderr,
                "compat32: game requested swap interval %d; bridge paces "
                "frames instead\n", requested);
    }
    return true;
}

static double frame_pacer_target_fps(NSWindow *window)
{
    /* -1: not read yet; -2: no override; otherwise the requested cap. */
    static int override_fps = -1;
    if (override_fps == -1) {
        const char *text = getenv("LP32_MAX_FPS");
        override_fps = text ? (atoi(text) > 0 ? atoi(text) : 0) : -2;
    }
    if (override_fps >= 0) return override_fps;

    NSScreen *screen = [window screen];
    if (!screen) screen = [NSScreen mainScreen];
    if (@available(macOS 12.0, *)) {
        NSInteger maximum = screen ? [screen maximumFramesPerSecond] : 0;
        if (maximum > 0 && maximum < kFramePacerDefaultCap) return (double)maximum;
    }
    return (double)kFramePacerDefaultCap;
}

/*
 * Only reached with LP32_MAX_FPS at or above 100 (the default cap is 60).
 * On a 120 Hz display a cap of 120 is only smooth if the game reaches it.
 * Gameplay frames take 8-15 ms of CPU here, so at 120 the pacer never
 * waits and every frame lands at a different time, which reads as constant
 * small drops.  The pacer therefore chooses between the display rate and
 * half of it (never below 60) from the recent frame work times: half rate
 * while any of the last 90 frames would not have fit a full-rate slot with
 * margin, full rate again once all of them fit comfortably.
 */
enum { kPacerHistory = 90 };
static uint64_t pacer_work_history[kPacerHistory];
static unsigned pacer_work_index;
static bool pacer_half_rate;

static uint64_t frame_pacer_choose_interval(double display_fps, uint64_t work_ns)
{
    uint64_t full_ns = (uint64_t)(1e9 / display_fps);
    pacer_work_history[pacer_work_index++ % kPacerHistory] = work_ns;
    if (display_fps < 100.0) return full_ns;

    uint64_t worst = 0;
    for (unsigned index = 0; index < kPacerHistory; ++index) {
        if (pacer_work_history[index] > worst) worst = pacer_work_history[index];
    }
    if (!pacer_half_rate && worst > full_ns * 9 / 10) {
        pacer_half_rate = true;
    } else if (pacer_half_rate && worst < full_ns * 7 / 10) {
        pacer_half_rate = false;
    }
    return pacer_half_rate ? full_ns * 2 : full_ns;
}

/* Called after flushBuffer.  Sleeps until the next presentation slot when
   the frame finished early.  The next slot is always one interval after the
   actual presentation, so a slow frame is never followed by a rushed one. */
static void frame_pacer_wait(NSWindow *window)
{
    static uint64_t next_slot_ns;
    static uint64_t last_present_ns;
    static double cached_fps;
    static uint64_t cached_fps_swap;
    static mach_timebase_info_data_t timebase;

    if (!frame_pacer_enabled) {
        frame_pacer_interval_ns = 0;
        next_slot_ns = 0;
        return;
    }
    if (!cached_fps || objc_bridge_swap_count - cached_fps_swap >= 120) {
        cached_fps = frame_pacer_target_fps(window);
        cached_fps_swap = objc_bridge_swap_count;
    }
    if (cached_fps <= 0.0) {
        frame_pacer_interval_ns = 0;
        next_slot_ns = 0;
        return;
    }
    uint64_t now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t work_ns = last_present_ns ? now_ns - last_present_ns : 0;
    uint64_t interval_ns = frame_pacer_choose_interval(cached_fps, work_ns);
    frame_pacer_interval_ns = interval_ns;
    if (next_slot_ns > now_ns) {
        if (!timebase.denom) mach_timebase_info(&timebase);
        uint64_t wait_ns = next_slot_ns - now_ns;
        uint64_t wait_ticks = wait_ns * timebase.denom / timebase.numer;
        mach_wait_until(mach_absolute_time() + wait_ticks);
        /* Schedule from the slot, not the wake-up time, so the few hundred
           microseconds the kernel oversleeps do not accumulate into a
           slower cadence. */
        last_present_ns = next_slot_ns;
        next_slot_ns += interval_ns;
    } else {
        last_present_ns = now_ns;
        next_slot_ns = now_ns + interval_ns;
    }
}

@interface OpenGLView : NSOpenGLView
- (id)initWithFrame:(NSRect)frame shareContext:(NSOpenGLContext *)shareContext;
- (id)initFullscreen:(NSRect)frame;
- (id)initWithFrame:(NSRect)frame shareContext:(NSOpenGLContext *)shareContext
  openGLDisplayMask:(uint32_t)displayMask sampleBuffers:(int)sampleBuffers
            samples:(int)samples;
- (id)initFullscreen:(NSRect)frame openGLDisplayMask:(uint32_t)displayMask
       sampleBuffers:(int)sampleBuffers samples:(int)samples;
- (int)updateMSAASettings:(int)sampleBuffers :(int)samples;
- (void)swapBuffers;
- (void)makeContextCurrent;
- (void)applyGuestDisplayMode;
- (NSRect)letterboxedFrame;
@end

/* The view the game presents through is declared above the screen selector. */

/*
 * Activation.  macOS 14 made it cooperative: activateIgnoringOtherApps: from a
 * process the user did not launch is ignored, and even a Finder launch
 * activates the app asynchronously, after the guest has ordered its window
 * front, so that window is not key yet when the first frame is presented.
 * Request activation and, once the app does become active, make the game
 * window key so keyboard input reaches it.
 */
/* LP32_BACKGROUND_TEST=1: an unattended run that must not disturb the user.
   The game is never activated or made key (its own requests are answered
   without effect), and it keeps running while inactive. */
static bool background_test_mode(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_BACKGROUND_TEST") != NULL;
    return enabled;
}

static void simulate_test_window_activation(NSWindow *window)
{
    if (getenv("LP32_TEST_FOCUS_LOSS")) return;
    /* Games driven by activation notifications must see the same simulated
     * focus as isActive/isKeyWindow, without making the real window key. */
    dispatch_async(dispatch_get_main_queue(), ^{
        /* Notify guest delegates only. Native AppKit observers require real
         * key-window state and must not receive synthetic focus changes. */
        id targets[] = {NSApp, NSApp.delegate, window.delegate};
        const char *selectors[] = {"applicationDidBecomeActive:",
                                   "applicationDidBecomeActive:", "windowDidBecomeKey:"};
        for (unsigned i = 0; i < 3; ++i) {
            if (!targets[i] || (i == 1 && targets[1] == targets[0]) ||
                !objc_legacy32_token(targets[i])) continue;
            SEL selector = sel_registerName(selectors[i]);
            if (![targets[i] respondsToSelector:selector]) continue;
            NSNotification *notification = [NSNotification notificationWithName:
                i == 2 ? NSWindowDidBecomeKeyNotification : NSApplicationDidBecomeActiveNotification
                object:i == 2 ? (id)window : (id)NSApp];
            [targets[i] performSelector:selector withObject:notification];
        }
    });
}


static bool handle_test_activation(id receiver, const char *selector, uint64_t *result)
{
        if (selector) {
            if (lp32_ignore_guest_focus_loss() &&
                ((!strcmp(selector, "isActive") && [receiver isKindOfClass:[NSApplication class]]) ||
                 ((!strcmp(selector, "isKeyWindow") || !strcmp(selector, "isMainWindow")) &&
                  [receiver isKindOfClass:[NSWindow class]]))) {
                *result = 1;
                return true;
            }
            if (!background_test_mode()) return false;
            if (strcmp(selector, "activateIgnoringOtherApps:") == 0 ||
                strcmp(selector, "makeKeyWindow") == 0 ||
                strcmp(selector, "makeMainWindow") == 0) {
                if ([receiver isKindOfClass:[NSWindow class]])
                    simulate_test_window_activation(receiver);
                *result = 0;
                return true;
            }
            if (strcmp(selector, "makeKeyAndOrderFront:") == 0 &&
                [receiver isKindOfClass:[NSWindow class]]) {
                place_test_window(receiver);
                [(NSWindow *)receiver orderFront:nil];
                simulate_test_window_activation(receiver);
                *result = 0;
                return true;
            }
        }
    return false;
}

/*
 * Quit from the Dock menu (or any other quit Apple event) reaches AppKit's own
 * -terminate:, not the guest's, so the objc_msgSend hook that ends a guest
 * quit with _Exit never sees it.  AppKit then runs the normal termination
 * teardown, which can wait forever on the mixed-ABI audio graph (see the exit
 * handling in compat_runtime.c), leaving a process that only Force Quit ends.
 * Leave the same way the guest's quit does, from the notification AppKit
 * posts right before exit().
 */
@interface LP32QuitEventHandler : NSObject
- (void)handleQuit:(NSAppleEventDescriptor *)event
    withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation LP32QuitEventHandler
- (void)handleQuit:(NSAppleEventDescriptor *)event
    withReplyEvent:(NSAppleEventDescriptor *)reply
{
    (void)event;
    (void)reply;
    fprintf(stderr, "compat32: quit Apple event received; exiting\n");
    compat_runtime32_heap_report("quit-apple-event");
    fflush(NULL);
    _Exit(EXIT_SUCCESS);
}
@end

static void install_termination_handler(void)
{
    static bool installed;
    if (installed) return;
    installed = true;
    /* The guest calls finishLaunching but never runs NSApplication's own
       loop, and AppKit's quit handler did not fire for a Dock quit under the
       bridge; register one directly with the Apple event manager. */
    static LP32QuitEventHandler *quit_handler;
    quit_handler = [[LP32QuitEventHandler alloc] init];
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:quit_handler
            andSelector:@selector(handleQuit:withReplyEvent:)
          forEventClass:kCoreEventClass
             andEventID:kAEQuitApplication];
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationWillTerminateNotification
                    object:NSApp
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        fprintf(stderr, "compat32: AppKit termination requested (Dock quit "
                "or quit Apple event); exiting\n");
        compat_runtime32_heap_report("AppKit-will-terminate");
        fflush(NULL);
        _Exit(EXIT_SUCCESS);
    }];
}

static void activate_game_application(void)
{
    if (background_test_mode()) return;
    static bool observing;
    if (!observing) {
        observing = true;
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidBecomeActiveNotification
                        object:NSApp
                         queue:nil
                    usingBlock:^(NSNotification *note) {
            (void)note;
            NSWindow *window = [presenting_view window];
            if (window && ![window isKeyWindow]) [window makeKeyAndOrderFront:nil];
        }];
    }
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
    }
}

@implementation OpenGLView

/* Size the context's backing surface to the emulated display mode (CGL scales
   it to the view) and letterbox the view inside its window.  With no mode in
   effect the surface follows the view and the view fills the window. */
- (void)applyGuestDisplayMode
{
    CGLContextObj context = [[self openGLContext] CGLContextObj];
    if (context) {
        if (guest_display_mode_active() &&
            display_id_for_screen(self.window.screen) == guest_display_mode_display) {
            GLint dimensions[2] = {
                (GLint)guest_display_mode.width, (GLint)guest_display_mode.height,
            };
            CGLSetParameter(context, kCGLCPSurfaceBackingSize, dimensions);
            CGLEnable(context, kCGLCESurfaceBackingSize);
        } else {
            CGLDisable(context, kCGLCESurfaceBackingSize);
        }
    }
    NSRect frame = [self letterboxedFrame];
    if (!NSEqualRects([self frame], frame)) [super setFrame:frame];
    [[self openGLContext] update];
}

/* Where the view belongs inside its window: the whole content area, or the
   aspect-correct rectangle for the emulated mode when the view sits in the
   letterbox container installed by the setContentView: bridge below.  A view
   that is itself the content view must fill the window (AppKit invariant), so
   it falls back to a stretched fill. */
- (NSRect)letterboxedFrame
{
    NSView *superview = [self superview];
    if (!superview) return [self frame];
    NSRect bounds = [superview bounds];
    return [[self window] contentView] == self ||
        display_id_for_screen(self.window.screen) != guest_display_mode_display ?
        bounds : guest_display_mode_rect(bounds);
}

- (void)setFrame:(NSRect)frame
{
    /* The game sizes the view to the mode it believes the display switched
       to; the emulated mode decides the real frame instead. */
    if ([self superview]) frame = [self letterboxedFrame];
    [super setFrame:frame];
    [self applyGuestDisplayMode];
}

static NSOpenGLPixelFormat *legacy_pixel_format(void)
{
    NSOpenGLPixelFormatAttribute attributes[] = {
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFADepthSize, 24,
        NSOpenGLPFAStencilSize, 8,
        0,
    };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [[[NSOpenGLPixelFormat alloc] initWithAttributes:attributes] autorelease];
#pragma clang diagnostic pop
}

- (id)initWithFrame:(NSRect)frame
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [super initWithFrame:frame pixelFormat:legacy_pixel_format()];
#pragma clang diagnostic pop
}

- (id)initWithFrame:(NSRect)frame shareContext:(NSOpenGLContext *)shareContext
{
    self = [self initWithFrame:frame];
    if (self && shareContext) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSOpenGLContext *context = [[[NSOpenGLContext alloc]
            initWithFormat:[self pixelFormat] shareContext:shareContext] autorelease];
        [self setOpenGLContext:context];
#pragma clang diagnostic pop
    }
    return self;
}

- (id)initFullscreen:(NSRect)frame { return [self initWithFrame:frame]; }

/* Clone Wars' older Feral layer passes the display mask and MSAA request
   explicitly.  Multisampling is not applied to the window's pixel format:
   the title renders through its own render targets and the swap-chain
   format is what the bridge's frame pacer and capture path expect. */
- (id)initWithFrame:(NSRect)frame shareContext:(NSOpenGLContext *)shareContext
  openGLDisplayMask:(uint32_t)displayMask sampleBuffers:(int)sampleBuffers
            samples:(int)samples
{
    (void)displayMask; (void)sampleBuffers; (void)samples;
    return [self initWithFrame:frame shareContext:shareContext];
}
- (id)initFullscreen:(NSRect)frame openGLDisplayMask:(uint32_t)displayMask
       sampleBuffers:(int)sampleBuffers samples:(int)samples
{
    (void)displayMask; (void)sampleBuffers; (void)samples;
    return [self initWithFrame:frame];
}
- (int)updateMSAASettings:(int)sampleBuffers :(int)samples
{
    (void)sampleBuffers; (void)samples;
    return 0;
}
- (void)swapBuffers
{
    static bool logged_first_frame;
    static bool captured_frame;
    static bool posted_debug_key;
    static bool posted_title_key;
    static bool posted_title_skip_return;
    static bool posted_title_skip_key;
    static CFAbsoluteTime title_skip_time;
    static bool installed_test_input_signals;
    static bool installed_gl_trace_signal;
    static int active_test_key = -1;
    static uint64_t test_key_release_swap;
    static CFAbsoluteTime test_key_release_time;
    static uint64_t posted_proxy_stress_events;
    static CFAbsoluteTime heap_test_start_time;
    static CFAbsoluteTime heap_gameplay_script_start;
    static CFAbsoluteTime heap_gameplay_next_movement;
    static unsigned heap_gameplay_script_step;
    static unsigned heap_gameplay_movement_step;
    static bool heap_gameplay_level_skip_posted;
    uint64_t swap_count = ++objc_bridge_swap_count;
    if (!installed_gl_trace_signal) {
        initialize_gl_trace_output();
        install_gl_trace_signal();
        installed_gl_trace_signal = true;
        gl_trace_printf(
            "compat32: enabled SIGQUIT-triggered GL tracing at swap %llu\n",
            (unsigned long long)swap_count);
    }
    arm_pending_gl_trace();
    controller_bridge32_poll(swap_count);
    compat_runtime32_heap_frame(swap_count);
    compat_runtime32_check_mode_guards(swap_count);
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (heap_test_start_time == 0.0) heap_test_start_time = now;
    const char *test_exit_text = getenv("LP32_TEST_EXIT_AT_SWAP");
    uint64_t test_exit_swap = test_exit_text ?
        strtoull(test_exit_text, NULL, 0) : 0;
    const char *test_seconds_text = getenv("LP32_TEST_EXIT_AFTER_SECONDS");
    double test_seconds = test_seconds_text ?
        strtod(test_seconds_text, NULL) : 0.0;
    if ((test_exit_swap && swap_count >= test_exit_swap) ||
        (test_seconds > 0.0 && now - heap_test_start_time >= test_seconds)) {
        compat_runtime32_heap_report("test-exit");
        fflush(NULL);
        _Exit(EXIT_SUCCESS);
    }
    if (!installed_test_input_signals && getenv("LP32_ENABLE_TEST_INPUT")) {
        test_input_wasd = getenv("LP32_TEST_WASD") != NULL;
        install_test_input_signals();
        installed_test_input_signals = true;
        fputs(test_input_wasd ?
              "compat32: enabled signal-driven test input "
              "(USR1=return USR2=space INFO=s HUP=w WINCH=escape "
              "URG=a ALRM=d PROF=backspace XCPU=u)\n" :
              "compat32: enabled signal-driven test input "
              "(USR1=return USR2=space INFO=down HUP=up "
              "WINCH=escape URG=left ALRM=right PROF=backspace "
              "XCPU=u)\n",
              stderr);
    }
    const char *stress_event_text = getenv("LP32_PROXY_STRESS_EVENTS");
    uint64_t stress_event_target = stress_event_text ?
        strtoull(stress_event_text, NULL, 0) : 0;
    if (stress_event_target > 100000) stress_event_target = 100000;
    if (posted_proxy_stress_events < stress_event_target) {
        NSWindow *window = [self window];
        uint64_t batch = stress_event_target - posted_proxy_stress_events;
        if (batch > 16) batch = 16;
        for (uint64_t index = 0; index < batch; ++index) {
            post_test_key_event(window, 90, (posted_proxy_stress_events & 1) == 0);
            ++posted_proxy_stress_events;
        }
        if (posted_proxy_stress_events == stress_event_target) {
            fprintf(stderr,
                    "compat32: posted %llu F20 proxy-stress events\n",
                    (unsigned long long)posted_proxy_stress_events);
        }
    }
    if (getenv("LP32_TRACE_OBJC_PROXIES") &&
        (swap_count <= 10 || swap_count % 300 == 0)) {
        fprintf(stderr,
                "compat32: Objective-C proxies swap=%llu persistent=%u "
                "event=%u\n",
                (unsigned long long)swap_count, proxy_count,
                event_proxy_count);
    }
    materialize_automatic_display_mode();
    trace_guest_display_selection();
    sig_atomic_t pending_key = pending_test_key_plus_one;
    if (pending_key > 0) {
        pending_test_key_plus_one = 0;
        unsigned short key_code = (unsigned short)(pending_key - 1);
        if (active_test_key >= 0) {
            post_test_key_event([self window],
                                (unsigned short)active_test_key, NO);
        }
        post_test_key_event([self window], key_code, YES);
        active_test_key = key_code;
        /* The guest applies its own menu-repeat policy to held arrows. */
        bool is_directional = (key_code >= 123 && key_code <= 126) ||
                              key_code == 0 || key_code == 1 ||
                              key_code == 2 || key_code == 13;
        test_key_release_swap = swap_count + (is_directional ? 1 : 6);
        test_key_release_time = now + (is_directional ? 0.05 : 0.15);
        fprintf(stderr, "compat32: pressed test key %hu at swap %llu\n",
                key_code, (unsigned long long)swap_count);
    }
    if (active_test_key < 0) {
        double hold_seconds = 0.0;
        int fifo_key = read_test_key_fifo(&hold_seconds);
        if (fifo_key >= 0) {
            post_test_key_event([self window], (unsigned short)fifo_key, YES);
            active_test_key = fifo_key;
            test_key_release_swap = swap_count + 1;
            test_key_release_time = now + (hold_seconds > 0.0 ? hold_seconds : 0.15);
            fprintf(stderr,
                    "compat32: pressed fifo test key %d for %.2fs at swap %llu\n",
                    fifo_key, hold_seconds > 0.0 ? hold_seconds : 0.15,
                    (unsigned long long)swap_count);
        }
    }
    if (active_test_key >= 0 && swap_count >= test_key_release_swap &&
        (test_key_release_time == 0.0 || now >= test_key_release_time)) {
        post_test_key_event([self window], (unsigned short)active_test_key, NO);
        fprintf(stderr, "compat32: released test key %d at swap %llu\n",
                active_test_key, (unsigned long long)swap_count);
        active_test_key = -1;
        test_key_release_time = 0.0;
    }
    if (!posted_title_skip_key && title_skip_time > 0.0 &&
        now >= title_skip_time) {
        if (active_test_key >= 0) {
            post_test_key_event([self window],
                                (unsigned short)active_test_key, NO);
        }
        if (!posted_title_skip_return) {
            /* The title's movie skip is a two-step chord: Return reveals the
               prompt, then U confirms it.  A lone U was timing-dependent. */
            post_test_key_event([self window], 36, YES);
            active_test_key = 36;
            test_key_release_swap = swap_count + 6;
            test_key_release_time = now + 0.15;
            title_skip_time = now + 0.5;
            posted_title_skip_return = true;
            fprintf(stderr,
                    "compat32: posted title cutscene Return at swap %llu\n",
                    (unsigned long long)swap_count);
        } else {
            post_test_key_event([self window], 32, YES);
            active_test_key = 32;
            test_key_release_swap = swap_count + 6;
            test_key_release_time = now + 0.25;
            posted_title_skip_key = true;
            fprintf(stderr,
                    "compat32: posted title cutscene U at swap %llu\n",
                    (unsigned long long)swap_count);
        }
    }
    if (getenv("LP32_HEAP_GAMEPLAY_STRESS") && posted_title_skip_key) {
        static const double menu_times[] = {40.0, 42.5, 45.0, 48.0, 118.0};
        static const unsigned short movement_keys[] = {13, 2, 13, 0, 49, 32};
        if (heap_gameplay_script_start == 0.0) {
            const char *movement_delay_text =
                getenv("LP32_HEAP_GAMEPLAY_MOVEMENT_DELAY");
            double movement_delay = movement_delay_text ?
                strtod(movement_delay_text, NULL) : 270.0;
            /* Step five starts gameplay at 118 seconds.  Keep at least a
               small loading margin when a shorter diagnostic delay is used. */
            if (movement_delay < 125.0) movement_delay = 125.0;
            heap_gameplay_script_start = now;
            heap_gameplay_next_movement = now + movement_delay;
            fprintf(stderr,
                    "compat32: armed heap gameplay stress after title at "
                    "swap %llu (movement delay %.1fs)\n",
                    (unsigned long long)swap_count, movement_delay);
        }
        double elapsed = now - heap_gameplay_script_start;
        if (heap_gameplay_script_step <
                sizeof(menu_times) / sizeof(menu_times[0]) &&
            elapsed >= menu_times[heap_gameplay_script_step] &&
            active_test_key < 0) {
            post_test_key_event([self window], 36, YES);
            active_test_key = 36;
            test_key_release_swap = swap_count + 6;
            test_key_release_time = now + 0.15;
            fprintf(stderr,
                    "compat32: heap gameplay stress Return step %u at "
                    "swap %llu\n", heap_gameplay_script_step + 1,
                    (unsigned long long)swap_count);
            ++heap_gameplay_script_step;
        } else if (heap_gameplay_script_step ==
                       sizeof(menu_times) / sizeof(menu_times[0]) &&
                   getenv("LP32_HEAP_GAMEPLAY_SKIP_LEVEL_CUTSCENE") &&
                   !heap_gameplay_level_skip_posted && elapsed >= 123.0 &&
                   active_test_key < 0) {
            post_test_key_event([self window], 32, YES);
            active_test_key = 32;
            test_key_release_swap = swap_count + 6;
            test_key_release_time = now + 0.25;
            heap_gameplay_level_skip_posted = true;
            fprintf(stderr,
                    "compat32: heap gameplay stress posted level cutscene U "
                    "at swap %llu\n", (unsigned long long)swap_count);
        } else if (heap_gameplay_script_step ==
                       sizeof(menu_times) / sizeof(menu_times[0]) &&
                   now >= heap_gameplay_next_movement &&
                   active_test_key < 0) {
            unsigned short key = movement_keys[heap_gameplay_movement_step %
                (sizeof(movement_keys) / sizeof(movement_keys[0]))];
            post_test_key_event([self window], key, YES);
            active_test_key = key;
            test_key_release_swap = swap_count + 30;
            test_key_release_time = now + 0.5;
            heap_gameplay_next_movement = now + 1.5;
            ++heap_gameplay_movement_step;
            if (heap_gameplay_movement_step <= 12 ||
                heap_gameplay_movement_step % 30 == 0) {
                fprintf(stderr,
                        "compat32: heap gameplay stress movement %u key %hu "
                        "at swap %llu\n", heap_gameplay_movement_step, key,
                        (unsigned long long)swap_count);
            }
        }
    }
    if (getenv("LP32_TRACE_TIMING") &&
        (swap_count <= 10 || swap_count % 60 == 0)) {
        fprintf(stderr, "compat32: OpenGL swap %llu\n",
                (unsigned long long)swap_count);
    }
    const struct lp32_display_layout *frontend = lp32_profile()->display;
    if (getenv("LP32_TRACE_FRONTEND_STATE") && frontend &&
        frontend->frontend_state_pointer &&
        (swap_count <= 10 || swap_count % 60 == 0)) {
        const uint32_t *state_slot =
            (const void *)(uintptr_t)frontend->frontend_state_pointer;
        const uint32_t *page_slot =
            (const void *)(uintptr_t)frontend->frontend_page_slot;
        const uint32_t *state = state_slot[0] ?
            (const void *)(uintptr_t)state_slot[0] : NULL;
        const uint32_t *page_base = page_slot[0] ?
            (const void *)(uintptr_t)page_slot[0] : NULL;
        const uint32_t *page = page_base ? page_base + 3 : NULL;
        const uint32_t *gate_slot =
            (const void *)(uintptr_t)frontend->frontend_gate_slot;
        const uint32_t *gate = gate_slot[0] ?
            (const void *)(uintptr_t)gate_slot[0] : NULL;
        fprintf(stderr,
                "compat32: frontend swap=%llu state=%d page=%08x "
                "count10=%d cursor18=%d limit40=%d item44=%d "
                "fade48=%08x clock4c=%08x:%08x now54=%08x:%08x "
                "delay5c=%08x wait60=%u flags64=%08x owner68=%08x "
                "gate=%08x gate2c=%d gate30=%08x:%08x "
                "gate38=%08x:%08x gate40=%08x gate44=%u "
                "gate48=%08x gate50=%08x gate54=%d active=%08x\n",
                (unsigned long long)swap_count,
                state ? (int32_t)state[0] : -999,
                page ? (uint32_t)(uintptr_t)page : 0,
                page ? (int32_t)page[0x10 / 4] : -999,
                page ? (int32_t)page[0x18 / 4] : -999,
                page ? (int32_t)page[0x40 / 4] : -999,
                page ? (int32_t)page[0x44 / 4] : -999,
                page ? page[0x48 / 4] : 0,
                page ? page[0x4c / 4] : 0,
                page ? page[0x50 / 4] : 0,
                page ? page[0x54 / 4] : 0,
                page ? page[0x58 / 4] : 0,
                page ? page[0x5c / 4] : 0,
                page ? ((const uint8_t *)page)[0x60] : 0,
                page ? page[0x64 / 4] : 0,
                page ? page[0x68 / 4] : 0,
                gate ? (uint32_t)(uintptr_t)gate : 0,
                gate ? (int32_t)gate[0x2c / 4] : -999,
                gate ? gate[0x30 / 4] : 0,
                gate ? gate[0x34 / 4] : 0,
                gate ? gate[0x38 / 4] : 0,
                gate ? gate[0x3c / 4] : 0,
                gate ? gate[0x40 / 4] : 0,
                gate ? ((const uint8_t *)gate)[0x44] : 0,
                gate ? gate[0x48 / 4] : 0,
                gate ? gate[0x50 / 4] : 0,
                gate ? (int32_t)gate[0x54 / 4] : -999,
                *(const uint32_t *)(uintptr_t)frontend->frontend_active_word);
    }
    presenting_view = self;
    /*
     * The game's fullscreen window hides on deactivate.  Ordering a window out
     * destroys its GL surface, and AppKit only re-attaches the context to the
     * new surface on an update, which nothing triggers when the window comes
     * back with an unchanged frame: every flushBuffer after reactivation went
     * to a detached drawable and the window stayed black while the game ran
     * on (audio and input still working).  Re-attach on the first frame
     * presented after the window becomes visible again.  A window hidden by
     * hidesOnDeactivate still answers isVisible=YES (only an explicit
     * orderOut: clears it) while its occlusion state drops the visible bit;
     * the occlusion bit in turn is not reliable for a window on another
     * display that is ordered in without the application being active.  A
     * window that regains either is treated as shown again.
     */
    {
        static unsigned window_visibility;
        static uint64_t reattach_until;
        NSWindow *window = [self window];
        unsigned visibility = 0;
        if (window && [window isVisible]) visibility |= 1;
        if (window && ([window occlusionState] &
                       NSWindowOcclusionStateVisible)) visibility |= 2;
        bool visible = visibility != 0;
        if ((visibility & ~window_visibility) && logged_first_frame) {
            reattach_until = swap_count + 30;
            fprintf(stderr, "compat32: window shown again at swap %llu "
                    "(visibility %u -> %u); re-attaching the GL context\n",
                    (unsigned long long)swap_count, window_visibility,
                    visibility);
        }
        window_visibility = visibility;
        if (visible && swap_count <= reattach_until) {
            NSOpenGLContext *context = [self openGLContext];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            if ([context view] != self) [context setView:self];
#pragma clang diagnostic pop
            [self applyGuestDisplayMode];
            [window display];
        }
    }
    if (!logged_first_frame) {
        NSWindow *window = [self window];
        install_termination_handler();
        [self applyGuestDisplayMode];
        if (window && !background_test_mode()) {
            activate_game_application();
            [window makeKeyAndOrderFront:nil];
            [window makeMainWindow];
        } else if (window) {
            [window orderFront:nil];
        }
        fprintf(stderr,
                "compat32: presenting first OpenGL frame window=%s visible=%d key=%d main=%d\n",
                window ? class_getName([window class]) : "(none)",
                window ? [window isVisible] : 0,
                window ? [window isKeyWindow] : 0,
                window ? [window isMainWindow] : 0);
        logged_first_frame = true;
    }
    const char *debug_key_swap_text = getenv("LP32_AUTOPRESS_RETURN_AT_SWAP");
    uint64_t debug_key_swap = debug_key_swap_text ?
        strtoull(debug_key_swap_text, NULL, 10) : 0;
    if (!posted_debug_key && debug_key_swap && swap_count >= debug_key_swap) {
        NSWindow *window = [self window];
        const char *debug_key_code_text = getenv("LP32_AUTOPRESS_KEYCODE");
        unsigned long parsed_key_code = debug_key_code_text ?
            strtoul(debug_key_code_text, NULL, 10) : 36;
        unsigned short key_code = parsed_key_code <= UINT16_MAX ?
            (unsigned short)parsed_key_code : 36;
        if (active_test_key >= 0) {
            post_test_key_event(window, (unsigned short)active_test_key, NO);
        }
        post_test_key_event(window, key_code, YES);
        active_test_key = key_code;
        test_key_release_swap = swap_count + 6;
        test_key_release_time = now + 0.15;
        fprintf(stderr, "compat32: posted debug key %hu at swap %llu\n",
                key_code, (unsigned long long)swap_count);
        posted_debug_key = true;
    }
    if (!posted_title_key && getenv("LP32_AUTOPRESS_ON_TITLE") &&
        swap_count % 15 == 0) {
        GLint viewport[4] = {0, 0, 0, 0};
        GLint old_read_buffer = GL_BACK;
        GLint old_pack_alignment = 4;
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
        int sample_x = viewport[0] + viewport[2] * 12 / 100;
        int sample_y = viewport[1] + viewport[3] * 66 / 100;
        int sample_width = viewport[2] * 16 / 100;
        int sample_height = viewport[3] * 18 / 100;
        size_t pixel_count = sample_width > 0 && sample_height > 0 ?
            (size_t)sample_width * (size_t)sample_height : 0;
        uint8_t *pixels = pixel_count ? malloc(pixel_count * 3) : NULL;
        if (pixels) {
            glReadBuffer(GL_BACK);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(sample_x, sample_y, sample_width, sample_height,
                         GL_RGB, GL_UNSIGNED_BYTE, pixels);
            size_t red_pixels = 0;
            for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
                const uint8_t *rgb = pixels + pixel * 3;
                if (rgb[0] > 160 && rgb[1] < 90 && rgb[2] < 90) {
                    ++red_pixels;
                }
            }
            free(pixels);
            if (red_pixels * 100 >= pixel_count * 30) {
                if (active_test_key >= 0) {
                    post_test_key_event([self window],
                                        (unsigned short)active_test_key, NO);
                }
                post_test_key_event([self window], 36, YES);
                active_test_key = 36;
                test_key_release_swap = swap_count + 6;
                test_key_release_time = now + 0.15;
                posted_title_key = true;
                if (getenv("LP32_AUTOSKIP_AFTER_TITLE")) {
                    title_skip_time = now + 1.0;
                }
                fprintf(stderr,
                        "compat32: detected title and posted test Return at swap %llu\n",
                        (unsigned long long)swap_count);
            }
        }
        glReadBuffer(old_read_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    }
    const char *capture_path = getenv("LP32_CAPTURE_FRAME");
    const char *capture_number_text = getenv("LP32_CAPTURE_FRAME_NUMBER");
    uint64_t capture_number = capture_number_text ?
        strtoull(capture_number_text, NULL, 10) : 1;
    const char *capture_interval_text = getenv("LP32_CAPTURE_INTERVAL");
    uint64_t capture_interval = capture_interval_text ?
        strtoull(capture_interval_text, NULL, 10) : 0;
    bool capture_due = swap_count >= capture_number &&
        (!capture_interval || (swap_count - capture_number) % capture_interval == 0);
    if ((!captured_frame || capture_interval) && capture_due &&
        capture_path && capture_path[0]) {
        GLint viewport[4] = {0, 0, 0, 0};
        GLint old_read_buffer = GL_BACK;
        GLint old_pack_alignment = 4;
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
        if (viewport[2] > 0 && viewport[3] > 0) {
            size_t row_size = (size_t)viewport[2] * 3;
            size_t byte_size = row_size * (size_t)viewport[3];
            uint8_t *pixels = malloc(byte_size);
            if (pixels) {
                glReadBuffer(GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3],
                             GL_RGB, GL_UNSIGNED_BYTE, pixels);
                bool has_visible_pixel = false;
                for (size_t offset = 0; offset < byte_size; ++offset) {
                    if (pixels[offset] > 2) {
                        has_visible_pixel = true;
                        break;
                    }
                }
                if (has_visible_pixel) {
                    char sequenced_path[4096];
                    const char *output_path = capture_path;
                    if (getenv("LP32_CAPTURE_SEQUENCE")) {
                        int length = snprintf(sequenced_path,
                                              sizeof(sequenced_path),
                                              "%s.swap-%llu.ppm", capture_path,
                                              (unsigned long long)swap_count);
                        if (length > 0 && (size_t)length < sizeof(sequenced_path)) {
                            output_path = sequenced_path;
                        }
                    }
                    FILE *file = fopen(output_path, "wb");
                    if (file) {
                        fprintf(file, "P6\n%d %d\n255\n", viewport[2], viewport[3]);
                        for (GLint row = viewport[3] - 1; row >= 0; --row) {
                            fwrite(pixels + (size_t)row * row_size, row_size, 1,
                                   file);
                        }
                        fclose(file);
                        fprintf(stderr,
                                "compat32: captured non-black swap %llu %dx%d at %s\n",
                                (unsigned long long)swap_count,
                                viewport[2], viewport[3], output_path);
                        captured_frame = capture_interval == 0;
                    }
                }
                free(pixels);
            }
        }
        glReadBuffer((GLenum)old_read_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    }
    trace_gl_frame_boundary();
    if (!compat_runtime32_frame_profile_enabled) {
        uint64_t work_end = hitch_recorder_enabled ? hitch_now() : 0;
        [[self openGLContext] flushBuffer];
        uint64_t flush_end = hitch_recorder_enabled ? hitch_now() : 0;
        frame_pacer_wait([self window]);
        if (hitch_recorder_enabled) {
            hitch_frame(swap_count, work_end, flush_end, hitch_now(),
                        frame_pacer_interval_ns,
                        [NSApp isActive] || getenv("LP32_BACKGROUND_TEST"));
        }
        audio_bridge32_note_frame_presented();
        return;
    }

    /* Frame pacing statistics (LP32_FRAME_STATS=<swap interval>).  The
       bridge counters cover the render thread since the previous swap; the
       previous flushBuffer wait is part of that dispatch total and is
       reported separately so bridge overhead can be read as
       dispatch - flush. */
    static struct {
        uint64_t last_swap_ns;
        uint64_t frames;
        uint64_t frame_ns_total, frame_ns_max;
        uint64_t frames_over_20ms, frames_over_34ms;
        uint64_t flush_ns_total, flush_ns_max;
        uint64_t calls_total, calls_max;
        uint64_t dispatch_ns_total, dispatch_ns_max;
        uint64_t audio_ns_total, audio_ns_max;
        uint64_t objc_ns_total;
        uint64_t lock_wait_ns_total, lock_wait_ns_max, lock_waits_total;
        uint64_t last_flush_ns;
        uint64_t present_ns_total, present_ns_max;
    } stats;
    static uint64_t report_interval;
    if (!report_interval) {
        report_interval = strtoull(getenv("LP32_FRAME_STATS"), NULL, 0);
        if (!report_interval) report_interval = 300;
    }
    uint64_t now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    struct compat_runtime32_frame_profile profile;
    compat_runtime32_take_frame_profile(&profile);
    if (stats.last_swap_ns) {
        uint64_t frame_ns = now_ns - stats.last_swap_ns;
        uint64_t bridge_ns = profile.dispatch_ns > stats.last_flush_ns ?
            profile.dispatch_ns - stats.last_flush_ns : 0;
        ++stats.frames;
        stats.frame_ns_total += frame_ns;
        if (frame_ns > stats.frame_ns_max) stats.frame_ns_max = frame_ns;
        if (frame_ns > 20000000) ++stats.frames_over_20ms;
        if (frame_ns > 34000000) ++stats.frames_over_34ms;
        stats.calls_total += profile.calls;
        if (profile.calls > stats.calls_max) stats.calls_max = profile.calls;
        stats.dispatch_ns_total += bridge_ns;
        if (bridge_ns > stats.dispatch_ns_max) stats.dispatch_ns_max = bridge_ns;
        stats.audio_ns_total += profile.audio_ns;
        if (profile.audio_ns > stats.audio_ns_max) stats.audio_ns_max = profile.audio_ns;
        stats.objc_ns_total += profile.objc_ns > stats.last_flush_ns ?
            profile.objc_ns - stats.last_flush_ns : 0;
        stats.lock_wait_ns_total += profile.lock_wait_ns;
        if (profile.lock_wait_ns > stats.lock_wait_ns_max) {
            stats.lock_wait_ns_max = profile.lock_wait_ns;
        }
        stats.lock_waits_total += profile.lock_waits;
        if (stats.frames % report_interval == 0) {
            double frames = (double)stats.frames;
            /* Presented frames per second: the interval between
               consecutive returns from flush (including pacing) is the
               real frame rate; "frame" below is the CPU time between
               flushes, i.e. how fast the game could run uncapped. */
            double present_seconds = (double)stats.present_ns_total / 1e9;
            uint64_t pool_hits = 0, pool_misses = 0;
            audio_bridge32_pool_statistics(&pool_hits, &pool_misses);
            uint32_t mode_to64 = 0, mode_to32 = 0;
            compat_runtime32_mode_guard_counts(&mode_to64, &mode_to32);
            struct audio_bridge32_worker_stats worker = {0};
            audio_bridge32_worker_statistics(&worker);
            fprintf(stderr,
                    "compat32: frames swap=%llu n=%llu fps=%.1f "
                    "present(avg=%.2f max=%.2f) "
                    "frame(avg=%.2f max=%.2f ms >20ms=%llu >34ms=%llu) "
                    "flush(avg=%.2f max=%.2f) "
                    "imports(avg=%.0f max=%llu) "
                    "bridge(avg=%.2f max=%.2f objc=%.2f audio=%.2f audio-max=%.2f) "
                    "lock-wait(avg=%.2f max=%.2f waits=%llu) "
                    "pool(hits=%llu misses=%llu) pace=%s "
                    "mode-recoveries(to64=%u to32=%u) "
                    "audio-worker(starts=%llu wait-avg=%.1f wait-max=%.1f "
                    "op-max=%.1f depth-max=%u) "
                    "audio-stream(silent=%llu gaps=%llu gap-frames=%llu "
                    "cb-max=%.2f backlog-max=%llu behind=%llu) "
                    "audio-hold(frames=%llu held=%.0fms renders=%llu)\n",
                    (unsigned long long)swap_count,
                    (unsigned long long)stats.frames,
                    present_seconds > 0 ? frames / present_seconds : 0.0,
                    (double)stats.present_ns_total / frames / 1e6,
                    (double)stats.present_ns_max / 1e6,
                    (double)stats.frame_ns_total / frames / 1e6,
                    (double)stats.frame_ns_max / 1e6,
                    (unsigned long long)stats.frames_over_20ms,
                    (unsigned long long)stats.frames_over_34ms,
                    (double)stats.flush_ns_total / frames / 1e6,
                    (double)stats.flush_ns_max / 1e6,
                    (double)stats.calls_total / frames,
                    (unsigned long long)stats.calls_max,
                    (double)stats.dispatch_ns_total / frames / 1e6,
                    (double)stats.dispatch_ns_max / 1e6,
                    (double)stats.objc_ns_total / frames / 1e6,
                    (double)stats.audio_ns_total / frames / 1e6,
                    (double)stats.audio_ns_max / 1e6,
                    (double)stats.lock_wait_ns_total / frames / 1e6,
                    (double)stats.lock_wait_ns_max / 1e6,
                    (unsigned long long)stats.lock_waits_total,
                    (unsigned long long)pool_hits,
                    (unsigned long long)pool_misses,
                    !frame_pacer_enabled ? "off" : pacer_half_rate ? "half" : "full",
                    mode_to64, mode_to32,
                    (unsigned long long)worker.starts, worker.start_wait_avg_ms,
                    worker.start_wait_max_ms, worker.op_run_max_ms,
                    worker.queue_depth_max,
                    (unsigned long long)worker.silent_renders,
                    (unsigned long long)worker.timestamp_gaps,
                    (unsigned long long)worker.timestamp_gap_frames,
                    worker.callback_max_ms,
                    (unsigned long long)worker.stream_pending_max,
                    (unsigned long long)worker.stream_behind_renders,
                    (unsigned long long)worker.hold_episodes, worker.hold_ms,
                    (unsigned long long)worker.hold_renders);
            if (getenv("LP32_FRAME_STATS_IMPORTS")) {
                compat_runtime32_report_import_profile(
                    (unsigned)strtoul(getenv("LP32_FRAME_STATS_IMPORTS"), NULL, 0));
            }
            /* LP32_WATCH=name@0xaddr:f|d|b,... prints guest cells with each
               stats line (diagnostic for engine-side clocks and flags). */
            const char *watch = getenv("LP32_WATCH");
            if (watch && watch[0]) {
                char buffer[512];
                strlcpy(buffer, watch, sizeof(buffer));
                char *cursor = buffer;
                fprintf(stderr, "compat32: watch t=%.3f", now_ns / 1e9);
                for (char *item = strsep(&cursor, ","); item; item = strsep(&cursor, ",")) {
                    char *at = strchr(item, '@');
                    char *colon = at ? strchr(at, ':') : NULL;
                    if (!at || !colon) continue;
                    *at = 0;
                    *colon = 0;
                    uint32_t address = (uint32_t)strtoul(at + 1, NULL, 0);
                    if (address < 0x1000 || address >= 0x7f000000) continue;
                    const void *cell = (const void *)(uintptr_t)address;
                    switch (colon[1]) {
                    case 'f': { float v; memcpy(&v, cell, 4); fprintf(stderr, " %s=%g", item, v); break; }
                    case 'd': { uint32_t v; memcpy(&v, cell, 4); fprintf(stderr, " %s=%u", item, v); break; }
                    case 'b': fprintf(stderr, " %s=%u", item, *(const uint8_t *)cell); break;
                    default: break;
                    }
                }
                fputc('\n', stderr);
            }
            uint64_t keep_last = stats.last_swap_ns;
            memset(&stats, 0, sizeof(stats));
            stats.last_swap_ns = keep_last;
        }
    }
    /* The pacer wait is folded into "flush" so bridge time stays
       dispatch - flush. */
    uint64_t flush_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    [[self openGLContext] flushBuffer];
    uint64_t hitch_flush_end = hitch_recorder_enabled ? hitch_now() : 0;
    frame_pacer_wait([self window]);
    uint64_t flush_end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (hitch_recorder_enabled) {
        hitch_frame(swap_count, flush_start, hitch_flush_end, flush_end,
                    frame_pacer_interval_ns,
                    [NSApp isActive] || getenv("LP32_BACKGROUND_TEST"));
    }
    audio_bridge32_note_frame_presented();
    stats.last_flush_ns = flush_end - flush_start;
    stats.flush_ns_total += stats.last_flush_ns;
    if (stats.last_flush_ns > stats.flush_ns_max) stats.flush_ns_max = stats.last_flush_ns;
    if (stats.last_swap_ns) {
        uint64_t present_ns = flush_end - stats.last_swap_ns;
        stats.present_ns_total += present_ns;
        if (present_ns > stats.present_ns_max) stats.present_ns_max = present_ns;
    }
    stats.last_swap_ns = flush_end;
}
- (void)makeContextCurrent
{
    [[self openGLContext] makeCurrentContext];
    NSRect bounds = [self bounds];
    if (bounds.size.width <= 0 || bounds.size.height <= 0) {
        bounds = [game_window_screen() frame];
    }
    const struct lp32_display_layout *display = lp32_profile()->display;
    if (!display || !display->legacy_renderer_display) return;
    uint32_t *legacy_renderer =
        (void *)(uintptr_t)display->legacy_renderer_display;
    if (legacy_renderer[0x48 / 4] == 0 || legacy_renderer[0x4c / 4] == 0) {
        legacy_renderer[0x48 / 4] = (uint32_t)llround(bounds.size.width);
        legacy_renderer[0x4c / 4] = (uint32_t)llround(bounds.size.height);
        fprintf(stderr, "compat32: renderer display size %.0fx%.0f\n",
                bounds.size.width, bounds.size.height);
    }
}
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
@end

@interface GameWindow : NSObject
{
    NSWindow *_fullScreenWindow;
    OpenGLView *_fullScreenOpenGLView;
    NSWindow *_window;
    OpenGLView *_openGLView;
    BOOL _isFullScreen;
}
- (id)initWithFullscreenView:(NSWindow *)window openGLView:(OpenGLView *)view;
- (id)initWithWindowedView:(NSWindow *)window openGLView:(OpenGLView *)view;
- (OpenGLView *)getActiveOpenGLView;
- (NSWindow *)getActiveWindow;
- (BOOL)isFullScreen;
- (BOOL)hasFocus;
- (void)goFullScreen;
- (void)goWindowed;
@end

@implementation GameWindow
- (id)initWithFullscreenView:(NSWindow *)window openGLView:(OpenGLView *)view
{
    self = [super init];
    if (self) {
        _fullScreenWindow = [window retain];
        _fullScreenOpenGLView = [view retain];
        _isFullScreen = YES;
    }
    return self;
}
- (id)initWithWindowedView:(NSWindow *)window openGLView:(OpenGLView *)view
{
    self = [super init];
    if (self) {
        _window = [window retain];
        _openGLView = [view retain];
        _isFullScreen = NO;
    }
    return self;
}
- (OpenGLView *)getActiveOpenGLView { return _isFullScreen ? _fullScreenOpenGLView : _openGLView; }
- (NSWindow *)getActiveWindow { return _isFullScreen ? _fullScreenWindow : _window; }
- (BOOL)isFullScreen { return _isFullScreen; }
- (BOOL)hasFocus
{
    /*
     * The game pauses its simulation and movie player whenever its window
     * stops being key, exactly like the shipped build. Continuing is opt-in
     * via the environment or bundle setting, separate from test automation.
     */
    if (lp32_ignore_guest_focus_loss()) return YES;
    /* Under exclusive fullscreen the game's window was key whenever the
       application was active.  Modern activation is asynchronous and the
       borderless window is not always picked as key, so treat an active
       application as focus; the guest reads key events straight from the
       event queue, so it does not need the window to be key for input. */
    BOOL active = [NSApp isActive];
    BOOL key = [[self getActiveWindow] isKeyWindow];
    /* Test hook: LP32_TEST_FOCUS_LOSS="<lost>,<regained>" (seconds after the
       first call) reports focus lost between those times regardless of the
       real state, to exercise the game's pause/resume path unattended. */
    static double script_lost = -1, script_regained = -1, script_start;
    if (script_lost < 0) {
        const char *script = getenv("LP32_TEST_FOCUS_LOSS");
        script_lost = 0;
        if (script && sscanf(script, "%lf,%lf", &script_lost, &script_regained) == 2) {
            script_start = CFAbsoluteTimeGetCurrent();
        } else {
            script_lost = 0;
        }
    }
    if (script_lost > 0) {
        double elapsed = CFAbsoluteTimeGetCurrent() - script_start;
        BOOL lost = elapsed >= script_lost && elapsed < script_regained;
        /* Mirror what a real deactivation does to the window (it hides on
           deactivate and is re-shown, then clicked, on reactivation). */
        static int scripted_state;
        NSWindow *window = [self getActiveWindow];
        if (lost && scripted_state == 0) {
            scripted_state = 1;
            [window orderOut:nil];
            fprintf(stderr, "compat32: focus script: window hidden\n");
        } else if (!lost && scripted_state == 1) {
            scripted_state = 2;
            [window orderFront:nil];
            NSPoint where = NSMakePoint(NSMidX([[window contentView] bounds]),
                                        NSMidY([[window contentView] bounds]));
            NSTimeInterval stamp = [[NSProcessInfo processInfo] systemUptime];
            NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                location:where modifierFlags:0 timestamp:stamp
                windowNumber:[window windowNumber] context:nil eventNumber:1
                clickCount:1 pressure:1.0];
            NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                location:where modifierFlags:0 timestamp:stamp + 0.05
                windowNumber:[window windowNumber] context:nil eventNumber:2
                clickCount:1 pressure:0.0];
            [NSApp postEvent:down atStart:NO];
            [NSApp postEvent:up atStart:NO];
            fprintf(stderr, "compat32: focus script: window shown and clicked\n");
        }
        active = !lost;
        key = !lost;
    }
    static int last_state = -1;
    int state = (active ? 2 : 0) | (key ? 1 : 0);
    if (state != last_state) {
        fprintf(stderr, "compat32: game focus %s (app %s, window %s)\n",
                active || key ? "gained" : "lost",
                active ? "active" : "inactive", key ? "key" : "not key");
        last_state = state;
    }
    return active || key;
}
- (void)goFullScreen { _isFullScreen = YES; [[self getActiveWindow] makeKeyAndOrderFront:nil]; }
- (void)goWindowed { _isFullScreen = NO; [[self getActiveWindow] makeKeyAndOrderFront:nil]; }
- (void)close { [[self getActiveWindow] close]; }
- (void)dealloc
{
    [_fullScreenWindow release];
    [_fullScreenOpenGLView release];
    [_window release];
    [_openGLView release];
    [super dealloc];
}
@end

int objc_bridge32_run_focus_self_test(int expected)
{
    @autoreleasepool {
        /* Run before AppKit activation: a nil native application/window is
           definitely unfocused. Exercise the real GameWindow pause query. */
        if (NSApp) return -1;
        GameWindow *window = [[GameWindow alloc] initWithWindowedView:nil openGLView:nil];
        int actual = [window hasFocus];
        [window release];
        if (actual != expected || lp32_continue_when_inactive() != expected ||
            lp32_suppress_background_input() != expected) return -1;
        fprintf(stderr, "focus selftest PASS (unfocused GameWindow hasFocus=%d, input suppressed=%d)\n",
                actual, lp32_suppress_background_input());
        return 0;
    }
}

/* Modern host counterpart for Marvel's application delegate. Dispatch its
   two state changes back into the selected image; never terminate over an
   in-progress guest save operation. */
@interface NuMacApplicationDelegate : NSObject <NSApplicationDelegate>
@end
@implementation NuMacApplicationDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    (void)sender;
    uint32_t function = lp32_profile()->application_should_terminate;
    const uint32_t args[] = {0, 0, 0};
    return function ? compat_runtime32_call(function, args, 3) : NSTerminateCancel;
}
- (void)applicationWillUnhide:(NSNotification *)notification
{
    (void)notification;
    uint32_t function = lp32_profile()->application_will_unhide;
    const uint32_t args[] = {0, 0, 0};
    if (function) compat_runtime32_call(function, args, 3);
}
@end

@interface ActivatorAppDelegate : NSObject <NSApplicationDelegate>
{
    NSWindow *_activateOnlineWindow;
    NSWindow *_activateManuallyWindow;
    NSButton *_activateOnlineButton;
    NSButton *_activateManuallyButton;
    NSTextField *_onlineSerialNumber;
    NSTextField *_manualSerialNumber;
    NSTextField *_manualUnlockCode;
    NSTextView *_unlockRequestCode;
    NSTextView *_manualUnlockLink;
    uint32_t _guestObject;
}
@property(nonatomic, retain) IBOutlet NSWindow *activateOnlineWindow;
@property(nonatomic, retain) IBOutlet NSWindow *activateManuallyWindow;
@property(nonatomic, retain) IBOutlet NSButton *activateOnlineButton;
@property(nonatomic, retain) IBOutlet NSButton *activateManuallyButton;
@property(nonatomic, retain) IBOutlet NSTextField *onlineSerialNumber;
@property(nonatomic, retain) IBOutlet NSTextField *manualSerialNumber;
@property(nonatomic, retain) IBOutlet NSTextField *manualUnlockCode;
@property(nonatomic, retain) IBOutlet NSTextView *unlockRequestCode;
@property(nonatomic, retain) IBOutlet NSTextView *manualUnlockLink;
- (IBAction)cancel:(id)sender;
- (IBAction)activateManually:(id)sender;
- (IBAction)activateOnline:(id)sender;
@end

uint32_t objc_bridge32_guest_selector(const char *name)
{
    uint32_t existing = objc_legacy32_selector(name);
    return existing ? existing : intern_guest_cstring(name);
}
static uint32_t guest_selector(const char *name)
{
    return objc_bridge32_guest_selector(name);
}

static uint32_t call_guest_method(uint32_t imp, uint32_t self_handle,
                                  const char *selector_name, id argument)
{
    uint32_t arguments[3] = {
        self_handle,
        guest_selector(selector_name),
        argument ? proxy_for_object(argument) : 0,
    };
    uint32_t count = argument ? 3 : 2;
    uint32_t result = compat_runtime32_call(imp, arguments, count);
    if (compat_runtime32_last_call_trapped()) {
        fprintf(stderr, "compat32: guest Objective-C callback %s escaped\n",
                selector_name);
    }
    return result;
}

static const struct lp32_activator_layout *activator_layout(void)
{
    static const struct lp32_activator_layout none;
    const struct lp32_activator_layout *layout = lp32_profile()->activator;
    return layout ? layout : &none;
}

/* Titles without a guest activation delegate never load its nib, but keep
   the outlets harmless if one is instantiated anyway. */
static uint32_t call_activator_method(uint32_t imp, uint32_t self_handle,
                                      const char *selector_name, id argument)
{
    if (!imp || !self_handle) return 0;
    return call_guest_method(imp, self_handle, selector_name, argument);
}

@implementation ActivatorAppDelegate

@synthesize activateOnlineWindow = _activateOnlineWindow;
@synthesize activateManuallyWindow = _activateManuallyWindow;
@synthesize activateOnlineButton = _activateOnlineButton;
@synthesize activateManuallyButton = _activateManuallyButton;
@synthesize onlineSerialNumber = _onlineSerialNumber;
@synthesize manualSerialNumber = _manualSerialNumber;
@synthesize manualUnlockCode = _manualUnlockCode;
@synthesize unlockRequestCode = _unlockRequestCode;
@synthesize manualUnlockLink = _manualUnlockLink;

- (uint32_t)ensureGuestObject
{
    if (!_guestObject) {
        _guestObject = compat_runtime32_allocate(0x28, 1);
        if (_guestObject) {
            uint32_t *words = (void *)(uintptr_t)_guestObject;
            words[0] = lp32_profile()->activator ? lp32_profile()->activator->app_delegate_isa : 0;
            register_proxy_with_handle(self, _guestObject);
        }
    }
    return _guestObject;
}

- (void)setGuestOutlet:(id)object offset:(uint32_t)offset
{
    uint32_t guest = [self ensureGuestObject];
    if (guest) {
        uint32_t *slot = (void *)(uintptr_t)(guest + offset);
        *slot = proxy_for_object(object);
    }
}

- (void)setActivateOnlineWindow:(NSWindow *)value
{
    if (_activateOnlineWindow != value) {
        [_activateOnlineWindow release];
        _activateOnlineWindow = [value retain];
    }
    [self setGuestOutlet:value offset:0x04];
}

- (void)setActivateManuallyWindow:(NSWindow *)value
{
    if (_activateManuallyWindow != value) {
        [_activateManuallyWindow release];
        _activateManuallyWindow = [value retain];
    }
    [self setGuestOutlet:value offset:0x08];
}

- (void)setActivateOnlineButton:(NSButton *)value
{
    if (_activateOnlineButton != value) {
        [_activateOnlineButton release];
        _activateOnlineButton = [value retain];
    }
    [self setGuestOutlet:value offset:0x0c];
}

- (void)setActivateManuallyButton:(NSButton *)value
{
    if (_activateManuallyButton != value) {
        [_activateManuallyButton release];
        _activateManuallyButton = [value retain];
    }
    [self setGuestOutlet:value offset:0x10];
}

- (void)setOnlineSerialNumber:(NSTextField *)value
{
    if (_onlineSerialNumber != value) {
        [_onlineSerialNumber release];
        _onlineSerialNumber = [value retain];
    }
    [self setGuestOutlet:value offset:0x14];
}

- (void)setManualSerialNumber:(NSTextField *)value
{
    if (_manualSerialNumber != value) {
        [_manualSerialNumber release];
        _manualSerialNumber = [value retain];
    }
    [self setGuestOutlet:value offset:0x18];
}

- (void)setManualUnlockCode:(NSTextField *)value
{
    if (_manualUnlockCode != value) {
        [_manualUnlockCode release];
        _manualUnlockCode = [value retain];
    }
    [self setGuestOutlet:value offset:0x1c];
}

- (void)setUnlockRequestCode:(NSTextView *)value
{
    if (_unlockRequestCode != value) {
        [_unlockRequestCode release];
        _unlockRequestCode = [value retain];
    }
    [self setGuestOutlet:value offset:0x20];
}

- (void)setManualUnlockLink:(NSTextView *)value
{
    if (_manualUnlockLink != value) {
        [_manualUnlockLink release];
        _manualUnlockLink = [value retain];
    }
    [self setGuestOutlet:value offset:0x24];
}

- (void)awakeFromNib
{
    call_activator_method(activator_layout()->awake_from_nib, [self ensureGuestObject],
                      "awakeFromNib", nil);
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    call_activator_method(activator_layout()->did_finish_launching, [self ensureGuestObject],
                      "applicationDidFinishLaunching:", notification);
}

- (void)textDidChange:(NSNotification *)notification
{
    call_activator_method(activator_layout()->text_did_change, [self ensureGuestObject],
                      "textDidChange:", notification);
}

- (IBAction)cancel:(id)sender
{
    call_activator_method(activator_layout()->cancel, [self ensureGuestObject],
                      "cancel:", sender);
}

- (IBAction)activateManually:(id)sender
{
    call_activator_method(activator_layout()->activate_manually, [self ensureGuestObject],
                      "activateManually:", sender);
}

- (IBAction)activateOnline:(id)sender
{
    call_activator_method(activator_layout()->activate_online, [self ensureGuestObject],
                      "activateOnline:", sender);
}

- (void)dealloc
{
    [_activateOnlineWindow release];
    [_activateManuallyWindow release];
    [_activateOnlineButton release];
    [_activateManuallyButton release];
    [_onlineSerialNumber release];
    [_manualSerialNumber release];
    [_manualUnlockCode release];
    [_unlockRequestCode release];
    [_manualUnlockLink release];
    [super dealloc];
}

@end

struct guest_rect {
    float x;
    float y;
    float width;
    float height;
};

static float guest_float_argument(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

enum {
    kGuestBufferStateCapacity = 8192,
    kGuestBufferMappingCapacity = 16,
};

/*
 * GL_BUFFER_FLUSHING_UNMAP_APPLE and mapped storage belong to a buffer
 * object, not to its binding target.  The game rotates several large array
 * buffers through GL_ARRAY_BUFFER and also creates level-specific buffers.
 * Keeping one flush flag per target lets those independent objects inherit
 * one another's policy, either dropping an upload or uploading stale bytes.
 * Buffer state is per object while low-address staging is retained only by a
 * small reusable pool of active-map slots, avoiding a duplicate of every
 * static level VBO in the guest heap.
 */
struct guest_buffer_state {
    CGLShareGroupObj share_group;
    GLuint buffer;
    bool flush_on_unmap;
};

struct guest_buffer_mapping {
    CGLContextObj context;
    GLenum target;
    GLenum access;
    GLbitfield access_flags;
    GLuint buffer;
    size_t buffer_size;
    size_t mapped_offset;
    size_t size;
    uint32_t storage;
    size_t capacity;
    struct guest_buffer_state *state;
    bool range_mapping;
    bool active;
};

static struct guest_buffer_state guest_buffer_states[kGuestBufferStateCapacity];
static struct guest_buffer_mapping
    guest_buffer_mappings[kGuestBufferMappingCapacity];
static bool logged_guest_buffer_state_exhaustion;
static bool logged_guest_buffer_mapping_exhaustion;

/*
 * CGDisplayAvailableModes/CGDisplayCurrentMode stopped returning useful
 * objects after macOS 10.6, but this 2011 title consumes their legacy
 * dictionary representation directly.  Rebuild that representation from
 * CGDisplayMode so the guest sees real resolutions on current macOS.
 */
static NSDictionary *legacy_dictionary_for_display_mode(CGDisplayModeRef mode)
{
    if (!mode) return nil;

    size_t width = CGDisplayModeGetWidth(mode);
    size_t height = CGDisplayModeGetHeight(mode);
    double refresh = CGDisplayModeGetRefreshRate(mode);
    if (refresh < 1.0) refresh = 60.0;
    uint32_t flags = CGDisplayModeGetIOFlags(mode);
    uint32_t mode_id = CGDisplayModeGetIODisplayModeID(mode);

    return [NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithUnsignedLongLong:width], @"Width",
        [NSNumber numberWithUnsignedLongLong:height], @"Height",
        [NSNumber numberWithUnsignedInt:mode_id], @"Mode",
        [NSNumber numberWithInt:32], @"BitsPerPixel",
        [NSNumber numberWithInt:8], @"BitsPerSample",
        [NSNumber numberWithInt:4], @"SamplesPerPixel",
        [NSNumber numberWithDouble:refresh], @"RefreshRate",
        [NSNumber numberWithBool:CGDisplayModeIsUsableForDesktopGUI(mode)],
            @"UsableForDesktopGUI",
        [NSNumber numberWithUnsignedInt:flags], @"IOFlags",
        [NSNumber numberWithUnsignedLongLong:width * 4],
            @"kCGDisplayBytesPerRow",
        [NSNumber numberWithUnsignedInt:mode_id], @"IODisplayModeID",
        [NSNumber numberWithBool:YES], @"kCGDisplayModeIsSafeForHardware",
        [NSNumber numberWithBool:(flags & kDisplayModeInterlacedFlag) != 0],
            @"kCGDisplayModeIsInterlaced",
        [NSNumber numberWithBool:(flags & kDisplayModeStretchedFlag) != 0],
            @"kCGDisplayModeIsStretched",
        [NSNumber numberWithBool:(flags & kDisplayModeTelevisionFlag) != 0],
            @"kCGDisplayModeIsTelevisionOutput",
        nil];
}

static NSArray *legacy_modes_for_display(CGDirectDisplayID display)
{
    static NSMutableDictionary *cache;
    if (!cache) cache = [[NSMutableDictionary alloc] init];
    NSNumber *display_key = [NSNumber numberWithUnsignedInt:display];
    NSArray *cached = [cache objectForKey:display_key];
    if (cached) return cached;

    CFArrayRef modern_modes = CGDisplayCopyAllDisplayModes(display, NULL);
    NSMutableArray *legacy_modes = [NSMutableArray array];
    NSMutableSet *seen_modes = [NSMutableSet set];
    if (modern_modes) {
        CFIndex count = CFArrayGetCount(modern_modes);
        for (CFIndex index = 0; index < count; ++index) {
            CGDisplayModeRef mode =
                (CGDisplayModeRef)CFArrayGetValueAtIndex(modern_modes, index);
            if (!CGDisplayModeIsUsableForDesktopGUI(mode)) continue;
            size_t width = CGDisplayModeGetWidth(mode);
            size_t height = CGDisplayModeGetHeight(mode);
            double refresh = CGDisplayModeGetRefreshRate(mode);
            if (refresh < 1.0) refresh = 60.0;
            NSString *identity = [NSString stringWithFormat:@"%zux%zu@%.3f",
                                  width, height, refresh];
            if ([seen_modes containsObject:identity]) continue;
            NSDictionary *legacy = legacy_dictionary_for_display_mode(mode);
            if (legacy) {
                [legacy_modes addObject:legacy];
                [seen_modes addObject:identity];
            }
        }
        CFRelease(modern_modes);
    }

    /* The legacy API always listed the active mode.  On Retina displays the
       active (scaled) mode is usually absent from CGDisplayCopyAllDisplayModes
       without the duplicate-low-resolution option, and Clone Wars discards
       every mode larger than the active one, so make sure it is present. */
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (current) {
        size_t width = CGDisplayModeGetWidth(current);
        size_t height = CGDisplayModeGetHeight(current);
        double refresh = CGDisplayModeGetRefreshRate(current);
        if (refresh < 1.0) refresh = 60.0;
        NSString *identity = [NSString stringWithFormat:@"%zux%zu@%.3f",
                              width, height, refresh];
        if (![seen_modes containsObject:identity]) {
            NSDictionary *legacy = legacy_dictionary_for_display_mode(current);
            if (legacy) [legacy_modes addObject:legacy];
        }
        CFRelease(current);
    }
    NSArray *result = [NSArray arrayWithArray:legacy_modes];
    [cache setObject:result forKey:display_key];
    return result;
}

static NSDictionary *legacy_current_mode_for_display(CGDirectDisplayID display)
{
    if (guest_display_mode.width > 0 && display == guest_display_mode_display) {
        for (NSDictionary *candidate in legacy_modes_for_display(display)) {
            if ([[candidate objectForKey:@"Width"] doubleValue] == guest_display_mode.width &&
                [[candidate objectForKey:@"Height"] doubleValue] == guest_display_mode.height) {
                return candidate;
            }
        }
    }
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (!current) return nil;
    size_t width = CGDisplayModeGetWidth(current);
    size_t height = CGDisplayModeGetHeight(current);
    double refresh = CGDisplayModeGetRefreshRate(current);
    if (refresh < 1.0) refresh = 60.0;

    NSDictionary *match = nil;
    for (NSDictionary *candidate in legacy_modes_for_display(display)) {
        if ([[candidate objectForKey:@"Width"] unsignedLongLongValue] == width &&
            [[candidate objectForKey:@"Height"] unsignedLongLongValue] == height &&
            fabs([[candidate objectForKey:@"RefreshRate"] doubleValue] - refresh) < 0.01) {
            match = candidate;
            break;
        }
    }
    if (!match) match = legacy_dictionary_for_display_mode(current);
    CFRelease(current);
    return match;
}

static GLenum buffer_binding_for_target(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
#ifdef GL_PIXEL_PACK_BUFFER
    case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
#endif
#ifdef GL_PIXEL_UNPACK_BUFFER
    case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
#endif
    default: return 0;
    }
}

static GLuint bound_buffer_for_target(GLenum target)
{
    GLenum binding = buffer_binding_for_target(target);
    if (!binding) return 0;
    GLint buffer = 0;
    glGetIntegerv(binding, &buffer);
    return buffer > 0 ? (GLuint)buffer : 0;
}

static struct guest_buffer_state *buffer_state_for_name(
    CGLContextObj context, GLuint buffer, bool create)
{
    if (!buffer) return NULL;
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    struct guest_buffer_state *empty = NULL;
    for (size_t index = 0;
         index < sizeof(guest_buffer_states) / sizeof(guest_buffer_states[0]);
         ++index) {
        struct guest_buffer_state *state = &guest_buffer_states[index];
        if (state->buffer == buffer && state->share_group == share_group) {
            return state;
        }
        if (!state->buffer && !empty) empty = state;
    }
    if (create && empty) {
        empty->share_group = share_group;
        empty->buffer = buffer;
        /* GL_APPLE_flush_buffer_range specifies TRUE as the default. */
        empty->flush_on_unmap = true;
        return empty;
    }
    if (create && !logged_guest_buffer_state_exhaustion) {
        fprintf(stderr,
                "compat32: guest buffer state table exhausted at swap %llu\n",
                (unsigned long long)objc_bridge_swap_count);
        logged_guest_buffer_state_exhaustion = true;
    }
    return NULL;
}

static struct guest_buffer_mapping *active_buffer_mapping(
    CGLContextObj context, GLenum target, GLuint buffer)
{
    for (size_t index = 0;
         index < sizeof(guest_buffer_mappings) / sizeof(guest_buffer_mappings[0]);
         ++index) {
        struct guest_buffer_mapping *mapping = &guest_buffer_mappings[index];
        if (mapping->active && mapping->context == context &&
            mapping->target == target && mapping->buffer == buffer) return mapping;
    }
    return NULL;
}

static struct guest_buffer_mapping *begin_buffer_mapping(
    CGLContextObj context, GLenum target, GLuint buffer,
    struct guest_buffer_state *state)
{
    struct guest_buffer_mapping *mapping =
        active_buffer_mapping(context, target, buffer);
    if (mapping) return mapping;
    for (size_t index = 0;
         index < sizeof(guest_buffer_mappings) / sizeof(guest_buffer_mappings[0]);
         ++index) {
        if (!guest_buffer_mappings[index].active) {
            mapping = &guest_buffer_mappings[index];
            uint32_t storage = mapping->storage;
            size_t capacity = mapping->capacity;
            memset(mapping, 0, sizeof(*mapping));
            mapping->storage = storage;
            mapping->capacity = capacity;
            mapping->context = context;
            mapping->target = target;
            mapping->buffer = buffer;
            mapping->state = state;
            mapping->active = true;
            return mapping;
        }
    }
    if (!logged_guest_buffer_mapping_exhaustion) {
        fprintf(stderr,
                "compat32: guest mapped-buffer table exhausted at swap %llu\n",
                (unsigned long long)objc_bridge_swap_count);
        logged_guest_buffer_mapping_exhaustion = true;
    }
    return NULL;
}

static void deactivate_buffer_mapping(struct guest_buffer_mapping *mapping)
{
    if (!mapping) return;
    mapping->context = NULL;
    mapping->target = 0;
    mapping->access = 0;
    mapping->access_flags = 0;
    mapping->buffer = 0;
    mapping->buffer_size = 0;
    mapping->mapped_offset = 0;
    mapping->size = 0;
    mapping->state = NULL;
    mapping->range_mapping = false;
    mapping->active = false;
}

static void discard_buffer_state(CGLContextObj context, GLuint buffer)
{
    if (!buffer) return;
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    for (size_t index = 0;
         index < sizeof(guest_buffer_mappings) / sizeof(guest_buffer_mappings[0]);
         ++index) {
        if (guest_buffer_mappings[index].active &&
            guest_buffer_mappings[index].buffer == buffer &&
            guest_buffer_mappings[index].state &&
            guest_buffer_mappings[index].state->share_group == share_group) {
            deactivate_buffer_mapping(&guest_buffer_mappings[index]);
        }
    }
    for (size_t index = 0;
         index < sizeof(guest_buffer_states) / sizeof(guest_buffer_states[0]);
         ++index) {
        struct guest_buffer_state *state = &guest_buffer_states[index];
        if (state->buffer != buffer || state->share_group != share_group) continue;
        memset(state, 0, sizeof(*state));
    }
}

static bool trace_gl_buffer_call(void)
{
    static bool initialized;
    static uint64_t first_swap;
    static unsigned int remaining;
    if (!initialized) {
        const char *value = getenv("LP32_TRACE_GL_BUFFERS");
        if (value) {
            first_swap = strtoull(value, NULL, 0);
            remaining = 1000;
        }
        initialized = true;
    }
    if (!remaining || objc_bridge_swap_count < first_swap) return false;
    --remaining;
    return true;
}

static bool gl_render_trace_initialized;
static uint64_t gl_render_trace_first_swap;
static unsigned int gl_render_trace_remaining;
static unsigned int gl_render_trace_limit = 20000;
static uint64_t gl_render_trace_generation;

enum { kTracedTextureCapacity = 4096 };
struct traced_texture_entry {
    CGLShareGroupObj share_group;
    GLenum target;
    GLuint texture;
    uint64_t generation;
};
static struct traced_texture_entry
    traced_textures[kTracedTextureCapacity];

static void initialize_gl_render_trace(void)
{
    if (!gl_render_trace_initialized) {
        const char *value = getenv("LP32_TRACE_GL_RENDER");
        const char *limit = getenv("LP32_TRACE_GL_RENDER_LIMIT");
        unsigned long parsed_limit = limit ? strtoul(limit, NULL, 0) : 0;
        if (parsed_limit > 0 && parsed_limit <= UINT_MAX) {
            gl_render_trace_limit = (unsigned int)parsed_limit;
        }
        if (value) {
            gl_render_trace_first_swap = strtoull(value, NULL, 0);
            gl_render_trace_remaining = gl_render_trace_limit;
        }
        gl_render_trace_initialized = true;
    }
}

static void arm_pending_gl_trace(void)
{
    initialize_gl_render_trace();
    if (!pending_gl_trace_arm) return;
    pending_gl_trace_arm = 0;
    gl_render_trace_first_swap = objc_bridge_swap_count;
    gl_render_trace_remaining = gl_render_trace_limit;
    ++gl_render_trace_generation;
    if (!gl_render_trace_generation) ++gl_render_trace_generation;
    memset(traced_textures, 0, sizeof(traced_textures));
    gl_trace_printf(
        "compat32: GL render trace armed at swap %llu for %u calls\n",
        (unsigned long long)objc_bridge_swap_count,
        gl_render_trace_remaining);
}

static bool trace_gl_render_call(void)
{
    initialize_gl_render_trace();
    arm_pending_gl_trace();
    if (!gl_render_trace_remaining ||
        objc_bridge_swap_count < gl_render_trace_first_swap) return false;
    --gl_render_trace_remaining;
    return true;
}

static void trace_gl_frame_boundary(void)
{
    static bool initialized;
    static uint64_t interval;
    if (!initialized) {
        const char *value = getenv("LP32_TRACE_GL_FRAMES");
        if (value) {
            unsigned long long parsed = strtoull(value, NULL, 0);
            interval = parsed > 0 ? parsed : 30;
        }
        initialized = true;
    }
    if (interval &&
        (objc_bridge_swap_count <= 10 || objc_bridge_swap_count % interval == 0)) {
        GLint framebuffer = 0;
        GLint framebuffer_status = GL_FRAMEBUFFER_COMPLETE;
        GLint viewport[4] = {0, 0, 0, 0};
        GLint scissor[4] = {0, 0, 0, 0};
        GLint draw_buffer = 0;
        GLint read_buffer = 0;
        GLint active_texture = 0;
        GLint texture_2d = 0;
        GLboolean color_mask[4] = {0, 0, 0, 0};
        GLboolean depth_mask = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        if (framebuffer) {
            framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        }
        fprintf(stderr,
                "compat32: GL frame swap=%llu draws=%llu clears=%llu "
                "fboBinds=%llu fboAttach=%llu blits=%llu texBinds=%llu "
                "texUploads=%llu progBinds=%llu params=%llu fbo=%d "
                "status=%04x viewport={%d,%d,%d,%d} scissor={%d,%d,%d,%d} "
                "drawbuf=%04x readbuf=%04x activeTex=%04x tex2d=%d "
                "colorMask=%d%d%d%d depthMask=%d vp=%u fp=%u\n",
                (unsigned long long)objc_bridge_swap_count,
                (unsigned long long)gl_frame_diagnostics.draw_calls,
                (unsigned long long)gl_frame_diagnostics.clear_calls,
                (unsigned long long)gl_frame_diagnostics.framebuffer_binds,
                (unsigned long long)gl_frame_diagnostics.framebuffer_attachments,
                (unsigned long long)gl_frame_diagnostics.framebuffer_blits,
                (unsigned long long)gl_frame_diagnostics.texture_binds,
                (unsigned long long)gl_frame_diagnostics.texture_uploads,
                (unsigned long long)gl_frame_diagnostics.program_binds,
                (unsigned long long)gl_frame_diagnostics.parameter_uploads,
                framebuffer, framebuffer_status,
                viewport[0], viewport[1], viewport[2], viewport[3],
                scissor[0], scissor[1], scissor[2], scissor[3],
                draw_buffer, read_buffer, active_texture, texture_2d,
                color_mask[0], color_mask[1], color_mask[2], color_mask[3],
                depth_mask, trace_vertex_program, trace_fragment_program);
        fflush(stderr);
    }
    memset(&gl_frame_diagnostics, 0, sizeof(gl_frame_diagnostics));
}

/*
 * Reports an ARB program the driver rejected.  Reads only the error position
 * (a glGetIntegerv query), never glGetError, so the game's own error check
 * after glProgramStringARB still sees the failure.
 */
static void report_arb_program_load_failure(GLenum target, const void *source,
                                            GLsizei source_size)
{
    GLint position = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &position);
    if (position < 0) return;
    static unsigned int reports;
    if (reports >= 16) return;
    ++reports;
    const GLubyte *message = glGetString(GL_PROGRAM_ERROR_STRING_ARB);
    const char *text = source;
    int line = 1;
    for (GLint index = 0; index < position && index < source_size; ++index) {
        if (text[index] == '\n') ++line;
    }
    GLuint identifier = target == GL_VERTEX_PROGRAM_ARB ? trace_vertex_program :
                        target == GL_FRAGMENT_PROGRAM_ARB ? trace_fragment_program : 0;
    fprintf(stderr, "compat32: ARB program rejected target=%04x id=%u bytes=%d "
            "position=%d line=%d: %s\n", target, identifier, source_size,
            position, line, message ? (const char *)message : "(no message)");
    if (position < source_size) {
        const char *start = text + position;
        while (start > text && start[-1] != '\n') --start;
        const char *end = memchr(start, '\n', (size_t)(source_size - (start - text)));
        fprintf(stderr, "compat32:   %.*s\n",
                (int)((end ? end : text + source_size) - start), start);
    }
}

static void maybe_dump_gl_program(GLenum target, GLsizei length,
                                  const void *program)
{
    initialize_gl_trace_output();
    const char *directory = getenv("LP32_DUMP_GL_PROGRAMS");
    if (!directory || !*directory) directory = gl_program_dump_directory;
    if (!directory || !*directory || length < 0 || !program) return;

    if (mkdir(directory, 0700) != 0) {
        struct stat status;
        if (stat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
            gl_trace_printf(
                "compat32: cannot create GL program dump directory %s\n",
                directory);
            return;
        }
    }

    GLuint identifier = 0;
    if (target == GL_VERTEX_PROGRAM_ARB) {
        identifier = trace_vertex_program;
    } else if (target == GL_FRAGMENT_PROGRAM_ARB) {
        identifier = trace_fragment_program;
    }

    char path[1024];
    int path_length = snprintf(path, sizeof(path),
                               "%s/program-%04x-%u.arb", directory,
                               target, identifier);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        gl_trace_printf("compat32: GL program dump path is too long\n");
        return;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        gl_trace_printf("compat32: cannot open GL program dump %s\n", path);
        return;
    }
    size_t expected = (size_t)length;
    size_t written = fwrite(program, 1, expected, file);
    if (fclose(file) != 0 || written != expected) {
        gl_trace_printf("compat32: incomplete GL program dump %s\n", path);
    }
}

static void trace_program_parameter_upload(const char *kind, GLenum target,
                                           GLuint index, GLsizei count,
                                           const GLfloat *values)
{
    ++gl_frame_diagnostics.parameter_uploads;
    if (!trace_gl_render_call()) return;
    bool finite = true;
    size_t value_count = count > 0 ? (size_t)count * 4 : 0;
    size_t first_nonfinite = SIZE_MAX;
    float largest_magnitude = 0.0f;
    for (size_t offset = 0; offset < value_count; ++offset) {
        if (!isfinite(values[offset])) {
            finite = false;
            if (first_nonfinite == SIZE_MAX) first_nonfinite = offset;
        } else if (fabsf(values[offset]) > largest_magnitude) {
            largest_magnitude = fabsf(values[offset]);
        }
    }
    uint32_t nonfinite_bits = 0;
    if (first_nonfinite != SIZE_MAX) {
        memcpy(&nonfinite_bits, values + first_nonfinite,
               sizeof(nonfinite_bits));
    }
    const GLfloat *last = value_count >= 4 ? values + value_count - 4 : NULL;
    gl_trace_printf(
        "compat32: %s swap=%llu target=%04x index=%u count=%d "
        "vp=%u fp=%u finite=%d bad=%zu badbits=%08x maxabs=%.7g "
        "first={%.7g,%.7g,%.7g,%.7g} "
        "last={%.7g,%.7g,%.7g,%.7g}\n",
        kind, (unsigned long long)objc_bridge_swap_count, target, index,
        count, trace_vertex_program, trace_fragment_program, finite,
        first_nonfinite, nonfinite_bits, largest_magnitude,
        value_count >= 4 ? values[0] : 0.0f,
        value_count >= 4 ? values[1] : 0.0f,
        value_count >= 4 ? values[2] : 0.0f,
        value_count >= 4 ? values[3] : 0.0f,
        last ? last[0] : 0.0f, last ? last[1] : 0.0f,
        last ? last[2] : 0.0f, last ? last[3] : 0.0f);
}

/*
 * The game was built around GL_EXT_gpu_program_parameters and deliberately
 * submits whole Cg constant banks in one call.  Preserve that atomic update:
 * splitting a bank into individual ARB calls has produced corrupt legacy
 * program state in Apple's current compatibility renderer.
 *
 * Old Cg parameter banks also contain NaN/Inf sentinels in unused vector
 * lanes.  Those lanes were benign on the period NVIDIA/ATI drivers, but are
 * undefined input to the current backend and can contaminate position or HDR
 * calculations when a generated program unexpectedly consumes one.  Replace
 * only non-finite lanes; ordinary values, including very large finite matrix
 * coefficients, remain bit-for-bit unchanged.
 */
enum { kProgramParameterScratchFloats = 4096 };
static _Thread_local GLfloat
    program_parameter_scratch[kProgramParameterScratchFloats];
static unsigned int program_parameter_sanitize_reports;

static bool preserve_nonfinite_program_parameters(void)
{
    static int initialized;
    static bool preserve;
    if (!initialized) {
        preserve = getenv("LP32_PRESERVE_NONFINITE_GL_PARAMETERS") != NULL;
        initialized = 1;
    }
    return preserve;
}

static bool split_program_parameter_batches(void)
{
    static int initialized;
    static bool split;
    if (!initialized) {
        split = getenv("LP32_SPLIT_GL_PARAMETER_BATCH") != NULL;
        initialized = 1;
    }
    return split;
}

static bool guard_undefined_arb_math(void)
{
    static bool initialized;
    static bool enabled;
    if (!initialized) {
        enabled = getenv("LP32_DISABLE_ARB_MATH_GUARD") == NULL &&
            getenv("LP32_DISABLE_ARB_RSQ_GUARD") == NULL;
        initialized = true;
    }
    return enabled;
}

static const GLfloat *safe_program_parameter_values(
    const GLfloat *values, GLsizei count, size_t *replaced)
{
    *replaced = 0;
    if (!values || count <= 0 || preserve_nonfinite_program_parameters()) {
        return values;
    }

    size_t value_count = (size_t)count * 4;
    if (value_count > kProgramParameterScratchFloats) {
        if (program_parameter_sanitize_reports < 8) {
            fprintf(stderr,
                    "compat32: refusing oversized GL parameter sanitize "
                    "count=%d\n", count);
            ++program_parameter_sanitize_reports;
        }
        return values;
    }

    size_t first_nonfinite = SIZE_MAX;
    for (size_t offset = 0; offset < value_count; ++offset) {
        if (!isfinite(values[offset])) {
            first_nonfinite = offset;
            break;
        }
    }
    if (first_nonfinite == SIZE_MAX) return values;

    memcpy(program_parameter_scratch, values,
           value_count * sizeof(*values));
    for (size_t offset = first_nonfinite; offset < value_count; ++offset) {
        if (!isfinite(program_parameter_scratch[offset])) {
            program_parameter_scratch[offset] = 0.0f;
            ++*replaced;
        }
    }
    return program_parameter_scratch;
}

static void report_sanitized_program_parameters(
    const char *kind, GLenum target, GLuint index, GLsizei count,
    size_t replaced)
{
    if (!replaced || program_parameter_sanitize_reports >= 8) return;
    fprintf(stderr,
            "compat32: sanitized %zu non-finite %s component%s "
            "swap=%llu target=%04x index=%u count=%d vp=%u fp=%u\n",
            replaced, kind, replaced == 1 ? "" : "s",
            (unsigned long long)objc_bridge_swap_count, target, index, count,
            trace_vertex_program, trace_fragment_program);
    ++program_parameter_sanitize_reports;
}

static size_t gl_attribute_component_size(GLenum type)
{
    switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
        case GL_HALF_FLOAT: return 2;
        case GL_INT:
        case GL_UNSIGNED_INT:
        case GL_FLOAT:
        case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
        case GL_DOUBLE: return 8;
        default: return 0;
    }
}

static size_t gl_index_component_size(GLenum type)
{
    switch (type) {
        case GL_UNSIGNED_BYTE: return 1;
        case GL_UNSIGNED_SHORT: return 2;
        case GL_UNSIGNED_INT: return 4;
        default: return 0;
    }
}

static double gl_half_to_double(uint16_t bits)
{
    unsigned int sign = bits >> 15;
    unsigned int exponent = (bits >> 10) & 0x1f;
    unsigned int fraction = bits & 0x3ff;
    double value;

    if (exponent == 0) {
        value = fraction ? ldexp((double)fraction, -24) : 0.0;
    } else if (exponent == 0x1f) {
        value = fraction ? NAN : INFINITY;
    } else {
        value = ldexp(1.0 + (double)fraction / 1024.0,
                      (int)exponent - 15);
    }
    return sign ? -value : value;
}

static uint64_t trace_hash_bytes(uint64_t hash, const void *bytes,
                                 size_t byte_count)
{
    const unsigned char *cursor = bytes;
    for (size_t index = 0; index < byte_count; ++index) {
        hash ^= cursor[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static GLuint read_index_value(const unsigned char *bytes, GLenum type,
                               size_t index)
{
    switch (type) {
        case GL_UNSIGNED_BYTE: return bytes[index];
        case GL_UNSIGNED_SHORT: {
            uint16_t value;
            memcpy(&value, bytes + index * sizeof(value), sizeof(value));
            return value;
        }
        case GL_UNSIGNED_INT: {
            uint32_t value;
            memcpy(&value, bytes + index * sizeof(value), sizeof(value));
            return value;
        }
        default: return 0;
    }
}

struct gl_index_diagnostic {
    GLuint minimum;
    GLuint maximum;
    bool readable;
    bool in_bounds;
};

static struct gl_index_diagnostic trace_index_buffer(
    GLsizei count, GLenum type, uintptr_t offset, GLuint advertised_start,
    GLuint advertised_end, GLint element_buffer)
{
    struct gl_index_diagnostic diagnostic = {
        .minimum = advertised_start,
        .maximum = advertised_end,
        .readable = false,
        .in_bounds = false,
    };
    size_t component_size = gl_index_component_size(type);
    GLint buffer_size = 0;
    if (element_buffer) {
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE,
                               &buffer_size);
    }
    bool byte_count_valid = count >= 0 && component_size != 0 &&
        (size_t)count <= SIZE_MAX / component_size;
    size_t byte_count = byte_count_valid ?
        (size_t)count * component_size : 0;
    diagnostic.in_bounds = element_buffer && buffer_size >= 0 &&
        byte_count_valid && offset <= (size_t)buffer_size &&
        byte_count <= (size_t)buffer_size - offset;

    unsigned char *indices = NULL;
    if (diagnostic.in_bounds && byte_count && byte_count <= 8 * 1024 * 1024) {
        indices = malloc(byte_count);
        if (indices) {
            glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)offset,
                               (GLsizeiptr)byte_count, indices);
            diagnostic.minimum = UINT_MAX;
            diagnostic.maximum = 0;
            for (GLsizei index = 0; index < count; ++index) {
                GLuint value = read_index_value(indices, type, (size_t)index);
                if (value < diagnostic.minimum) diagnostic.minimum = value;
                if (value > diagnostic.maximum) diagnostic.maximum = value;
            }
            if (count == 0) diagnostic.minimum = diagnostic.maximum = 0;
            diagnostic.readable = true;
        }
    }
    free(indices);

    gl_trace_printf(
        "compat32: index-state swap=%llu ibo=%d type=%04x offset=%zu "
        "count=%d bytes=%zu bufferSize=%d inBounds=%d readable=%d "
        "actual={%u,%u} advertised={%u,%u} advertisedContains=%d\n",
        (unsigned long long)objc_bridge_swap_count, element_buffer, type,
        (size_t)offset, count, byte_count, buffer_size,
        diagnostic.in_bounds, diagnostic.readable, diagnostic.minimum,
        diagnostic.maximum, advertised_start, advertised_end,
        !diagnostic.readable ||
            (diagnostic.minimum >= advertised_start &&
             diagnostic.maximum <= advertised_end));
    return diagnostic;
}

static void trace_vertex_attributes(GLuint minimum, GLuint maximum)
{
    GLint previous_array_buffer = 0;
    GLint maximum_attributes = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_array_buffer);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maximum_attributes);
    if (maximum_attributes < 0) maximum_attributes = 0;
    if (maximum_attributes > 32) maximum_attributes = 32;

    for (GLuint index = 0; index < (GLuint)maximum_attributes; ++index) {
        GLint enabled = 0;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;

        GLint size = 0;
        GLint type = 0;
        GLint normalized = 0;
        GLint stride = 0;
        GLint buffer = 0;
        GLvoid *raw_pointer = NULL;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                            &normalized);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                            &buffer);
        glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                  &raw_pointer);

        size_t component_size = gl_attribute_component_size((GLenum)type);
        size_t element_size = size > 0 && component_size &&
            (size_t)size <= SIZE_MAX / component_size ?
            (size_t)size * component_size : 0;
        size_t effective_stride = stride > 0 ? (size_t)stride : element_size;
        size_t pointer = (size_t)(uintptr_t)raw_pointer;
        GLint buffer_size = 0;
        if (buffer > 0) {
            glBindBuffer(GL_ARRAY_BUFFER, (GLuint)buffer);
            glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE,
                                   &buffer_size);
        }

        bool range_valid = maximum >= minimum && element_size &&
            effective_stride &&
            maximum <= (SIZE_MAX - pointer - element_size) /
                           effective_stride;
        size_t final_byte = range_valid ?
            pointer + (size_t)maximum * effective_stride + element_size : 0;
        bool in_bounds = buffer > 0 && buffer_size >= 0 && range_valid &&
            final_byte <= (size_t)buffer_size;

        size_t nonfinite = 0;
        double minimum_value = INFINITY;
        double maximum_value = -INFINITY;
        float maximum_magnitude = 0.0f;
        unsigned int minimum_byte = UINT_MAX;
        unsigned int maximum_byte = 0;
        uint64_t content_hash = UINT64_C(1469598103934665603);
        bool scanned = false;
        if (in_bounds && size > 0 && size <= 4) {
            size_t first_byte = pointer + (size_t)minimum * effective_stride;
            size_t scan_size = final_byte - first_byte;
            if (scan_size && scan_size <= 2 * 1024 * 1024) {
                unsigned char *data = malloc(scan_size);
                if (data) {
                    glGetBufferSubData(GL_ARRAY_BUFFER,
                                       (GLintptr)first_byte,
                                       (GLsizeiptr)scan_size, data);
                    for (GLuint vertex = minimum; vertex <= maximum; ++vertex) {
                        size_t relative =
                            (size_t)(vertex - minimum) * effective_stride;
                        content_hash = trace_hash_bytes(
                            content_hash, data + relative, element_size);
                        if (type == GL_FLOAT || type == GL_HALF_FLOAT) {
                            for (GLint component = 0; component < size;
                                 ++component) {
                                double value;
                                if (type == GL_FLOAT) {
                                    float float_value;
                                    memcpy(&float_value,
                                           data + relative +
                                               (size_t)component *
                                                   sizeof(float_value),
                                           sizeof(float_value));
                                    value = float_value;
                                } else {
                                    uint16_t half_value;
                                    memcpy(&half_value,
                                           data + relative +
                                               (size_t)component *
                                                   sizeof(half_value),
                                           sizeof(half_value));
                                    value = gl_half_to_double(half_value);
                                }
                                if (!isfinite(value)) {
                                    ++nonfinite;
                                    continue;
                                }
                                if (value < minimum_value) minimum_value = value;
                                if (value > maximum_value) maximum_value = value;
                                if (fabs(value) > maximum_magnitude) {
                                    maximum_magnitude = (float)fabs(value);
                                }
                            }
                        } else if (type == GL_UNSIGNED_BYTE ||
                                   type == GL_BYTE) {
                            for (size_t component = 0;
                                 component < element_size; ++component) {
                                unsigned int value = data[relative + component];
                                if (value < minimum_byte) minimum_byte = value;
                                if (value > maximum_byte) maximum_byte = value;
                            }
                        }
                        if (vertex == UINT_MAX) break;
                    }
                    scanned = true;
                    free(data);
                }
            }
        }

        gl_trace_printf(
            "compat32: attrib-state swap=%llu index=%u size=%d type=%04x "
            "normalized=%d stride=%d effectiveStride=%zu pointer=%zu "
            "buffer=%d bufferSize=%d vertices={%u,%u} finalByte=%zu "
            "inBounds=%d contentScan=%d nonfinite=%zu min=%.9g max=%.9g "
            "maxabs=%.7g byteRange={%u,%u} hash=%016llx\n",
            (unsigned long long)objc_bridge_swap_count, index, size, type,
            normalized, stride, effective_stride, pointer, buffer,
            buffer_size, minimum, maximum, final_byte, in_bounds, scanned,
            nonfinite, isfinite(minimum_value) ? minimum_value : 0.0,
            isfinite(maximum_value) ? maximum_value : 0.0,
            maximum_magnitude, minimum_byte == UINT_MAX ? 0 : minimum_byte,
            maximum_byte, (unsigned long long)(scanned ? content_hash : 0));
    }
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous_array_buffer);
}

static bool mark_texture_for_trace(GLenum target, GLuint texture)
{
    if (!texture || !gl_render_trace_generation) return false;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    size_t start = ((size_t)texture * 2654435761u + (size_t)target) %
        kTracedTextureCapacity;
    for (size_t probe = 0; probe < kTracedTextureCapacity; ++probe) {
        struct traced_texture_entry *entry =
            &traced_textures[(start + probe) % kTracedTextureCapacity];
        if (entry->generation != gl_render_trace_generation) {
            entry->share_group = share_group;
            entry->target = target;
            entry->texture = texture;
            entry->generation = gl_render_trace_generation;
            return true;
        }
        if (entry->share_group == share_group && entry->target == target &&
            entry->texture == texture) {
            return false;
        }
    }
    return false;
}

static bool texture_filter_needs_mipmaps(GLint filter)
{
    return filter == GL_NEAREST_MIPMAP_NEAREST ||
        filter == GL_LINEAR_MIPMAP_NEAREST ||
        filter == GL_NEAREST_MIPMAP_LINEAR ||
        filter == GL_LINEAR_MIPMAP_LINEAR;
}

static void trace_texture_2d_content(GLint unit, GLuint texture,
                                     GLint min_filter)
{
    if (!mark_texture_for_trace(GL_TEXTURE_2D, texture)) return;

    GLint base_level = 0;
    GLint maximum_level = 0;
    GLint wrap_s = 0;
    GLint wrap_t = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base_level);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maximum_level);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);

    GLint base_width = 0;
    GLint base_height = 0;
    GLint base_format = 0;
    GLint base_compressed = 0;
    GLint compressed_size = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level, GL_TEXTURE_WIDTH,
                             &base_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level, GL_TEXTURE_HEIGHT,
                             &base_height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level,
                             GL_TEXTURE_INTERNAL_FORMAT, &base_format);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level, GL_TEXTURE_COMPRESSED,
                             &base_compressed);
    if (base_compressed) {
        glGetTexLevelParameteriv(GL_TEXTURE_2D, base_level,
                                 GL_TEXTURE_COMPRESSED_IMAGE_SIZE,
                                 &compressed_size);
    }

    GLint required_last = base_level;
    if (texture_filter_needs_mipmaps(min_filter)) {
        GLint dimension = base_width > base_height ? base_width : base_height;
        while (dimension > 1 && required_last < maximum_level) {
            dimension >>= 1;
            ++required_last;
        }
    }
    GLint expected_width = base_width;
    GLint expected_height = base_height;
    GLint first_missing = -1;
    GLint first_wrong_size = -1;
    unsigned int levels_present = 0;
    for (GLint level = base_level; level <= required_last; ++level) {
        GLint width = 0;
        GLint height = 0;
        GLint format = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH,
                                 &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT,
                                 &height);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level,
                                 GL_TEXTURE_INTERNAL_FORMAT, &format);
        if (width <= 0 || height <= 0 || !format) {
            if (first_missing < 0) first_missing = level;
        } else {
            ++levels_present;
            if ((width != expected_width || height != expected_height ||
                 format != base_format) && first_wrong_size < 0) {
                first_wrong_size = level;
            }
        }
        if (expected_width > 1) expected_width >>= 1;
        if (expected_height > 1) expected_height >>= 1;
    }

    uint64_t hash = 0;
    if (base_compressed && compressed_size > 0 &&
        compressed_size <= 16 * 1024 * 1024) {
        void *bytes = malloc((size_t)compressed_size);
        if (bytes) {
            GLint previous_pack_buffer = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,
                          &previous_pack_buffer);
            if (previous_pack_buffer) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            }
            glGetCompressedTexImage(GL_TEXTURE_2D, base_level, bytes);
            if (previous_pack_buffer) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER,
                             (GLuint)previous_pack_buffer);
            }
            hash = trace_hash_bytes(UINT64_C(1469598103934665603), bytes,
                                    (size_t)compressed_size);
            free(bytes);
        }
    }
    gl_trace_printf(
        "compat32: texture-content swap=%llu unit=%d target=0de1 tex=%u "
        "base=%d max=%d requiredLast=%d levelsPresent=%u missing=%d "
        "wrongSize=%d baseSize=%dx%d format=%04x compressed=%d "
        "compressedBytes=%d min=%04x wrap={%04x,%04x} hash=%016llx\n",
        (unsigned long long)objc_bridge_swap_count, unit, texture,
        base_level, maximum_level, required_last, levels_present,
        first_missing, first_wrong_size, base_width, base_height, base_format,
        base_compressed, compressed_size, min_filter, wrap_s, wrap_t,
        (unsigned long long)hash);
}

static void trace_texture_cube_content(GLint unit, GLuint texture,
                                       GLint min_filter)
{
    if (!mark_texture_for_trace(GL_TEXTURE_CUBE_MAP, texture)) return;

    GLint base_level = 0;
    GLint maximum_level = 0;
    GLint wrap_s = 0;
    GLint wrap_t = 0;
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL,
                        &base_level);
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL,
                        &maximum_level);
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, &wrap_s);
    glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, &wrap_t);

    GLint base_width = 0;
    GLint base_height = 0;
    GLint base_format = 0;
    GLint base_compressed = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, base_level,
                             GL_TEXTURE_WIDTH, &base_width);
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, base_level,
                             GL_TEXTURE_HEIGHT, &base_height);
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, base_level,
                             GL_TEXTURE_INTERNAL_FORMAT, &base_format);
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, base_level,
                             GL_TEXTURE_COMPRESSED, &base_compressed);

    GLint required_last = base_level;
    if (texture_filter_needs_mipmaps(min_filter)) {
        GLint dimension = base_width > base_height ? base_width : base_height;
        while (dimension > 1 && required_last < maximum_level) {
            dimension >>= 1;
            ++required_last;
        }
    }
    GLint expected_width = base_width;
    GLint expected_height = base_height;
    GLint first_missing = -1;
    GLint first_wrong_size = -1;
    unsigned int levels_present = 0;
    for (GLint level = base_level; level <= required_last; ++level) {
        bool complete_level = true;
        for (unsigned int face = 0; face < 6; ++face) {
            GLenum face_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
            GLint width = 0;
            GLint height = 0;
            GLint format = 0;
            glGetTexLevelParameteriv(face_target, level, GL_TEXTURE_WIDTH,
                                     &width);
            glGetTexLevelParameteriv(face_target, level, GL_TEXTURE_HEIGHT,
                                     &height);
            glGetTexLevelParameteriv(face_target, level,
                                     GL_TEXTURE_INTERNAL_FORMAT, &format);
            if (width <= 0 || height <= 0 || !format) {
                if (first_missing < 0) first_missing = level;
                complete_level = false;
            } else if (width != expected_width ||
                       height != expected_height || format != base_format) {
                if (first_wrong_size < 0) first_wrong_size = level;
                complete_level = false;
            }
        }
        if (complete_level) ++levels_present;
        if (expected_width > 1) expected_width >>= 1;
        if (expected_height > 1) expected_height >>= 1;
    }

    uint64_t hash = UINT64_C(1469598103934665603);
    GLint compressed_size = 0;
    if (base_compressed) {
        GLint previous_pack_buffer = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pack_buffer);
        if (previous_pack_buffer) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        for (unsigned int face = 0; face < 6; ++face) {
            GLenum face_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
            GLint face_size = 0;
            glGetTexLevelParameteriv(face_target, base_level,
                                     GL_TEXTURE_COMPRESSED_IMAGE_SIZE,
                                     &face_size);
            if (face_size <= 0 || face_size > 16 * 1024 * 1024) continue;
            void *bytes = malloc((size_t)face_size);
            if (!bytes) continue;
            glGetCompressedTexImage(face_target, base_level, bytes);
            hash = trace_hash_bytes(hash, bytes, (size_t)face_size);
            compressed_size += face_size;
            free(bytes);
        }
        if (previous_pack_buffer) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)previous_pack_buffer);
        }
    } else {
        hash = 0;
    }

    gl_trace_printf(
        "compat32: texture-content swap=%llu unit=%d target=8513 tex=%u "
        "base=%d max=%d requiredLast=%d levelsPresent=%u missing=%d "
        "wrongSize=%d baseSize=%dx%d format=%04x compressed=%d "
        "compressedBytes=%d min=%04x wrap={%04x,%04x} hash=%016llx\n",
        (unsigned long long)objc_bridge_swap_count, unit, texture,
        base_level, maximum_level, required_last, levels_present,
        first_missing, first_wrong_size, base_width, base_height, base_format,
        base_compressed, compressed_size, min_filter, wrap_s, wrap_t,
        (unsigned long long)hash);
}

enum { kMipmapRepairCapacity = 8192 };
struct mipmap_repair_entry {
    CGLShareGroupObj share_group;
    GLenum target;
    GLuint texture;
    GLint requested_maximum;
    GLint compatibility_maximum;
    /* Bumped whenever an upload may add or resize one of this texture's
       levels.  The last full scan recorded the value it saw; while the two
       match, repeated parameter writes can reuse that scan. */
    uint64_t level_generation;
    uint64_t validated_generation;
    GLint validated_min_filter;
    GLint validated_base_level;
    bool occupied;
    bool clamped;
};
static struct mipmap_repair_entry mipmap_repairs[kMipmapRepairCapacity];
static pthread_mutex_t mipmap_repair_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int mipmap_repair_reports;

/* Level generations come from one counter, so an entry recreated for a
   reused texture name never matches a scan of the deleted texture. */
static uint64_t mipmap_generation_counter;

static size_t mipmap_repair_home(CGLShareGroupObj share_group, GLenum target,
                                 GLuint texture)
{
    return ((size_t)texture * 2654435761u + (size_t)target * 40503u +
            ((uintptr_t)share_group >> 4)) % kMipmapRepairCapacity;
}

/* Callers hold mipmap_repair_mutex. Deletion moves entries, so a returned
   pointer is valid only until the mutex is released. */
static struct mipmap_repair_entry *mipmap_repair_entry_for(
    CGLShareGroupObj share_group, GLenum target, GLuint texture, bool create)
{
    size_t start = mipmap_repair_home(share_group, target, texture);
    for (size_t probe = 0; probe < kMipmapRepairCapacity; ++probe) {
        struct mipmap_repair_entry *entry =
            &mipmap_repairs[(start + probe) % kMipmapRepairCapacity];
        if (!entry->occupied) {
            if (!create) return NULL;
            *entry = (struct mipmap_repair_entry){
                .share_group = share_group,
                .target = target,
                .texture = texture,
                /* A validated generation of 0 never matches, so the first
                   parameter write scans. */
                .level_generation = ++mipmap_generation_counter,
                .occupied = true,
            };
            return entry;
        }
        if (entry->share_group == share_group && entry->target == target &&
            entry->texture == texture) {
            return entry;
        }
    }
    return NULL;
}

/* Linear-probing deletion: pull later entries of the same probe run back
   into the hole, so lookups can still stop at the first empty slot. */
static void remove_mipmap_repair_entry(size_t hole)
{
    mipmap_repairs[hole].occupied = false;
    for (size_t next = (hole + 1) % kMipmapRepairCapacity;
         mipmap_repairs[next].occupied;
         next = (next + 1) % kMipmapRepairCapacity) {
        const struct mipmap_repair_entry *entry = &mipmap_repairs[next];
        size_t home = mipmap_repair_home(entry->share_group, entry->target,
                                         entry->texture);
        /* The entry can move only if its home is not in (hole, next]. */
        bool home_after_hole = hole <= next ?
            (home > hole && home <= next) : (home > hole || home <= next);
        if (home_after_hole) continue;
        mipmap_repairs[hole] = *entry;
        mipmap_repairs[next].occupied = false;
        hole = next;
    }
}

/*
 * The loader thread can delete hundreds of textures at once while the render
 * thread sets texture parameters.  Each name is found by hashing, instead of
 * scanning the whole table under the mutex the render thread waits on.
 */
static void forget_mipmap_repairs(GLsizei count, const GLuint *textures)
{
    if (count <= 0 || !textures) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    static const GLenum targets[] = {GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP};
    pthread_mutex_lock(&mipmap_repair_mutex);
    for (GLsizei index = 0; index < count; ++index) {
        for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
            struct mipmap_repair_entry *entry = mipmap_repair_entry_for(
                share_group, targets[t], textures[index], false);
            if (entry) remove_mipmap_repair_entry(
                (size_t)(entry - mipmap_repairs));
        }
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);
}

/*
 * Legacy NVIDIA/ATI drivers sampled the mip levels that existed even when a
 * texture's declared GL_TEXTURE_MAX_LEVEL extended one level past its loaded
 * image chain.  Apple's current Metal-backed OpenGL correctly treats that
 * texture as incomplete and returns the fallback sample.  In this game that
 * turns alpha-cutout rain, foliage, water and sky cards into solid neon
 * geometry.  Keep the engine's requested range, but temporarily clamp it to
 * the largest contiguous, correctly-sized chain.  As streaming uploads add
 * levels, this function expands the range again and ultimately restores the
 * requested maximum.
 */
static GLenum canonical_mipmap_target(GLenum target)
{
    if (target == GL_TEXTURE_2D) return GL_TEXTURE_2D;
    if (target == GL_TEXTURE_CUBE_MAP ||
        (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
         target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) {
        return GL_TEXTURE_CUBE_MAP;
    }
    return 0;
}

static void repair_bound_texture_mipmap_range(GLenum upload_target)
{
    GLenum target = canonical_mipmap_target(upload_target);
    static int repair_disabled = -1;
    if (repair_disabled < 0) {
        repair_disabled = getenv("LP32_DISABLE_MIPMAP_REPAIR") != NULL;
    }
    if (!target || repair_disabled) return;

    GLint texture = 0;
    GLint min_filter = 0;
    GLint base_level = 0;
    GLint current_maximum = 0;
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D :
                  GL_TEXTURE_BINDING_CUBE_MAP, &texture);
    if (texture <= 0) return;
    glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &min_filter);
    if (!texture_filter_needs_mipmaps(min_filter)) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    /* Read before the scan: a level added by another context mid-scan then
       leaves the entry stale rather than wrongly current. */
    uint64_t level_generation = 0;
    pthread_mutex_lock(&mipmap_repair_mutex);
    struct mipmap_repair_entry *entry = mipmap_repair_entry_for(
        share_group, target, (GLuint)texture, true);
    if (entry) level_generation = entry->level_generation;
    pthread_mutex_unlock(&mipmap_repair_mutex);
    if (!entry) return;
    glGetTexParameteriv(target, GL_TEXTURE_BASE_LEVEL, &base_level);
    glGetTexParameteriv(target, GL_TEXTURE_MAX_LEVEL, &current_maximum);

    GLenum image_targets[6] = {GL_TEXTURE_2D};
    size_t image_target_count = 1;
    if (target == GL_TEXTURE_CUBE_MAP) {
        image_target_count = 6;
        for (size_t face = 0; face < image_target_count; ++face) {
            image_targets[face] = GL_TEXTURE_CUBE_MAP_POSITIVE_X +
                                  (GLenum)face;
        }
    }

    GLint base_width = 0;
    GLint base_height = 0;
    GLint base_format = 0;
    glGetTexLevelParameteriv(image_targets[0], base_level, GL_TEXTURE_WIDTH,
                             &base_width);
    glGetTexLevelParameteriv(image_targets[0], base_level, GL_TEXTURE_HEIGHT,
                             &base_height);
    glGetTexLevelParameteriv(image_targets[0], base_level,
                             GL_TEXTURE_INTERNAL_FORMAT, &base_format);
    if (base_width <= 0 || base_height <= 0 || !base_format) return;

    pthread_mutex_lock(&mipmap_repair_mutex);
    entry = mipmap_repair_entry_for(
        share_group, target, (GLuint)texture, true);
    if (!entry) {
        pthread_mutex_unlock(&mipmap_repair_mutex);
        return;
    }
    GLint requested_maximum = current_maximum;
    if (entry->clamped &&
        current_maximum == entry->compatibility_maximum) {
        requested_maximum = entry->requested_maximum;
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);

    GLint natural_last = base_level;
    GLint largest_dimension =
        base_width > base_height ? base_width : base_height;
    while (largest_dimension > 1 && natural_last < INT_MAX) {
        largest_dimension >>= 1;
        ++natural_last;
    }
    GLint required_last = requested_maximum < natural_last ?
        requested_maximum : natural_last;
    if (required_last < base_level) return;

    GLint expected_width = base_width;
    GLint expected_height = base_height;
    GLint last_contiguous = base_level - 1;
    for (GLint level = base_level; level <= required_last; ++level) {
        bool complete_level = true;
        for (size_t face = 0; face < image_target_count; ++face) {
            GLint width = 0;
            GLint height = 0;
            GLint format = 0;
            glGetTexLevelParameteriv(image_targets[face], level,
                                     GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(image_targets[face], level,
                                     GL_TEXTURE_HEIGHT, &height);
            glGetTexLevelParameteriv(image_targets[face], level,
                                     GL_TEXTURE_INTERNAL_FORMAT, &format);
            if (width != expected_width || height != expected_height ||
                format != base_format) {
                complete_level = false;
                break;
            }
        }
        if (!complete_level) break;
        last_contiguous = level;
        if (expected_width > 1) expected_width >>= 1;
        if (expected_height > 1) expected_height >>= 1;
    }
    if (last_contiguous < base_level) return;

    GLint compatibility_maximum = last_contiguous < required_last ?
        last_contiguous : requested_maximum;
    if (compatibility_maximum != current_maximum) {
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, compatibility_maximum);
    }

    pthread_mutex_lock(&mipmap_repair_mutex);
    entry = mipmap_repair_entry_for(share_group, target, (GLuint)texture,
                                    true);
    if (entry) {
        entry->requested_maximum = requested_maximum;
        entry->compatibility_maximum = compatibility_maximum;
        entry->clamped = compatibility_maximum != requested_maximum;
        entry->validated_generation = level_generation;
        entry->validated_min_filter = min_filter;
        entry->validated_base_level = base_level;
    }
    bool report = compatibility_maximum != requested_maximum &&
        mipmap_repair_reports < 32;
    if (report) ++mipmap_repair_reports;
    pthread_mutex_unlock(&mipmap_repair_mutex);
    if (report) {
        gl_trace_printf(
            "compat32: repaired incomplete mip chain target=%04x tex=%d base=%d "
            "requestedMax=%d availableMax=%d size=%dx%d format=%04x\n",
            target, texture, base_level, requested_maximum,
            compatibility_maximum, base_width, base_height, base_format);
    }
}

/* Called before an upload that may add or resize a level of the texture
   bound to upload_target.  Other textures keep their cached scans. */
static void note_texture_level_change(GLenum upload_target)
{
    GLenum target = canonical_mipmap_target(upload_target);
    if (!target) return;
    GLint texture = 0;
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D :
                  GL_TEXTURE_BINDING_CUBE_MAP, &texture);
    if (texture <= 0) return;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;
    pthread_mutex_lock(&mipmap_repair_mutex);
    struct mipmap_repair_entry *entry = mipmap_repair_entry_for(
        share_group, target, (GLuint)texture, false);
    if (entry) entry->level_generation = ++mipmap_generation_counter;
    pthread_mutex_unlock(&mipmap_repair_mutex);
}

enum mipmap_parameter_action {
    kMipmapParameterApply,
    kMipmapParameterApplyAndScan,
    /* The bridge's last validated value is already the GL value. */
    kMipmapParameterSkip,
    /* GL already holds the clamped maximum for this requested value. */
    kMipmapParameterKeepClamp,
};

/*
 * The game restates MIN_FILTER, BASE_LEVEL and MAX_LEVEL about 1,500 times a
 * frame, through both glTexParameteri and glTexParameterf.  When the last
 * full scan of this texture saw the same value and none of its levels has
 * changed since, the scan would reach the same answer, so skip it.  On a
 * clamped texture, the game's MAX_LEVEL write restates its requested maximum;
 * forwarding it would undo the clamp and force another scan and clamp, and
 * each change makes the driver revalidate the texture.  Keep the clamp and
 * drop the write instead.
 */
static enum mipmap_parameter_action mipmap_parameter_action(
    GLenum upload_target, GLenum parameter, GLint value)
{
    if (parameter != GL_TEXTURE_MIN_FILTER &&
        parameter != GL_TEXTURE_BASE_LEVEL &&
        parameter != GL_TEXTURE_MAX_LEVEL) {
        return kMipmapParameterApply;
    }
    GLenum target = canonical_mipmap_target(upload_target);
    if (!target) return kMipmapParameterApply;
    GLint texture = 0;
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D :
                  GL_TEXTURE_BINDING_CUBE_MAP, &texture);
    if (texture <= 0) return kMipmapParameterApply;
    CGLContextObj context = CGLGetCurrentContext();
    CGLShareGroupObj share_group = context ? CGLGetShareGroup(context) : NULL;

    enum mipmap_parameter_action action = kMipmapParameterApplyAndScan;
    pthread_mutex_lock(&mipmap_repair_mutex);
    const struct mipmap_repair_entry *entry = mipmap_repair_entry_for(
        share_group, target, (GLuint)texture, false);
    if (entry && entry->validated_generation == entry->level_generation) {
        if (parameter == GL_TEXTURE_MIN_FILTER) {
            if (value == entry->validated_min_filter) {
                action = kMipmapParameterSkip;
            }
        } else if (parameter == GL_TEXTURE_BASE_LEVEL) {
            if (value == entry->validated_base_level) {
                action = kMipmapParameterSkip;
            }
        } else if (value == entry->requested_maximum) {
            action = entry->clamped ? kMipmapParameterKeepClamp :
                                      kMipmapParameterSkip;
        }
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);
    return action;
}

static void set_texture_parameter_i(GLenum target, GLenum parameter,
                                    GLint value)
{
    enum mipmap_parameter_action action =
        mipmap_parameter_action(target, parameter, value);
    if (action == kMipmapParameterKeepClamp ||
        action == kMipmapParameterSkip) return;
    glTexParameteri(target, parameter, value);
    if (action == kMipmapParameterApplyAndScan) {
        repair_bound_texture_mipmap_range(target);
    }
}

static void set_texture_parameter_f(GLenum target, GLenum parameter,
                                    GLfloat value)
{
    enum mipmap_parameter_action action =
        mipmap_parameter_action(target, parameter, (GLint)value);
    if (action == kMipmapParameterKeepClamp ||
        action == kMipmapParameterSkip) return;
    glTexParameterf(target, parameter, value);
    if (action == kMipmapParameterApplyAndScan) {
        repair_bound_texture_mipmap_range(target);
    }
}

static void trace_texture_units(void)
{
    GLint previous_active_texture = GL_TEXTURE0;
    GLint maximum_units = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maximum_units);
    if (maximum_units < 0) maximum_units = 0;
    if (maximum_units > 16) maximum_units = 16;

    for (GLint unit = 0; unit < maximum_units; ++unit) {
        glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
        GLint texture_2d = 0;
        GLint texture_3d = 0;
        GLint texture_cube = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d);
        glGetIntegerv(GL_TEXTURE_BINDING_3D, &texture_3d);
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &texture_cube);
        if (!texture_2d && !texture_3d && !texture_cube) continue;

        GLint width_2d = 0;
        GLint height_2d = 0;
        GLint format_2d = 0;
        GLint min_filter_2d = 0;
        if (texture_2d) {
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                                     &width_2d);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                                     &height_2d);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                                     GL_TEXTURE_INTERNAL_FORMAT, &format_2d);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                &min_filter_2d);
            trace_texture_2d_content(unit, (GLuint)texture_2d,
                                     min_filter_2d);
        }
        GLint width_3d = 0;
        GLint height_3d = 0;
        GLint depth_3d = 0;
        GLint format_3d = 0;
        if (texture_3d) {
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_WIDTH,
                                     &width_3d);
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_HEIGHT,
                                     &height_3d);
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH,
                                     &depth_3d);
            glGetTexLevelParameteriv(GL_TEXTURE_3D, 0,
                                     GL_TEXTURE_INTERNAL_FORMAT, &format_3d);
        }
        GLint width_cube = 0;
        GLint height_cube = 0;
        GLint format_cube = 0;
        GLint min_filter_cube = 0;
        if (texture_cube) {
            glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0,
                                     GL_TEXTURE_WIDTH, &width_cube);
            glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0,
                                     GL_TEXTURE_HEIGHT, &height_cube);
            glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0,
                                     GL_TEXTURE_INTERNAL_FORMAT,
                                     &format_cube);
            glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                                &min_filter_cube);
            trace_texture_cube_content(unit, (GLuint)texture_cube,
                                       min_filter_cube);
        }
        gl_trace_printf(
            "compat32: texture-state swap=%llu unit=%d "
            "tex2d=%d size2d=%dx%d format2d=%04x min2d=%04x enabled2d=%d "
            "tex3d=%d size3d=%dx%dx%d format3d=%04x enabled3d=%d "
            "cube=%d sizeCube=%dx%d formatCube=%04x enabledCube=%d "
            "minCube=%04x\n",
            (unsigned long long)objc_bridge_swap_count, unit,
            texture_2d, width_2d, height_2d, format_2d, min_filter_2d,
            glIsEnabled(GL_TEXTURE_2D), texture_3d, width_3d, height_3d,
            depth_3d, format_3d, glIsEnabled(GL_TEXTURE_3D), texture_cube,
            width_cube, height_cube, format_cube,
            glIsEnabled(GL_TEXTURE_CUBE_MAP), min_filter_cube);
    }
    glActiveTexture((GLenum)previous_active_texture);
}

static void trace_draw_call(const char *kind, GLenum mode, GLint first,
                            GLuint advertised_start, GLuint advertised_end,
                            GLsizei count, GLenum index_type,
                            uintptr_t index_pointer, bool indexed)
{
    ++gl_frame_diagnostics.draw_calls;
    if (!trace_gl_render_call()) return;
    GLint framebuffer = 0;
    GLint array_buffer = 0;
    GLint element_buffer = 0;
    GLint framebuffer_status = GL_FRAMEBUFFER_COMPLETE;
    GLint blend_source = 0;
    GLint blend_destination = 0;
    GLint blend_equation = 0;
    GLint alpha_function = 0;
    GLfloat alpha_reference = 0.0f;
    GLint depth_function = 0;
    GLint cull_mode = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    glGetIntegerv(GL_BLEND_SRC, &blend_source);
    glGetIntegerv(GL_BLEND_DST, &blend_destination);
    glGetIntegerv(GL_BLEND_EQUATION, &blend_equation);
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &alpha_function);
    glGetFloatv(GL_ALPHA_TEST_REF, &alpha_reference);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_function);
    glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
    if (framebuffer) {
        framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }

    GLint maximum_draw_buffers = 0;
    GLint draw_buffers[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    GLint attachment_types[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    GLint attachment_names[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_MAX_DRAW_BUFFERS_ARB, &maximum_draw_buffers);
    if (maximum_draw_buffers < 0) maximum_draw_buffers = 0;
    if (maximum_draw_buffers > 4) maximum_draw_buffers = 4;
    for (GLint index = 0; index < maximum_draw_buffers; ++index) {
        glGetIntegerv(GL_DRAW_BUFFER0_ARB + index, &draw_buffers[index]);
        if (!framebuffer ||
            draw_buffers[index] < GL_COLOR_ATTACHMENT0 ||
            draw_buffers[index] > GL_COLOR_ATTACHMENT15) {
            continue;
        }
        glGetFramebufferAttachmentParameteriv(
            GL_FRAMEBUFFER, (GLenum)draw_buffers[index],
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attachment_types[index]);
        glGetFramebufferAttachmentParameteriv(
            GL_FRAMEBUFFER, (GLenum)draw_buffers[index],
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attachment_names[index]);
    }

    GLuint minimum = first >= 0 ? (GLuint)first : 0;
    GLuint maximum = count > 0 && first >= 0 &&
        (GLuint)first <= UINT_MAX - (GLuint)(count - 1) ?
        (GLuint)first + (GLuint)(count - 1) : minimum;
    gl_trace_printf(
        "compat32: %s swap=%llu mode=%04x first=%d count=%d fbo=%d "
        "fboStatus=%04x vbo=%d ibo=%d vp=%u fp=%u vpEnabled=%d "
        "fpEnabled=%d blend=%d blendFunc={%04x,%04x} blendEq=%04x "
        "alphaTest=%d alphaFunc=%04x alphaRef=%.7g depthTest=%d "
        "depthFunc=%04x cull=%d cullMode=%04x\n",
        kind, (unsigned long long)objc_bridge_swap_count, mode, first, count,
        framebuffer, framebuffer_status, array_buffer, element_buffer,
        trace_vertex_program, trace_fragment_program,
        glIsEnabled(GL_VERTEX_PROGRAM_ARB),
        glIsEnabled(GL_FRAGMENT_PROGRAM_ARB), glIsEnabled(GL_BLEND),
        blend_source, blend_destination, blend_equation,
        glIsEnabled(GL_ALPHA_TEST), alpha_function, alpha_reference,
        glIsEnabled(GL_DEPTH_TEST), depth_function,
        glIsEnabled(GL_CULL_FACE), cull_mode);
    gl_trace_printf(
        "compat32: framebuffer-state swap=%llu fbo=%d "
        "drawBuffers={%04x,%04x,%04x,%04x} "
        "attachmentTypes={%04x,%04x,%04x,%04x} "
        "attachmentNames={%d,%d,%d,%d}\n",
        (unsigned long long)objc_bridge_swap_count, framebuffer,
        draw_buffers[0], draw_buffers[1], draw_buffers[2], draw_buffers[3],
        attachment_types[0], attachment_types[1], attachment_types[2],
        attachment_types[3], attachment_names[0], attachment_names[1],
        attachment_names[2], attachment_names[3]);

    if (indexed) {
        struct gl_index_diagnostic diagnostic = trace_index_buffer(
            count, index_type, index_pointer, advertised_start,
            advertised_end, element_buffer);
        minimum = diagnostic.minimum;
        maximum = diagnostic.maximum;
    }
    trace_vertex_attributes(minimum, maximum);
    trace_texture_units();
}

/*
 * Diagnostic pixel probe: LP32_TRACE_PIXEL=fx,fy (fractions of the viewport,
 * origin bottom-left) reads that pixel back after every draw of an armed
 * render trace and logs each change together with the programs that made it,
 * so the draw that paints a given screen region can be identified.
 */
static void probe_trace_pixel(const char *kind)
{
    static int state = -1;
    static float fraction_x, fraction_y;
    static GLubyte last[4];
    if (state < 0) {
        const char *value = getenv("LP32_TRACE_PIXEL");
        state = 0;
        if (value && sscanf(value, "%f,%f", &fraction_x, &fraction_y) == 2) {
            state = 1;
        }
    }
    if (state != 1 || !gl_render_trace_remaining) return;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint framebuffer = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (viewport[2] < 256 || viewport[3] < 256) return;
    /* Negative fractions select a grid over the whole viewport instead. */
    if (fraction_x < 0.0f) {
        enum { GRID = 12 };
        static GLubyte grid_last[GRID * GRID][4];
        for (int index = 0; index < GRID * GRID; ++index) {
            GLint x = viewport[0] + (GLint)((index % GRID + 0.5f) *
                                            (float)viewport[2] / GRID);
            GLint y = viewport[1] + (GLint)((index / GRID + 0.5f) *
                                            (float)viewport[3] / GRID);
            GLubyte pixel[4] = {0, 0, 0, 0};
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            if (memcmp(pixel, grid_last[index], sizeof(pixel)) == 0) continue;
            memcpy(grid_last[index], pixel, sizeof(pixel));
            gl_trace_printf(
                "compat32: pixel-probe swap=%llu after=%s fbo=%d point=%d "
                "at=(%d,%d) rgba={%u,%u,%u,%u} vp=%u fp=%u\n",
                (unsigned long long)objc_bridge_swap_count, kind, framebuffer,
                index, x, y, pixel[0], pixel[1], pixel[2], pixel[3],
                trace_vertex_program, trace_fragment_program);
        }
        return;
    }
    GLint x = viewport[0] + (GLint)(fraction_x * (float)viewport[2]);
    GLint y = viewport[1] + (GLint)(fraction_y * (float)viewport[3]);
    GLubyte pixel[4] = {0, 0, 0, 0};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (memcmp(pixel, last, sizeof(pixel)) == 0) return;
    memcpy(last, pixel, sizeof(pixel));
    gl_trace_printf(
        "compat32: pixel-probe swap=%llu after=%s fbo=%d viewport={%d,%d,%d,%d} "
        "at=(%d,%d) rgba={%u,%u,%u,%u} vp=%u fp=%u\n",
        (unsigned long long)objc_bridge_swap_count, kind, framebuffer,
        viewport[0], viewport[1], viewport[2], viewport[3], x, y,
        pixel[0], pixel[1], pixel[2], pixel[3], trace_vertex_program,
        trace_fragment_program);
}

static uint32_t guest_gl_string(GLenum name)
{
    const GLubyte *raw_value = glGetString(name);
    if (!raw_value) return 0;
    static bool logged_host_graphics;
    if (!__atomic_exchange_n(&logged_host_graphics, true, __ATOMIC_RELAXED)) {
        const GLubyte *vendor = glGetString(GL_VENDOR);
        const GLubyte *renderer = glGetString(GL_RENDERER);
        const GLubyte *version = glGetString(GL_VERSION);
        NSProcessInfo *process = [NSProcessInfo processInfo];
        fprintf(stderr, "compat32: host GL vendor=%s renderer=%s version=%s thermal_state=%ld",
                vendor ? (const char *)vendor : "unknown",
                renderer ? (const char *)renderer : "unknown",
                version ? (const char *)version : "unknown", (long)[process thermalState]);
        if (@available(macOS 12.0, *))
            fprintf(stderr, " low_power=%d", [process isLowPowerModeEnabled]);
        fputc('\n', stderr);
    }
    const char *value = (const char *)raw_value;
    const char *buffer_mode = getenv("LP32_GL_BUFFER_MODE");
    bool prefer_subdata = getenv("LP32_PREFER_BUFFER_SUBDATA") != NULL ||
        (buffer_mode && strcmp(buffer_mode, "subdata") == 0);
    bool prefer_map_range = buffer_mode && strcmp(buffer_mode, "maprange") == 0;
    if (name != GL_EXTENSIONS || (!prefer_subdata && !prefer_map_range)) {
        return intern_guest_cstring(value);
    }

    static const char apple_extension[] = "GL_APPLE_flush_buffer_range";
    static const char range_extension[] = "GL_ARB_map_buffer_range";
    size_t length = strlen(value);
    char *filtered = malloc(length + 1);
    if (!filtered) return 0;
    size_t output = 0;
    const char *cursor = value;
    while (*cursor) {
        while (*cursor == ' ') ++cursor;
        const char *end = cursor;
        while (*end && *end != ' ') ++end;
        size_t token_length = (size_t)(end - cursor);
        bool omit_apple = token_length == sizeof(apple_extension) - 1 &&
            memcmp(cursor, apple_extension, token_length) == 0;
        bool omit_range = prefer_subdata &&
            token_length == sizeof(range_extension) - 1 &&
            memcmp(cursor, range_extension, token_length) == 0;
        if (token_length && !omit_apple && !omit_range) {
            if (output) filtered[output++] = ' ';
            memcpy(filtered + output, cursor, token_length);
            output += token_length;
        }
        cursor = end;
    }
    filtered[output] = '\0';
    uint32_t guest_value = intern_guest_cstring(filtered);
    free(filtered);
    static bool logged;
    if (!logged) {
        fprintf(stderr,
                "compat32: GL buffer backend override=%s; hiding %s%s\n",
                prefer_subdata ? "subdata" : "maprange",
                apple_extension,
                prefer_subdata ? " and GL_ARB_map_buffer_range" : "");
        logged = true;
    }
    return guest_value;
}

static int objc_bridge32_dispatch_body(const char *import_name,
                                       size_t import_length,
                                       const uint32_t *arguments,
                                       uint64_t *result);

/*
 * Feral's MacDoze SendMessage wakes the main thread with an application-
 * defined NSEvent posted from the game thread, then spins until the main
 * thread's -sendEvent: override has handled the message.  Current AppKit
 * returns such queued events from -nextEventMatchingMask: only on its next
 * display-cycle pass, about 16 ms later, whether they were posted from the
 * game thread or re-posted on the main thread.  Several round trips per
 * frame capped Complete Saga's title and menus near 30 FPS, and much lower
 * while loading.  Deliver these wake-up events from a main-thread run-loop
 * source instead.  -[NSApplication run] would pass them to the same
 * -sendEvent:.  Other run-loop modes (modal panels, tracking) and guests
 * pumping -nextEventMatchingMask: themselves keep AppKit's queue, so their
 * event filtering is unchanged.
 */
static os_unfair_lock cross_thread_event_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableArray *cross_thread_events;
/* Main-thread depth of guest-issued -nextEventMatchingMask: calls.  A guest
   pumping its own events must dequeue (or skip) these events itself. */
static unsigned guest_event_wait_depth;

static void cross_thread_event_perform(void *info)
{
    (void)info;
    os_unfair_lock_lock(&cross_thread_event_lock);
    NSArray *events = cross_thread_events;
    cross_thread_events = nil;
    os_unfair_lock_unlock(&cross_thread_event_lock);
    if (!events) return;
    CFStringRef mode = CFRunLoopCopyCurrentMode(CFRunLoopGetMain());
    bool direct = mode && CFEqual(mode, kCFRunLoopDefaultMode) &&
                  !guest_event_wait_depth;
    if (mode) CFRelease(mode);
    for (NSArray *entry in events) {
        NSEvent *event = entry[0];
        if (direct) {
            @autoreleasepool { [NSApp sendEvent:event]; }
        } else {
            [NSApp postEvent:event atStart:[entry[1] boolValue]];
        }
    }
    [events release];
}

static void deliver_cross_thread_event(NSEvent *event, BOOL at_start)
{
    static CFRunLoopSourceRef source;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        CFRunLoopSourceContext context = {0};
        context.perform = cross_thread_event_perform;
        source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
        CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    });
    NSArray *entry = [[NSArray alloc] initWithObjects:event, @(at_start), nil];
    os_unfair_lock_lock(&cross_thread_event_lock);
    if (!cross_thread_events) cross_thread_events = [[NSMutableArray alloc] init];
    [cross_thread_events addObject:entry];
    os_unfair_lock_unlock(&cross_thread_event_lock);
    [entry release];
    CFRunLoopSourceSignal(source);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

/*
 * Direct handlers for the GL entry points that make up most of the 10-30
 * thousand imports per frame.  The runtime memoizes them per import id after
 * the first chained dispatch, so later calls skip the name chains in
 * dispatch_named_import and objc_bridge32_dispatch_body entirely.  The chain
 * cases below call these same functions; keep them the only implementation.
 */
#define FAST_GL(name) \
    static uint64_t fast_##name(const uint32_t *arguments, \
                                uint32_t return_address)

/* Feral's command queues poll these at very high frequency. Keep exactly
 * the same i386 argument widening/native operations as the chained handlers. */
FAST_GL(dispatch_time)
{
    (void)return_address;
    uint64_t when; int64_t delta;
    memcpy(&when, arguments, 8); memcpy(&delta, arguments + 2, 8);
    return dispatch_time(when, delta);
}
FAST_GL(dispatch_semaphore_wait)
{
    (void)return_address;
    uint64_t timeout; memcpy(&timeout, arguments + 1, 8);
    /* A past monotonic deadline means a nonblocking attempt. libdispatch's
       generic timed path still enters the kernel for it. Feral constructs
       dispatch_time(NOW,0) millions of times; use the explicit poll path.
       Leave encoded wall/continuous clocks and FOREVER untouched. */
    if(timeout && timeout<(UINT64_C(1)<<62) && timeout<=dispatch_time(DISPATCH_TIME_NOW,0))
        timeout=DISPATCH_TIME_NOW;
    return (uint32_t)dispatch_semaphore_wait(
        (dispatch_semaphore_t)object_for_argument(arguments[0]), timeout);
}
FAST_GL(dispatch_semaphore_signal)
{
    (void)return_address;
    return (uint32_t)dispatch_semaphore_signal(
        (dispatch_semaphore_t)object_for_argument(arguments[0]));
}

FAST_GL(CGLGetCurrentContext)
{
    (void)arguments; (void)return_address;
    return objc_bridge32_guest_pointer(CGLGetCurrentContext());
}

/* Resource unloads switch contexts about 14,000 times a frame, almost all to
   the context that is already current. */
FAST_GL(CGLSetCurrentContext)
{
    (void)return_address;
    NSValue *wrapper = object_for_argument(arguments[0]);
    CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
    if (context == CGLGetCurrentContext()) return kCGLNoError;
    return (uint32_t)CGLSetCurrentContext(context);
}

FAST_GL(glVertexAttribPointer)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        GLint array_buffer = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        gl_trace_printf(
            "compat32: glVertexAttribPointer swap=%llu index=%u "
            "size=%d type=%04x normalized=%u stride=%d pointer=%08x "
            "buffer=%d\n",
            (unsigned long long)objc_bridge_swap_count, arguments[0],
            (GLint)arguments[1], arguments[2], arguments[3],
            (GLsizei)arguments[4], arguments[5], array_buffer);
    }
    glVertexAttribPointer(arguments[0], (GLint)arguments[1], arguments[2],
                          (GLboolean)arguments[3], (GLsizei)arguments[4],
                          (const void *)(uintptr_t)arguments[5]);
    return 0;
}

FAST_GL(glEnableVertexAttribArray)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glEnableVertexAttribArray swap=%llu index=%u\n",
            (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glEnableVertexAttribArray(arguments[0]);
    return 0;
}

FAST_GL(glDisableVertexAttribArray)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glDisableVertexAttribArray swap=%llu index=%u\n",
            (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glDisableVertexAttribArray(arguments[0]);
    return 0;
}

FAST_GL(glDrawRangeElements)
{
    (void)return_address;
    struct sampler_fallback_restore sampler_restore;
    repair_unbound_fragment_samplers(&sampler_restore);
    trace_draw_call("glDrawRangeElements", arguments[0], 0,
                    arguments[1], arguments[2], (GLsizei)arguments[3],
                    arguments[4], (uintptr_t)arguments[5], true);
    glDrawRangeElements(arguments[0], arguments[1], arguments[2],
                        (GLsizei)arguments[3], arguments[4],
                        (const void *)(uintptr_t)arguments[5]);
    probe_trace_pixel("glDrawRangeElements");
    restore_unbound_fragment_samplers(&sampler_restore);
    return 0;
}

FAST_GL(glDrawArrays)
{
    (void)return_address;
    struct sampler_fallback_restore sampler_restore;
    repair_unbound_fragment_samplers(&sampler_restore);
    trace_draw_call("glDrawArrays", arguments[0], (GLint)arguments[1],
                    0, 0, (GLsizei)arguments[2], 0, 0, false);
    glDrawArrays(arguments[0], arguments[1], arguments[2]);
    probe_trace_pixel("glDrawArrays");
    restore_unbound_fragment_samplers(&sampler_restore);
    return 0;
}

FAST_GL(glProgramEnvParameters4fvEXT)
{
    (void)return_address;
    const GLfloat *values = (const void *)(uintptr_t)arguments[3];
    GLsizei count = (GLsizei)arguments[2];
    trace_program_parameter_upload(
        "glProgramEnvParameters4fvEXT", arguments[0], arguments[1],
        count, values);
    size_t replaced = 0;
    const GLfloat *upload =
        safe_program_parameter_values(values, count, &replaced);
    report_sanitized_program_parameters(
        "environment-batch", arguments[0], arguments[1], count, replaced);
    if (split_program_parameter_batches()) {
        for (GLsizei index = 0; index < count; ++index) {
            glProgramEnvParameter4fvARB(
                arguments[0], arguments[1] + index, upload + index * 4);
        }
    } else {
        glProgramEnvParameters4fvEXT(arguments[0], arguments[1], count,
                                     upload);
    }
    return 0;
}

FAST_GL(glProgramLocalParameters4fvEXT)
{
    (void)return_address;
    const GLfloat *values = (const void *)(uintptr_t)arguments[3];
    GLsizei count = (GLsizei)arguments[2];
    trace_program_parameter_upload(
        "glProgramLocalParameters4fvEXT", arguments[0], arguments[1],
        count, values);
    size_t replaced = 0;
    const GLfloat *upload =
        safe_program_parameter_values(values, count, &replaced);
    report_sanitized_program_parameters(
        "local-batch", arguments[0], arguments[1], count, replaced);
    if (split_program_parameter_batches()) {
        for (GLsizei index = 0; index < count; ++index) {
            glProgramLocalParameter4fvARB(
                arguments[0], arguments[1] + index, upload + index * 4);
        }
    } else {
        glProgramLocalParameters4fvEXT(arguments[0], arguments[1], count,
                                       upload);
    }
    return 0;
}

FAST_GL(glProgramEnvParameter4fvARB)
{
    (void)return_address;
    const GLfloat *values = (const void *)(uintptr_t)arguments[2];
    trace_program_parameter_upload(
        "glProgramEnvParameter4fvARB", arguments[0], arguments[1], 1, values);
    size_t replaced = 0;
    const GLfloat *upload = safe_program_parameter_values(values, 1, &replaced);
    report_sanitized_program_parameters(
        "environment-parameter", arguments[0], arguments[1], 1, replaced);
    glProgramEnvParameter4fvARB(arguments[0], arguments[1], upload);
    return 0;
}

FAST_GL(glProgramLocalParameter4fvARB)
{
    (void)return_address;
    const GLfloat *values = (const void *)(uintptr_t)arguments[2];
    trace_program_parameter_upload(
        "glProgramLocalParameter4fvARB", arguments[0], arguments[1], 1,
        values);
    size_t replaced = 0;
    const GLfloat *upload = safe_program_parameter_values(values, 1, &replaced);
    report_sanitized_program_parameters(
        "local-parameter", arguments[0], arguments[1], 1, replaced);
    glProgramLocalParameter4fvARB(arguments[0], arguments[1], upload);
    return 0;
}

FAST_GL(glBindProgramARB)
{
    if (hitch_recorder_enabled) hitch_program(arguments[0], arguments[1]);
    (void)return_address;
    ++gl_frame_diagnostics.program_binds;
    if (trace_gl_render_call()) {
        fprintf(stderr,
                "compat32: glBindProgramARB swap=%llu target=%04x "
                "program=%u\n",
                (unsigned long long)objc_bridge_swap_count,
                arguments[0], arguments[1]);
    }
    glBindProgramARB(arguments[0], arguments[1]);
    if (arguments[0] == GL_VERTEX_PROGRAM_ARB) {
        trace_vertex_program = arguments[1];
    } else if (arguments[0] == GL_FRAGMENT_PROGRAM_ARB) {
        trace_fragment_program = arguments[1];
    }
    return 0;
}

FAST_GL(glBindBuffer)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glBindBuffer swap=%llu target=%04x buffer=%u\n",
            (unsigned long long)objc_bridge_swap_count,
            arguments[0], arguments[1]);
    }
    glBindBuffer(arguments[0], arguments[1]);
    return 0;
}

FAST_GL(glActiveTexture)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glActiveTexture swap=%llu unit=%04x\n",
            (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glActiveTexture(arguments[0]);
    return 0;
}

FAST_GL(glClientActiveTexture)
{
    (void)return_address;
    glClientActiveTexture(arguments[0]);
    return 0;
}

FAST_GL(glBlendEquation)
{
    (void)return_address;
    glBlendEquation(arguments[0]);
    return 0;
}

FAST_GL(glBlendFuncSeparate)
{
    (void)return_address;
    glBlendFuncSeparate(arguments[0], arguments[1], arguments[2],
                        arguments[3]);
    return 0;
}

FAST_GL(glEnable)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        fprintf(stderr, "compat32: glEnable swap=%llu cap=%04x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glEnable(arguments[0]);
    return 0;
}

FAST_GL(glDisable)
{
    (void)return_address;
    if (trace_gl_render_call()) {
        fprintf(stderr, "compat32: glDisable swap=%llu cap=%04x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0]);
    }
    glDisable(arguments[0]);
    return 0;
}

FAST_GL(glBindTexture)
{
    (void)return_address;
    ++gl_frame_diagnostics.texture_binds;
    if (trace_gl_render_call()) {
        gl_trace_printf(
            "compat32: glBindTexture swap=%llu target=%04x texture=%u\n",
            (unsigned long long)objc_bridge_swap_count,
            arguments[0], arguments[1]);
    }
    glBindTexture(arguments[0], arguments[1]);
    return 0;
}

FAST_GL(glTexParameteri)
{
    (void)return_address;
    set_texture_parameter_i(arguments[0], arguments[1], (GLint)arguments[2]);
    return 0;
}

FAST_GL(glTexEnvf)
{
    (void)return_address;
    float value;
    memcpy(&value, arguments + 2, sizeof(value));
    glTexEnvf(arguments[0], arguments[1], value);
    return 0;
}

FAST_GL(glAlphaFunc)
{
    (void)return_address;
    float value;
    memcpy(&value, arguments + 1, sizeof(value));
    glAlphaFunc(arguments[0], value);
    return 0;
}

FAST_GL(glPolygonOffset)
{
    (void)return_address;
    const float *v = (const void *)arguments;
    glPolygonOffset(v[0], v[1]);
    return 0;
}

FAST_GL(glBlendFunc)  { (void)return_address; glBlendFunc(arguments[0], arguments[1]); return 0; }
FAST_GL(glColorMask)  { (void)return_address; glColorMask(arguments[0], arguments[1], arguments[2], arguments[3]); return 0; }
FAST_GL(glCullFace)   { (void)return_address; glCullFace(arguments[0]); return 0; }
FAST_GL(glDepthFunc)  { (void)return_address; glDepthFunc(arguments[0]); return 0; }
FAST_GL(glDepthMask)  { (void)return_address; glDepthMask(arguments[0]); return 0; }
FAST_GL(glFrontFace)  { (void)return_address; glFrontFace(arguments[0]); return 0; }
FAST_GL(glScissor)    { (void)return_address; glScissor(arguments[0], arguments[1], arguments[2], arguments[3]); return 0; }
FAST_GL(glStencilFunc){ (void)return_address; glStencilFunc(arguments[0], arguments[1], arguments[2]); return 0; }
FAST_GL(glStencilMask){ (void)return_address; glStencilMask(arguments[0]); return 0; }
FAST_GL(glStencilOp)  { (void)return_address; glStencilOp(arguments[0], arguments[1], arguments[2]); return 0; }
FAST_GL(glGetIntegerv){ (void)return_address; glGetIntegerv(arguments[0], (GLint *)(uintptr_t)arguments[1]); return 0; }
FAST_GL(glGetFloatv)  { (void)return_address; glGetFloatv(arguments[0], (GLfloat *)(uintptr_t)arguments[1]); return 0; }

#undef FAST_GL

lp32_fast_import_fn objc_bridge32_fast_import(const char *import_name)
{
    static const struct {
        const char *name;
        lp32_fast_import_fn handler;
    } table[] = {
        {"dispatch_time", fast_dispatch_time},
        {"dispatch_semaphore_wait", fast_dispatch_semaphore_wait},
        {"dispatch_semaphore_signal", fast_dispatch_semaphore_signal},
        {"CGLGetCurrentContext", fast_CGLGetCurrentContext},
        {"CGLSetCurrentContext", fast_CGLSetCurrentContext},
        {"glVertexAttribPointer", fast_glVertexAttribPointer},
        {"glVertexAttribPointerARB", fast_glVertexAttribPointer},
        {"glEnableVertexAttribArray", fast_glEnableVertexAttribArray},
        {"glEnableVertexAttribArrayARB", fast_glEnableVertexAttribArray},
        {"glDisableVertexAttribArray", fast_glDisableVertexAttribArray},
        {"glDisableVertexAttribArrayARB", fast_glDisableVertexAttribArray},
        {"glDrawRangeElements", fast_glDrawRangeElements},
        {"glDrawRangeElementsEXT", fast_glDrawRangeElements},
        {"_glDrawArrays", fast_glDrawArrays},
        {"glProgramEnvParameters4fvEXT", fast_glProgramEnvParameters4fvEXT},
        {"glProgramLocalParameters4fvEXT", fast_glProgramLocalParameters4fvEXT},
        {"glProgramEnvParameter4fvARB", fast_glProgramEnvParameter4fvARB},
        {"glProgramLocalParameter4fvARB", fast_glProgramLocalParameter4fvARB},
        {"glBindProgramARB", fast_glBindProgramARB},
        {"glBindBuffer", fast_glBindBuffer},
        {"glBindBufferARB", fast_glBindBuffer},
        {"glActiveTexture", fast_glActiveTexture},
        {"glActiveTextureARB", fast_glActiveTexture},
        {"glClientActiveTexture", fast_glClientActiveTexture},
        {"glClientActiveTextureARB", fast_glClientActiveTexture},
        {"glBlendEquation", fast_glBlendEquation},
        {"glBlendEquationEXT", fast_glBlendEquation},
        {"glBlendFuncSeparate", fast_glBlendFuncSeparate},
        {"glBlendFuncSeparateEXT", fast_glBlendFuncSeparate},
        {"_glEnable", fast_glEnable},
        {"_glDisable", fast_glDisable},
        {"_glBindTexture", fast_glBindTexture},
        {"_glTexParameteri", fast_glTexParameteri},
        {"_glTexEnvf", fast_glTexEnvf},
        {"_glAlphaFunc", fast_glAlphaFunc},
        {"_glPolygonOffset", fast_glPolygonOffset},
        {"_glBlendFunc", fast_glBlendFunc},
        {"_glColorMask", fast_glColorMask},
        {"_glCullFace", fast_glCullFace},
        {"_glDepthFunc", fast_glDepthFunc},
        {"_glDepthMask", fast_glDepthMask},
        {"_glFrontFace", fast_glFrontFace},
        {"_glScissor", fast_glScissor},
        {"_glStencilFunc", fast_glStencilFunc},
        {"_glStencilMask", fast_glStencilMask},
        {"_glStencilOp", fast_glStencilOp},
        {"_glGetIntegerv", fast_glGetIntegerv},
        {"_glGetFloatv", fast_glGetFloatv},
    };
    /* Mach-O imports and dlsym expose the same functions with different
       leading-underscore conventions. Both must populate the fast cache. */
    if (import_name[0] == '_') ++import_name;
    for (size_t index = 0; index < sizeof(table) / sizeof(table[0]); ++index) {
        const char *name = table[index].name;
        if (name[0] == '_') ++name;
        if (strcmp(import_name, name) == 0) {
            return table[index].handler;
        }
    }
    return NULL;
}

/* Earlier Marvel caches omitted all shader constants because the guest's
 * inline ctype table was missing. Preserve those files, then let the game
 * compile valid metadata once. Other titles keep their existing caches. */
int objc_bridge32_prepare_shader_cache(void)
{
    if (lp32_profile()->title != LP32_TITLE_MARVEL) return 0;
    @autoreleasepool {
        const char *override = getenv("LP32_APPLICATION_SUPPORT_DIR");
        NSString *support = override && override[0] ?
            [NSString stringWithUTF8String:override] :
            [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                NSUserDomainMask, YES) firstObject];
        if (!support) return -1;
        NSString *root = [support stringByAppendingPathComponent:
            @"Feral Interactive/LEGO Marvel Super Heroes"];
        NSString *marker = [root stringByAppendingPathComponent:
            @".lp32-shader-cache-ctype-v1"];
        NSFileManager *files = [NSFileManager defaultManager];
        if ([files fileExistsAtPath:marker]) return 0;
        NSError *error = nil;
        if (![files createDirectoryAtPath:root withIntermediateDirectories:YES
                               attributes:nil error:&error]) {
            fprintf(stderr, "compat32: cannot prepare Marvel shader cache: %s\n",
                    [[error description] UTF8String]);
            return -1;
        }
        NSString *cache = [root stringByAppendingPathComponent:@"CachedShadersGL"];
        if ([files fileExistsAtPath:cache]) {
            NSString *backup = [cache stringByAppendingFormat:
                @".before-ctype-%@", [[NSUUID UUID] UUIDString]];
            if (![files moveItemAtPath:cache toPath:backup error:&error]) {
                fprintf(stderr, "compat32: cannot back up Marvel shader cache: %s\n",
                        [[error description] UTF8String]);
                return -1;
            }
            fprintf(stderr, "compat32: rebuilding Marvel shader cache; backup: %s\n",
                    [backup fileSystemRepresentation]);
        }
        if (![@"1\n" writeToFile:marker atomically:YES
                       encoding:NSUTF8StringEncoding error:&error]) {
            fprintf(stderr, "compat32: cannot mark Marvel shader cache migration: %s\n",
                    [[error description] UTF8String]);
            return -1;
        }
    }
    return 0;
}

struct display_callback32 {uint32_t function,context;bool active;};
static struct display_callback32 display_callbacks[64];
static pthread_mutex_t display_callback_lock=PTHREAD_MUTEX_INITIALIZER;
static void display_reconfigured(CGDirectDisplayID display,CGDisplayChangeSummaryFlags flags,void *raw){
    struct display_callback32 *entry=raw;pthread_mutex_lock(&display_callback_lock);
    uint32_t function=entry->active?entry->function:0,context=entry->context;pthread_mutex_unlock(&display_callback_lock);
    if(function){uint32_t args[]={display,flags,context};compat_runtime32_call(function,args,3);}
}
static int display_callback_dispatch(const char *name,const uint32_t *args,uint64_t *result){
    bool add=!strcmp(name,"_CGDisplayRegisterReconfigurationCallback");
    if(!add&&strcmp(name,"_CGDisplayRemoveReconfigurationCallback"))return 0;
    pthread_mutex_lock(&display_callback_lock);struct display_callback32 *entry=NULL;
    for(unsigned i=0;i<64;++i)if(display_callbacks[i].function==args[0]&&display_callbacks[i].context==args[1]){entry=&display_callbacks[i];break;}
    if(!entry&&add)for(unsigned i=0;i<64;++i)if(!display_callbacks[i].function){entry=&display_callbacks[i];entry->function=args[0];entry->context=args[1];break;}
    pthread_mutex_unlock(&display_callback_lock);
    if(!entry){*result=kCGErrorIllegalArgument;return 1;}
    CGError error=add?CGDisplayRegisterReconfigurationCallback(display_reconfigured,entry):CGDisplayRemoveReconfigurationCallback(display_reconfigured,entry);
    if(error==kCGErrorSuccess){pthread_mutex_lock(&display_callback_lock);entry->active=add;pthread_mutex_unlock(&display_callback_lock);}
    *result=error;return 1;
}

struct lp32_provider_data {uint32_t info, data, size, release;};
static void release_provider_data(void *raw,const void *data,size_t size) {
    (void)data;(void)size;struct lp32_provider_data *p=raw;
    if(p->release){uint32_t args[]={p->info,p->data,p->size};compat_runtime32_call(p->release,args,3);}free(p);
}
int objc_bridge32_dispatch(const char *import_name, const uint32_t *arguments,
                           uint64_t *result)
{
    if (strncmp(import_name,"CG",2)==0) {
        char normalized[128]; if(strlen(import_name)+2>sizeof(normalized))return 0;
        normalized[0]='_';strcpy(normalized+1,import_name);
        return objc_bridge32_dispatch(normalized,arguments,result);
    }
    if (!strcmp(import_name,"_CGDataProviderRelease") || !strcmp(import_name,"_CGImageRelease") ||
        !strcmp(import_name,"_CGColorSpaceRelease") || !strcmp(import_name,"_CGContextRelease"))
        return objc_bridge32_dispatch("_CFRelease",arguments,result);
    if (!strcmp(import_name,"_CGDataProviderRetain") || !strcmp(import_name,"_CGImageRetain") ||
        !strcmp(import_name,"_CGColorSpaceRetain") || !strcmp(import_name,"_CGContextRetain"))
        return objc_bridge32_dispatch("_CFRetain",arguments,result);
    /*
     * The guest creates and drains its own NSAutoreleasePool objects, but
     * those pools cannot reliably describe the lifetime of temporary host
     * objects created inside a single ABI bridge call.  In particular,
     * invoke_simple_message creates an autoreleased NSInvocation (and a
     * return-data allocation) for every objc_msgSend.  During rendering that
     * retained hundreds of thousands of invocations in the outer AppKit pool
     * and grew the native heap by roughly 100 MiB in a few minutes.
     *
     * Returned Objective-C objects are registered in proxy_for_object before
     * leaving this scope, which retains them.  It is therefore safe, and much
     * closer to an FFI call boundary, to drain all other host temporaries at
     * the end of each dispatch.  @autoreleasepool cleanup also runs on every
     * early return from the body.
     *
     * OpenGL entry points are the bulk of the per-frame traffic (10-30
     * thousand calls) and never create Objective-C temporaries, so they skip
     * the pool push/pop entirely.
     */
    const size_t import_length = strlen(import_name);
    bool is_gl = (import_name[0] == 'g' && import_name[1] == 'l') ||
                 (import_name[0] == '_' && import_name[1] == 'g' &&
                  import_name[2] == 'l');
    if (is_gl) {
        if (objc_bridge32_dispatch_body(import_name, import_length, arguments, result)) return 1;
        /* Mach-O imports have a leading underscore; dlsym names do not. */
        if (import_name[0] == '_' && objc_bridge32_dispatch_body(import_name+1, import_length-1, arguments, result)) return 1;
        if (import_name[0] != '_' && import_length < 126) {
            char prefixed[128];prefixed[0]='_';memcpy(prefixed+1,import_name,import_length+1);
            if(objc_bridge32_dispatch_body(prefixed,import_length+1,arguments,result))return 1;
        }
        return gl_core_bridge32_dispatch(import_name, arguments, result);
    }
    @autoreleasepool {
        /* CF Create/Copy results carry a guest ownership count. Borrowed
         * Cocoa/CF results keep their existing persistent lifetime. */
        bool previous = creating_cf_object;
        const char *previous_origin=proxy_origin;
        proxy_origin=(!strcmp(import_name,"_objc_msgSend") || !strcmp(import_name,"_objc_msgSend_fpret")) ?
            (const char *)(uintptr_t)arguments[1] : import_name;
        creating_cf_object = (strncmp(import_name, "_CT", 3) == 0 || strncmp(import_name, "_CF", 3) == 0 || strncmp(import_name, "_CG", 3) == 0 || strncmp(import_name, "_IOHID", 6) == 0 || strncmp(import_name, "_TIS", 4) == 0) &&
            (strstr(import_name, "Create") || strstr(import_name, "Copy"));
        if (strncmp(import_name, "_dispatch_", 10) == 0 && strstr(import_name, "_create"))
            creating_cf_object = true;
        int handled = objc_bridge32_dispatch_body(import_name, import_length,
                                                   arguments, result);
        creating_cf_object = previous;
        proxy_origin=previous_origin;
        refresh_enumeration_mutations();
        return handled;
    }
}

static int objc_bridge32_dispatch_body(const char *import_name,
                                       size_t import_length,
                                       const uint32_t *arguments,
                                       uint64_t *result)
{
    if (LP32_NAME_IS(import_name, import_length, "_objc_msgSendSuper")) {
        const uint32_t *super = (const void *)(uintptr_t)arguments[0];
        id receiver = object_for_argument(super[0]);
        Class parent = (Class)object_for_argument(super[1]);
        const char *selector = (const void *)(uintptr_t)arguments[1];
        Method method = parent ? class_getInstanceMethod(parent, sel_registerName(selector)) : NULL;
        if (!receiver || !method) return 0;
        NSMethodSignature *signature = [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(method)];
        unsigned count = 2;
        for (NSUInteger i=2;i<signature.numberOfArguments;++i) {
            const char *type = [signature getArgumentTypeAtIndex:i];
            count += strstr(type,"Rect=") ? 4 :
                (strstr(type,"Point=") || strstr(type,"Size=")) ? 2 : guest_words_for_type(type);
        }
        if (count > 128) return 0;
        uint32_t args[128]; memcpy(args, arguments, count * 4); args[0] = super[0];
        if(objc_legacy32_super_message(receiver,parent,args,result))return 1;
        if (handle_test_activation(receiver, selector, result)) return 1;
        if (getenv("LP32_TRACE_EVENTS") && !strcmp(selector,"sendEvent:")) {
            NSEvent *event=object_for_argument(args[2]);
            fprintf(stderr,"compat32: super sendEvent token=%08x native=%p type=%lu caller=%08x\n",
                args[2],event,(unsigned long)event.type,arguments[-1]);
        }
        return invoke_simple_message(receiver, selector, args, result, method, NULL);
    }
    if (LP32_NAME_IS(import_name, import_length, "_objc_sync_enter") ||
        LP32_NAME_IS(import_name, import_length, "_objc_sync_exit")) {
        id object = object_for_argument(arguments[0]);
        *result = (uint32_t)(LP32_NAME_IS(import_name, import_length, "_objc_sync_enter") ?
                            objc_sync_enter(object) : objc_sync_exit(object));
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryApplyFunction")) {
        CFDictionaryRef dictionary=(CFDictionaryRef)object_for_argument(arguments[0]);
        CFIndex count=CFDictionaryGetCount(dictionary);
        const void **entries=calloc((size_t)(count?count:1)*2,sizeof(void *));
        if(!entries)return 0;
        CFDictionaryGetKeysAndValues(dictionary,entries,entries+count);
        for(CFIndex i=0;i<count;++i){
            uint32_t args[]={proxy_for_object((id)entries[i]),proxy_for_object((id)entries[count+i]),arguments[2]};
            compat_runtime32_call(arguments[1],args,3);
            if(compat_runtime32_last_call_trapped())break;
        }
        free(entries);*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayApplyFunction")) {
        CFArrayRef array=(CFArrayRef)object_for_argument(arguments[0]);
        for(int32_t i=0;i<(int32_t)arguments[2];++i){
            uint32_t args[]={proxy_for_object((id)CFArrayGetValueAtIndex(array,(int32_t)arguments[1]+i)),arguments[4]};
            compat_runtime32_call(arguments[3],args,2);
            if(compat_runtime32_last_call_trapped())break;
        }
        *result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFUUIDGetConstantUUIDWithBytes")) {
        CFUUIDRef uuid=CFUUIDGetConstantUUIDWithBytes(NULL,
            arguments[1],arguments[2],arguments[3],arguments[4],arguments[5],arguments[6],arguments[7],arguments[8],
            arguments[9],arguments[10],arguments[11],arguments[12],arguments[13],arguments[14],arguments[15],arguments[16]);
        *result=proxy_for_object((id)uuid);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFUUIDGetUUIDBytes")) {
        CFUUIDBytes bytes=CFUUIDGetUUIDBytes((CFUUIDRef)object_for_argument(arguments[1]));
        memcpy((void *)(uintptr_t)arguments[0],&bytes,sizeof(bytes));*result=arguments[0];return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNumberGetTypeID")) { *result=CFNumberGetTypeID();return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_CFBooleanGetTypeID")) { *result=CFBooleanGetTypeID();return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_CFBooleanGetValue")) { *result=CFBooleanGetValue((CFBooleanRef)object_for_argument(arguments[0]));return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_CFDateGetTypeID")) { *result=CFDateGetTypeID();return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_CFSetGetTypeID")) { *result=CFSetGetTypeID();return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetTypeID")) { *result=CFStringGetTypeID();return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLGetTypeID")) { *result=CFURLGetTypeID();return 1; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateStringByAddingPercentEscapes")) {
        CFStringRef value=CFURLCreateStringByAddingPercentEscapes(NULL,
            (CFStringRef)object_for_argument(arguments[1]),(CFStringRef)object_for_argument(arguments[2]),
            (CFStringRef)object_for_argument(arguments[3]),arguments[4]);
        *result=proxy_for_object((id)value);if(value)CFRelease(value);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateStringByReplacingPercentEscapes")) {
        CFStringRef value=CFURLCreateStringByReplacingPercentEscapes(NULL,
            (CFStringRef)object_for_argument(arguments[1]),(CFStringRef)object_for_argument(arguments[2]));
        *result=proxy_for_object((id)value);if(value)CFRelease(value);return 1;
    }
#pragma clang diagnostic pop
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateWithBytes")) {
        CFURLRef value=CFURLCreateWithBytes(NULL,(const UInt8 *)(uintptr_t)arguments[1],
            (int32_t)arguments[2],arguments[3],(CFURLRef)object_for_argument(arguments[4]));
        *result=proxy_for_object((id)value);if(value)CFRelease(value);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLGetString")) {
        *result=proxy_for_object((id)CFURLGetString((CFURLRef)object_for_argument(arguments[0])));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCopyScheme")) {
        CFStringRef value=CFURLCopyScheme((CFURLRef)object_for_argument(arguments[0]));
        *result=proxy_for_object((id)value);if(value)CFRelease(value);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithCString")) {
        CFStringRef string=CFStringCreateWithCString(NULL,(const char *)(uintptr_t)arguments[1],arguments[2]);
        *result=proxy_for_object((id)string);if(string)CFRelease(string);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateExternalRepresentation")) {
        CFDataRef data=CFStringCreateExternalRepresentation(NULL,
            (CFStringRef)object_for_argument(arguments[1]),arguments[2],(UInt8)arguments[3]);
        *result=proxy_for_object((id)data);if(data)CFRelease(data);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCompareWithOptions")) {
        CFRange range = CFRangeMake((int32_t)arguments[2], (int32_t)arguments[3]);
        *result = (uint32_t)CFStringCompareWithOptions((CFStringRef)object_for_argument(arguments[0]),
            (CFStringRef)object_for_argument(arguments[1]), range, arguments[4]);
        return 1;
    }
    if (display_callback_dispatch(import_name,arguments,result)) return 1;
    if (dispatch_bridge32_dispatch(import_name, arguments, result)) return 1;
    if (hid_bridge32_dispatch(import_name,arguments,result)) return 1;
    if (LP32_NAME_IS(import_name,import_length,"_CFSetGetCount")) {*result=(uint32_t)CFSetGetCount((CFSetRef)object_for_argument(arguments[0]));return 1;}
    if (LP32_NAME_IS(import_name,import_length,"_CFSetGetValues") || LP32_NAME_IS(import_name,import_length,"_CFSetApplyFunction")) {
        CFSetRef set=(CFSetRef)object_for_argument(arguments[0]);CFIndex count=CFSetGetCount(set);
        const void **values=calloc(count?count:1,sizeof(*values));if(!values)return 0;CFSetGetValues(set,values);
        for(CFIndex i=0;i<count;++i){uint32_t token=proxy_for_object((id)values[i]);
            if(LP32_NAME_IS(import_name,import_length,"_CFSetGetValues"))((uint32_t *)(uintptr_t)arguments[1])[i]=token;
            else{uint32_t a[]={token,arguments[2]};compat_runtime32_call(arguments[1],a,2);}}
        free(values);*result=0;return 1;
    }
    if (objc_legacy32_property(import_name,arguments,result)) return 1;
    if (LP32_NAME_IS(import_name, import_length, "_NSClassFromString")) {
        NSString *name=object_for_argument(arguments[0]);
        Class klass=name?objc_legacy32_class([name UTF8String]):Nil;
        *result=proxy_for_object(klass);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSStringFromClass")) {
        *result=proxy_for_object(NSStringFromClass((Class)object_for_argument(arguments[0])));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSSelectorFromString")) {
        NSString *name=object_for_argument(arguments[0]);*result=objc_bridge32_guest_selector([name UTF8String]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSStringFromSelector")) {
        const char *name=(void *)(uintptr_t)arguments[0];*result=proxy_for_object(name?[NSString stringWithUTF8String:name]:nil);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFTimeZoneCopyDefault") ||
        LP32_NAME_IS(import_name, import_length, "_CFTimeZoneCreateWithName")) {
        CFTimeZoneRef zone=LP32_NAME_IS(import_name,import_length,"_CFTimeZoneCopyDefault") ?
            CFTimeZoneCopyDefault() : CFTimeZoneCreateWithName(NULL,(CFStringRef)object_for_argument(arguments[1]),arguments[2]!=0);
        *result=proxy_for_object((id)zone);if(zone)CFRelease(zone);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFTimeZoneGetSecondsFromGMT")) {
        double time;memcpy(&time,arguments+1,8);
        *result=compat_runtime32_return_double(CFTimeZoneGetSecondsFromGMT((CFTimeZoneRef)object_for_argument(arguments[0]),time));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDateCreate")) {
        double time;memcpy(&time,arguments+1,8);CFDateRef date=CFDateCreate(NULL,time);
        *result=proxy_for_object((id)date);if(date)CFRelease(date);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDateGetAbsoluteTime")) {
        *result=compat_runtime32_return_double(CFDateGetAbsoluteTime((CFDateRef)object_for_argument(arguments[0])));return 1;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (LP32_NAME_IS(import_name, import_length, "_CFAbsoluteTimeGetGregorianDate")) {
        double time;memcpy(&time,arguments+1,8);
        CFGregorianDate date=CFAbsoluteTimeGetGregorianDate(time,(CFTimeZoneRef)object_for_argument(arguments[3]));
        _Static_assert(sizeof(CFGregorianDate)==16,"Gregorian date layout");
        memcpy((void *)(uintptr_t)arguments[0],&date,16);*result=arguments[0];return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFGregorianDateGetAbsoluteTime")) {
        CFGregorianDate date;memcpy(&date,arguments,16);
        *result=compat_runtime32_return_double(CFGregorianDateGetAbsoluteTime(date,(CFTimeZoneRef)object_for_argument(arguments[4])));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFAbsoluteTimeGetDayOfWeek")) {
        double time;memcpy(&time,arguments,8);
        *result=(uint32_t)CFAbsoluteTimeGetDayOfWeek(time,(CFTimeZoneRef)object_for_argument(arguments[2]));return 1;
    }
#pragma clang diagnostic pop
    if (LP32_NAME_IS(import_name,import_length,"_CFStringFindAndReplace")) {
        *result=(uint32_t)CFStringFindAndReplace((CFMutableStringRef)object_for_argument(arguments[0]),(CFStringRef)object_for_argument(arguments[1]),
            (CFStringRef)object_for_argument(arguments[2]),CFRangeMake((int32_t)arguments[3],(int32_t)arguments[4]),arguments[5]);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CFStringTransform")) {
        int32_t *guest=(void *)(uintptr_t)arguments[1];CFRange range=guest?CFRangeMake(guest[0],guest[1]):CFRangeMake(0,0);
        *result=CFStringTransform((CFMutableStringRef)object_for_argument(arguments[0]),guest?&range:NULL,(CFStringRef)object_for_argument(arguments[2]),arguments[3]!=0);
        if(guest){guest[0]=(int32_t)range.location;guest[1]=(int32_t)range.length;}return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CFStringAppendFormat")) {
        CFStringRef formatted=format_cf_string32((CFStringRef)object_for_argument(arguments[2]),arguments+3);
        if(!formatted)return 0;CFStringAppend((CFMutableStringRef)object_for_argument(arguments[0]),formatted);CFRelease(formatted);*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithFileSystemRepresentation")) {
        CFStringRef string = CFStringCreateWithFileSystemRepresentation(NULL, (void *)(uintptr_t)arguments[1]);
        *result = proxy_for_object((id)string); if(string)CFRelease(string); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetFileSystemRepresentation")) {
        *result = CFStringGetFileSystemRepresentation((CFStringRef)object_for_argument(arguments[0]), (void *)(uintptr_t)arguments[1], (int32_t)arguments[2]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetMaximumSizeOfFileSystemRepresentation")) {
        *result = (uint32_t)CFStringGetMaximumSizeOfFileSystemRepresentation((CFStringRef)object_for_argument(arguments[0]));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringConvertIANACharSetNameToEncoding")) {
        *result = CFStringConvertIANACharSetNameToEncoding((CFStringRef)object_for_argument(arguments[0]));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateArrayBySeparatingStrings")) {
        CFArrayRef array=CFStringCreateArrayBySeparatingStrings(NULL,(CFStringRef)object_for_argument(arguments[1]),(CFStringRef)object_for_argument(arguments[2]));
        *result=proxy_for_object((id)array);if(array)CFRelease(array);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringConvertWindowsCodepageToEncoding")) {*result=CFStringConvertWindowsCodepageToEncoding(arguments[0]);return 1;}
    if (LP32_NAME_IS(import_name, import_length, "_CFStringConvertEncodingToWindowsCodepage")) {*result=CFStringConvertEncodingToWindowsCodepage(arguments[0]);return 1;}
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetSystemEncoding")) {*result=CFStringGetSystemEncoding();return 1;}
    if (LP32_NAME_IS(import_name, import_length, "_CFStringConvertEncodingToNSStringEncoding")) {*result=(uint32_t)CFStringConvertEncodingToNSStringEncoding(arguments[0]);return 1;}
    if (LP32_NAME_IS(import_name, import_length, "_CFStringConvertNSStringEncodingToEncoding")) {*result=CFStringConvertNSStringEncodingToEncoding(arguments[0]);return 1;}
    if (LP32_NAME_IS(import_name, import_length, "_dispatch_semaphore_create")) {
        dispatch_semaphore_t semaphore = dispatch_semaphore_create((int32_t)arguments[0]);
        *result = proxy_for_object((id)semaphore);
        if (semaphore) dispatch_release(semaphore);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_dispatch_semaphore_signal")) {
        *result = fast_dispatch_semaphore_signal(arguments, 0);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_dispatch_semaphore_wait")) {
        *result = fast_dispatch_semaphore_wait(arguments, 0);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_dispatch_time")) {
        *result = fast_dispatch_time(arguments, 0); return 1;
    }
    if ((strncmp(import_name,"_CF",3)==0 || strncmp(import_name,"_SC",3)==0) &&
        cfnetwork_bridge32_dispatch(import_name, arguments, result)) return 1;
    if (LP32_NAME_IS(import_name, import_length, "_ABGetSharedAddressBook") ||
        LP32_NAME_IS(import_name, import_length, "_ABGetMe") ||
        LP32_NAME_IS(import_name, import_length, "_ABRecordCopyValue") ||
        LP32_NAME_IS(import_name, import_length, "_ABMultiValueCopyPrimaryIdentifier") ||
        LP32_NAME_IS(import_name, import_length, "_ABMultiValueCopyValueAtIndex") ||
        LP32_NAME_IS(import_name, import_length, "_ABMultiValueIndexForIdentifier")) {
        static void *address_book;
        if (!address_book) address_book = dlopen(
            "/System/Library/Frameworks/AddressBook.framework/AddressBook", RTLD_NOW | RTLD_LOCAL);
        void *function = address_book ? dlsym(address_book, import_name + 1) : NULL;
        if (!function) return 0;
        id value = nil;
        if (LP32_NAME_IS(import_name, import_length, "_ABGetSharedAddressBook")) {
            value = ((id (*)(void))function)();
        } else {
            id object = object_for_argument(arguments[0]);
            if (LP32_NAME_IS(import_name, import_length, "_ABMultiValueIndexForIdentifier")) {
                *result = object ? (uint32_t)((CFIndex (*)(id, id))function)(
                    object, object_for_argument(arguments[1])) : UINT32_MAX;
                return 1;
            }
            if (object) {
                if (LP32_NAME_IS(import_name, import_length, "_ABRecordCopyValue"))
                    value = ((id (*)(id, id))function)(object, object_for_argument(arguments[1]));
                else if (LP32_NAME_IS(import_name, import_length, "_ABMultiValueCopyValueAtIndex"))
                    value = ((id (*)(id, CFIndex))function)(object, (int32_t)arguments[1]);
                else value = ((id (*)(id))function)(object);
            }
        }
        *result = proxy_for_object(value);
        if (value && strstr(import_name, "Copy")) CFRelease((CFTypeRef)value);
        return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGDataProviderCreateWithData")) {
        struct lp32_provider_data *p=malloc(sizeof(*p));if(!p){*result=0;return 1;}
        *p=(struct lp32_provider_data){arguments[0],arguments[1],arguments[2],arguments[3]};
        CGDataProviderRef provider=CGDataProviderCreateWithData(p,(void *)(uintptr_t)p->data,p->size,release_provider_data);
        if(!provider)free(p);*result=proxy_for_object((id)provider);if(provider)CGDataProviderRelease(provider);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontCreateUIFontForLanguage") || LP32_NAME_IS(import_name,import_length,"_CTFontCreateWithName")) {
        float size;memcpy(&size,arguments+1,4);CTFontRef font;
        if(LP32_NAME_IS(import_name,import_length,"_CTFontCreateUIFontForLanguage"))font=CTFontCreateUIFontForLanguage(arguments[0],size,(CFStringRef)object_for_argument(arguments[2]));
        else {CGAffineTransform transform,*matrix=NULL;if(arguments[2]){const float *f=(void *)(uintptr_t)arguments[2];transform=CGAffineTransformMake(f[0],f[1],f[2],f[3],f[4],f[5]);matrix=&transform;}font=CTFontCreateWithName((CFStringRef)object_for_argument(arguments[0]),size,matrix);}
        *result=proxy_for_object((id)font);if(font)CFRelease(font);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontCreateCopyWithSymbolicTraits")) {
        float size;memcpy(&size,arguments+1,4);CGAffineTransform transform,*matrix=NULL;
        if(arguments[2]){const float *f=(void *)(uintptr_t)arguments[2];transform=CGAffineTransformMake(f[0],f[1],f[2],f[3],f[4],f[5]);matrix=&transform;}
        CTFontRef font=CTFontCreateCopyWithSymbolicTraits((CTFontRef)object_for_argument(arguments[0]),size,matrix,arguments[3],arguments[4]);
        *result=proxy_for_object((id)font);if(font)CFRelease(font);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontCopyGraphicsFont")) {
        CTFontDescriptorRef descriptor=NULL;CGFontRef font=CTFontCopyGraphicsFont((CTFontRef)object_for_argument(arguments[0]),arguments[1]?&descriptor:NULL);
        if(arguments[1])*(uint32_t *)(uintptr_t)arguments[1]=proxy_for_object((id)descriptor);
        if(descriptor)CFRelease(descriptor);*result=proxy_for_object((id)font);if(font)CGFontRelease(font);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontGetGlyphsForCharacters")) {
        *result=CTFontGetGlyphsForCharacters((CTFontRef)object_for_argument(arguments[0]),(void *)(uintptr_t)arguments[1],(void *)(uintptr_t)arguments[2],(int32_t)arguments[3]);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontGetAdvancesForGlyphs")) {
        uint32_t count=arguments[4];if(count>1048576)return 0;CGSize *sizes=arguments[3]?calloc(count?count:1,sizeof(*sizes)):NULL;
        double advance=CTFontGetAdvancesForGlyphs((CTFontRef)object_for_argument(arguments[0]),arguments[1],(void *)(uintptr_t)arguments[2],sizes,count);
        if(sizes){float *out=(void *)(uintptr_t)arguments[3];for(uint32_t i=0;i<count;++i){out[i*2]=sizes[i].width;out[i*2+1]=sizes[i].height;}free(sizes);}
        *result=compat_runtime32_return_double(advance);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontGetBoundingBox") || LP32_NAME_IS(import_name,import_length,"_CTFontGetBoundingRectsForGlyphs") || LP32_NAME_IS(import_name,import_length,"_CTLineGetImageBounds")) {
        CGRect rect;
        if(LP32_NAME_IS(import_name,import_length,"_CTFontGetBoundingBox"))rect=CTFontGetBoundingBox((CTFontRef)object_for_argument(arguments[1]));
        else if(LP32_NAME_IS(import_name,import_length,"_CTLineGetImageBounds"))rect=CTLineGetImageBounds((CTLineRef)object_for_argument(arguments[1]),(CGContextRef)object_for_argument(arguments[2]));
        else {uint32_t count=arguments[5];if(count>1048576)return 0;CGRect *rects=arguments[4]?calloc(count?count:1,sizeof(*rects)):NULL;
            rect=CTFontGetBoundingRectsForGlyphs((CTFontRef)object_for_argument(arguments[1]),arguments[2],(void *)(uintptr_t)arguments[3],rects,count);
            if(rects){float *out=(void *)(uintptr_t)arguments[4];for(uint32_t i=0;i<count;++i){out[i*4]=rects[i].origin.x;out[i*4+1]=rects[i].origin.y;out[i*4+2]=rects[i].size.width;out[i*4+3]=rects[i].size.height;}free(rects);}}
        float out[]={rect.origin.x,rect.origin.y,rect.size.width,rect.size.height};memcpy((void *)(uintptr_t)arguments[0],out,16);*result=arguments[0];return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CTLineCreateWithAttributedString")) {
        CTLineRef line=CTLineCreateWithAttributedString((CFAttributedStringRef)object_for_argument(arguments[0]));*result=proxy_for_object((id)line);if(line)CFRelease(line);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CTLineDraw")) {CTLineDraw((CTLineRef)object_for_argument(arguments[0]),(CGContextRef)object_for_argument(arguments[1]));*result=0;return 1;}
    if (LP32_NAME_IS(import_name,import_length,"_CTFontGetAscent")) { *result=compat_runtime32_return_double(CTFontGetAscent((CTFontRef)object_for_argument(arguments[0])));return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontGetDescent")) { *result=compat_runtime32_return_double(CTFontGetDescent((CTFontRef)object_for_argument(arguments[0])));return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontGetLeading")) { *result=compat_runtime32_return_double(CTFontGetLeading((CTFontRef)object_for_argument(arguments[0])));return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CTFontGetSize")) { *result=compat_runtime32_return_double(CTFontGetSize((CTFontRef)object_for_argument(arguments[0])));return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGBitmapContextCreate")) {
        CGContextRef context=CGBitmapContextCreate((void *)(uintptr_t)arguments[0],arguments[1],arguments[2],arguments[3],arguments[4],(CGColorSpaceRef)object_for_argument(arguments[5]),arguments[6]);
        *result=proxy_for_object((id)context);if(context)CGContextRelease(context);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGColorSpaceCreateDeviceGray")) {
        CGColorSpaceRef color=CGColorSpaceCreateDeviceGray();*result=proxy_for_object((id)color);CGColorSpaceRelease(color);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGColorSpaceCreateDeviceRGB")) {
        CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();*result=proxy_for_object((id)color);CGColorSpaceRelease(color);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGColorSpaceCopyICCProfile")) {
        /* Complete Saga's movie decoder looks for Apple's HDTV profile to
           pick the Rec. 709 YUV matrix for each attract/cutscene movie. */
        CGColorSpaceRef color=(CGColorSpaceRef)object_for_argument(arguments[0]);
        CFDataRef profile=color?CGColorSpaceCopyICCData(color):NULL;
        *result=profile?proxy_for_object((id)profile):0;if(profile)CFRelease(profile);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGImageCreate")) {
        CGColorSpaceRef color=(CGColorSpaceRef)object_for_argument(arguments[5]);
        CGFloat decode[32];const CGFloat *values=NULL;
        if(arguments[8]){size_t count=color?CGColorSpaceGetNumberOfComponents(color)*2:0;if(count>32)return 0;const float *guest=(void *)(uintptr_t)arguments[8];for(size_t i=0;i<count;++i)decode[i]=guest[i];values=decode;}
        CGImageRef image=CGImageCreate(arguments[0],arguments[1],arguments[2],arguments[3],arguments[4],color,arguments[6],(CGDataProviderRef)object_for_argument(arguments[7]),values,arguments[9]!=0,arguments[10]);
        *result=proxy_for_object((id)image);if(image)CGImageRelease(image);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGImageGetWidth") || LP32_NAME_IS(import_name,import_length,"_CGImageGetHeight")) {
        CGImageRef image=(CGImageRef)object_for_argument(arguments[0]);*result=LP32_NAME_IS(import_name,import_length,"_CGImageGetWidth")?CGImageGetWidth(image):CGImageGetHeight(image);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextSaveGState") || LP32_NAME_IS(import_name,import_length,"_CGContextRestoreGState")) {
        CGContextRef context=(CGContextRef)object_for_argument(arguments[0]);
        if(LP32_NAME_IS(import_name,import_length,"_CGContextSaveGState"))CGContextSaveGState(context);else CGContextRestoreGState(context);*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextClearRect") || LP32_NAME_IS(import_name,import_length,"_CGContextAddRect") || LP32_NAME_IS(import_name,import_length,"_CGContextDrawImage") || LP32_NAME_IS(import_name,import_length,"_CGContextFillRect") || LP32_NAME_IS(import_name,import_length,"_CGContextStrokeRect") || LP32_NAME_IS(import_name,import_length,"_CGContextClipToRect")) {
        CGContextRef context=(CGContextRef)object_for_argument(arguments[0]);float f[4];memcpy(f,arguments+1,sizeof(f));CGRect rect=CGRectMake(f[0],f[1],f[2],f[3]);
        if(LP32_NAME_IS(import_name,import_length,"_CGContextDrawImage"))CGContextDrawImage(context,rect,(CGImageRef)object_for_argument(arguments[5]));
        else if(LP32_NAME_IS(import_name,import_length,"_CGContextFillRect"))CGContextFillRect(context,rect);
        else if(LP32_NAME_IS(import_name,import_length,"_CGContextStrokeRect"))CGContextStrokeRect(context,rect);
        else if(LP32_NAME_IS(import_name,import_length,"_CGContextClearRect"))CGContextClearRect(context,rect);
        else if(LP32_NAME_IS(import_name,import_length,"_CGContextAddRect"))CGContextAddRect(context,rect);
        else CGContextClipToRect(context,rect);*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextSetRGBFillColor") || LP32_NAME_IS(import_name,import_length,"_CGContextSetRGBStrokeColor")) {
        float f[4];memcpy(f,arguments+1,sizeof(f));CGContextRef context=(CGContextRef)object_for_argument(arguments[0]);
        if(LP32_NAME_IS(import_name,import_length,"_CGContextSetRGBFillColor"))CGContextSetRGBFillColor(context,f[0],f[1],f[2],f[3]);else CGContextSetRGBStrokeColor(context,f[0],f[1],f[2],f[3]);*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextTranslateCTM") || LP32_NAME_IS(import_name,import_length,"_CGContextScaleCTM")) {
        float f[2];memcpy(f,arguments+1,sizeof(f));CGContextRef context=(CGContextRef)object_for_argument(arguments[0]);
        if(LP32_NAME_IS(import_name,import_length,"_CGContextTranslateCTM"))CGContextTranslateCTM(context,f[0],f[1]);else CGContextScaleCTM(context,f[0],f[1]);*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextFlush")) { CGContextFlush((CGContextRef)object_for_argument(arguments[0])); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextSynchronize")) { CGContextSynchronize((CGContextRef)object_for_argument(arguments[0])); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextClip")) { CGContextClip((CGContextRef)object_for_argument(arguments[0])); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextEOClip")) { CGContextEOClip((CGContextRef)object_for_argument(arguments[0])); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextFillPath")) { CGContextFillPath((CGContextRef)object_for_argument(arguments[0])); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextStrokePath")) { CGContextStrokePath((CGContextRef)object_for_argument(arguments[0])); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextSetBlendMode")) { CGContextSetBlendMode((CGContextRef)object_for_argument(arguments[0]),(CGBlendMode)arguments[1]); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextSetShouldAntialias")) { CGContextSetShouldAntialias((CGContextRef)object_for_argument(arguments[0]),(bool)arguments[1]); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextMoveToPoint")) { float f[2];memcpy(f,arguments+1,8);CGContextMoveToPoint((CGContextRef)object_for_argument(arguments[0]),f[0],f[1]); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextAddLineToPoint")) { float f[2];memcpy(f,arguments+1,8);CGContextAddLineToPoint((CGContextRef)object_for_argument(arguments[0]),f[0],f[1]); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextSetTextPosition")) { float f[2];memcpy(f,arguments+1,8);CGContextSetTextPosition((CGContextRef)object_for_argument(arguments[0]),f[0],f[1]); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextSetLineWidth")) { float f;memcpy(&f,arguments+1,4);CGContextSetLineWidth((CGContextRef)object_for_argument(arguments[0]),f); *result=0;return 1; }
    if (LP32_NAME_IS(import_name,import_length,"_CGContextSetCharacterSpacing")) { float f;memcpy(&f,arguments+1,4);CGContextSetCharacterSpacing((CGContextRef)object_for_argument(arguments[0]),f); *result=0;return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_IOPMAssertionCreateWithName")) {
        CFStringRef type = (CFStringRef)object_for_argument(arguments[0]);
        CFStringRef name = (CFStringRef)object_for_argument(arguments[2]);
        IOPMAssertionID assertion = kIOPMNullAssertionID;
        *result = IOPMAssertionCreateWithName(type, arguments[1], name, &assertion);
        if (arguments[3]) *(uint32_t *)(uintptr_t)arguments[3] = assertion;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGSetLocalEventsSuppressionInterval")) {
        double interval;memcpy(&interval,arguments,8);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        *result=background_test_mode()?kCGErrorSuccess:CGSetLocalEventsSuppressionInterval(interval);
#pragma clang diagnostic pop
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayIsMain")) {
        *result = CGDisplayIsMain(arguments[0]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGMainDisplayID")) {
        *result = preferred_game_display_id();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayBounds")) {
        CGRect bounds = CGDisplayBounds(arguments[1]);
        float rect[] = {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height};
        memcpy((void *)(uintptr_t)arguments[0], rect, sizeof(rect));
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGGetOnlineDisplayList") ||
        LP32_NAME_IS(import_name, import_length, "_CGGetActiveDisplayList")) {
        *result = strstr(import_name, "Online") ?
            CGGetOnlineDisplayList(arguments[0], (void *)(uintptr_t)arguments[1], (void *)(uintptr_t)arguments[2]) :
            CGGetActiveDisplayList(arguments[0], (void *)(uintptr_t)arguments[1], (void *)(uintptr_t)arguments[2]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayIDToOpenGLDisplayMask")) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        *result = CGDisplayIDToOpenGLDisplayMask(arguments[0]);
#pragma clang diagnostic pop
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayAvailableModes") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayCopyAllDisplayModes")) {
        materialize_automatic_display_mode();
        NSArray *modes = legacy_modes_for_display(arguments[0]);
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: display %u has %lu legacy modes\n",
                    arguments[0], (unsigned long)[modes count]);
        }
        *result = proxy_for_object(modes);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayCurrentMode") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayCopyDisplayMode")) {
        materialize_automatic_display_mode();
        NSDictionary *mode = legacy_current_mode_for_display(arguments[0]);
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: current legacy display mode: %s\n",
                    [[mode description] UTF8String]);
        }
        *result = proxy_for_object(mode);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayModeGetWidth") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayModeGetHeight")) {
        NSDictionary *mode = object_for_argument(arguments[0]);
        NSString *key = LP32_NAME_IS(import_name, import_length, "_CGDisplayModeGetWidth") ? @"Width" : @"Height";
        *result = [[mode objectForKey:key] unsignedIntValue];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayModeGetIOFlags") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayModeGetIODisplayModeID")) {
        NSDictionary *mode = object_for_argument(arguments[0]);
        NSString *key = strstr(import_name, "IOFlags") ? @"IOFlags" : @"IODisplayModeID";
        *result = [[mode objectForKey:key] unsignedIntValue];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayMirrorsDisplay")) {
        *result = CGDisplayMirrorsDisplay(arguments[0]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayModeGetRefreshRate")) {
        NSDictionary *mode = object_for_argument(arguments[0]);
        double refresh = [[mode objectForKey:@"RefreshRate"] doubleValue];
        *result = compat_runtime32_return_double(refresh);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayModeCopyPixelEncoding")) {
        *result = proxy_for_object(@"--------RRRRRRRRGGGGGGGGBBBBBBBB");
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayModeRelease")) {
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGWindowLevelForKey")) {
        *result = (uint32_t)CGWindowLevelForKey((CGWindowLevelKey)arguments[0]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayIOServicePort")) {
        /* Removed by Apple; callers use zero as "no registry service". */
        *result = 0;
        return 1;
    }
    /*
     * Display capture and mode switching (Clone Wars' Feral layer).  The
     * bridge presents the game in a window that already covers the preferred
     * display, and modern macOS has no exclusive fullscreen or CGL fullscreen
     * drawables, so capture/fade succeed without doing anything, the "best
     * mode" is whatever legacy mode matches the request on that display, and
     * a mode switch becomes the emulated mode (see guest_display_mode) that
     * the GL surface is rendered at and that later queries report back.
     */
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplaySwitchToMode") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplaySetDisplayMode")) {
        NSDictionary *mode = object_for_argument(arguments[1]);
        NSSize requested = NSMakeSize([[mode objectForKey:@"Width"] doubleValue],
                                      [[mode objectForKey:@"Height"] doubleValue]);
        if (mode) {
            set_guest_display_mode(requested, arguments[0]);
            [presenting_view applyGuestDisplayMode];
        }
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: _CGDisplaySwitchToMode %.0fx%.0f (emulated)\n",
                    requested.width, requested.height);
        }
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayRelease") ||
        LP32_NAME_IS(import_name, import_length, "_CGReleaseAllDisplays")) {
        /* Releasing another display must not discard this display's mode. */
        if (LP32_NAME_IS(import_name, import_length, "_CGReleaseAllDisplays") ||
            arguments[0] == guest_display_mode_display) {
            set_guest_display_mode(NSZeroSize, 0);
            [presenting_view applyGuestDisplayMode];
        }
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayCapture") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayHideCursor") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayShowCursor") ||
        LP32_NAME_IS(import_name, import_length, "_CGDisplayFade") ||
        LP32_NAME_IS(import_name, import_length, "_CGReleaseDisplayFadeReservation")) {
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: %s ignored (windowed presentation)\n",
                    import_name);
        }
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGAcquireDisplayFadeReservation")) {
        uint32_t *token = (void *)(uintptr_t)arguments[1];
        if (token) *token = 1;
        *result = kCGErrorSuccess;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayIsCaptured")) {
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGShieldingWindowLevel")) {
        *result = (uint32_t)CGShieldingWindowLevel();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayBestModeForParameters")) {
        materialize_automatic_display_mode();
        uint32_t width = arguments[2];
        uint32_t height = arguments[3];
        uint32_t *exact_match = (void *)(uintptr_t)arguments[4];
        NSDictionary *best = nil;
        /* First launch (no pcconfig.txt yet): the binary's built-in 1024x768
           only stood in for the resolution Feral's launcher dialog chose, and
           that dialog is bypassed, so answer "no such mode"; the game then
           adopts the display's current mode and writes it to pcconfig.txt,
           as the launcher would have proposed.  Later launches honour the
           file (and the in-game Options). */
        static bool applied_first_launch_default;
        bool first_launch = compat_runtime32_game_config_missing() &&
            !applied_first_launch_default && !getenv("LP32_KEEP_DEFAULT_RESOLUTION");
        if (first_launch) {
            applied_first_launch_default = true;
            NSSize native = display_native_size(arguments[0]);
            /* The game re-validates its ScreenWidth/ScreenHeight against the
               mode list after this call, so move those to the native size as
               well or it would go back to the placeholder. */
            const struct lp32_display_layout *display = lp32_profile()->display;
            if (display && display->screen_width && display->screen_height) {
                *(volatile int32_t *)(uintptr_t)display->screen_width =
                    (int32_t)native.width;
                *(volatile int32_t *)(uintptr_t)display->screen_height =
                    (int32_t)native.height;
            }
            fprintf(stderr,
                    "compat32: no pcconfig.txt; starting at the display resolution "
                    "%.0fx%.0f instead of %ux%u\n", native.width, native.height,
                    width, height);
        }
        for (NSDictionary *candidate in legacy_modes_for_display(arguments[0])) {
            if (first_launch) break;
            if ([[candidate objectForKey:@"Width"] unsignedIntValue] == width &&
                [[candidate objectForKey:@"Height"] unsignedIntValue] == height) {
                best = candidate;
                break;
            }
        }
        if (exact_match) *exact_match = best ? 1 : 0;
        if (!best) best = legacy_current_mode_for_display(arguments[0]);
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr,
                    "compat32: CGDisplayBestModeForParameters %ux%u@%ubpp -> %s\n",
                    width, height, arguments[1], [[best description] UTF8String]);
        }
        *result = proxy_for_object(best);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLChoosePixelFormat")) {
        /* Attributes are an int-terminated list; strip kCGLPFAFullScreen
           (no longer honoured) and let CGL pick the rest. */
        const int32_t *guest_attributes = (const void *)(uintptr_t)arguments[0];
        CGLPixelFormatAttribute attributes[64];
        size_t count = 0;
        while (guest_attributes && guest_attributes[count] && count < 62) {
            attributes[count] = (CGLPixelFormatAttribute)guest_attributes[count];
            ++count;
            /* Attributes taking a value: skip the value too. */
            switch (attributes[count - 1]) {
                case kCGLPFAAuxBuffers: case kCGLPFAColorSize:
                case kCGLPFAAlphaSize: case kCGLPFADepthSize:
                case kCGLPFAStencilSize: case kCGLPFAAccumSize:
                case kCGLPFASampleBuffers: case kCGLPFASamples:
                case kCGLPFARendererID: case kCGLPFADisplayMask:
                case kCGLPFAOpenGLProfile: case kCGLPFAVirtualScreenCount:
                    attributes[count] = (CGLPixelFormatAttribute)guest_attributes[count];
                    ++count;
                    break;
                default:
                    break;
            }
        }
        attributes[count] = 0;
        size_t write = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        for (size_t read = 0; read < count; ++read) {
            if (attributes[read] == kCGLPFAFullScreen) continue;
            attributes[write++] = attributes[read];
        }
#pragma clang diagnostic pop
        attributes[write] = 0;
        CGLPixelFormatObj pixel_format = NULL;
        GLint virtual_screens = 0;
        CGLError error = CGLChoosePixelFormat(attributes, &pixel_format,
                                              &virtual_screens);
        uint32_t *guest_pixel_format = (void *)(uintptr_t)arguments[1];
        int32_t *guest_count = (void *)(uintptr_t)arguments[2];
        if (guest_pixel_format) {
            *guest_pixel_format = pixel_format ?
                proxy_for_object([NSValue valueWithPointer:pixel_format]) : 0;
        }
        if (guest_count) *guest_count = virtual_screens;
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: CGLChoosePixelFormat error=%d screens=%d\n",
                    error, virtual_screens);
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLCreateContext")) {
        NSValue *format_wrapper = object_for_argument(arguments[0]);
        NSValue *share_wrapper = object_for_argument(arguments[1]);
        CGLPixelFormatObj pixel_format =
            format_wrapper ? [format_wrapper pointerValue] : NULL;
        CGLContextObj share = share_wrapper ? [share_wrapper pointerValue] : NULL;
        CGLContextObj context = NULL;
        CGLError error = pixel_format ?
            CGLCreateContext(pixel_format, share, &context) : kCGLBadPixelFormat;
        uint32_t *guest_context = (void *)(uintptr_t)arguments[2];
        if (guest_context) {
            *guest_context = context ?
                proxy_for_object([NSValue valueWithPointer:context]) : 0;
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLDestroyContext")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
        *result = context ? (uint32_t)CGLDestroyContext(context) : kCGLBadContext;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLDestroyPixelFormat")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLPixelFormatObj pixel_format = wrapper ? [wrapper pointerValue] : NULL;
        *result = pixel_format ?
            (uint32_t)CGLDestroyPixelFormat(pixel_format) : kCGLBadPixelFormat;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLSetFullScreen") ||
        LP32_NAME_IS(import_name, import_length, "_CGLClearDrawable")) {
        /* The drawable is always the window's NSOpenGLView. */
        *result = kCGLNoError;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLQueryRendererInfo")) {
        CGLRendererInfoObj info = NULL;
        GLint count = 0;
        CGLError error = CGLQueryRendererInfo(arguments[0], &info, &count);
        uint32_t *guest_info = (void *)(uintptr_t)arguments[1];
        int32_t *guest_count = (void *)(uintptr_t)arguments[2];
        if (guest_info) {
            *guest_info = info ? proxy_for_object([NSValue valueWithPointer:info]) : 0;
        }
        if (guest_count) *guest_count = count;
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLDescribeRenderer")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLRendererInfoObj info = wrapper ? [wrapper pointerValue] : NULL;
        GLint value = 0;
        CGLError error = info ?
            CGLDescribeRenderer(info, (GLint)arguments[1],
                                (CGLRendererProperty)arguments[2], &value) :
            kCGLBadRendererInfo;
        /* Old titles size texture budgets from kCGLRPVideoMemory in bytes,
           which overflowed 32 bits long ago; report a comfortable 1 GiB. */
        if (error == kCGLNoError &&
            (arguments[2] == kCGLRPVideoMemoryMegabytes ||
             arguments[2] == kCGLRPTextureMemoryMegabytes)) {
            if (value > 1024) value = 1024;
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (arguments[2] == kCGLRPVideoMemory || arguments[2] == kCGLRPTextureMemory) {
            value = 1024 * 1024 * 1024;
            error = kCGLNoError;
        }
#pragma clang diagnostic pop
        int32_t *guest_value = (void *)(uintptr_t)arguments[3];
        if (guest_value) *guest_value = value;
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: CGLDescribeRenderer renderer=%d prop=%u -> %d (err=%d)\n",
                    (int)arguments[1], arguments[2], value, error);
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLDestroyRendererInfo")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLRendererInfoObj info = wrapper ? [wrapper pointerValue] : NULL;
        *result = info ? (uint32_t)CGLDestroyRendererInfo(info) : kCGLBadRendererInfo;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLFlushDrawable")) {
        static uint64_t presented_frames;
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
        uint64_t flush_start=hitch_recorder_enabled?hitch_now():0;
        {
            /* Test aid: `touch $LP32_CAPTURE_TRIGGER` dumps the next CGL
               back buffer to $LP32_CAPTURE_TRIGGER.ppm. */
            static const char *trigger;
            static bool checked;
            if (!checked) { trigger = getenv("LP32_CAPTURE_TRIGGER"); checked = true; }
            if (trigger && context && access(trigger, F_OK) == 0) {
                unlink(trigger);
                GLint viewport[4] = {0};
                glGetIntegerv(GL_VIEWPORT, viewport);
                size_t row = (size_t)viewport[2] * 3, size = row * (size_t)viewport[3];
                uint8_t *pixels = size ? malloc(size) : NULL;
                if (pixels) {
                    GLint old_read = GL_BACK, old_align = 4;
                    glGetIntegerv(GL_READ_BUFFER, &old_read);
                    glGetIntegerv(GL_PACK_ALIGNMENT, &old_align);
                    glReadBuffer(GL_BACK);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3],
                                 GL_RGB, GL_UNSIGNED_BYTE, pixels);
                    glReadBuffer((GLenum)old_read);
                    glPixelStorei(GL_PACK_ALIGNMENT, old_align);
                    char path[4096];
                    snprintf(path, sizeof(path), "%s.ppm", trigger);
                    FILE *file = fopen(path, "wb");
                    if (file) {
                        fprintf(file, "P6\n%d %d\n255\n", viewport[2], viewport[3]);
                        for (GLint y = viewport[3] - 1; y >= 0; --y)
                            fwrite(pixels + (size_t)y * row, row, 1, file);
                        fclose(file);
                    }
                    free(pixels);
                }
            }
        }
        *result = context ? CGLFlushDrawable(context) : kCGLBadContext;
        poll_cgl_test_input();
        if(lp32_profile()->title==LP32_TITLE_COMPLETE_SAGA)
            controller_bridge32_poll(presented_frames+1);
        /* Same cap as the NSOpenGLView path: at most 60 FPS (less on a
           slower display).  The TT engine's simulation is frame-rate bound;
           above 60 FPS the PC release breaks physics and puzzles. */
        if (context) frame_pacer_wait(nil);
        ++presented_frames;
        correct_saga_automatic_camera_ratio(presented_frames);
        trace_saga_aspect_state(presented_frames);
        poll_debug_mem_fifo();
        if(hitch_recorder_enabled) {
            uint64_t end=hitch_now();
            hitch_frame(presented_frames,flush_start,end,end,0,
                [NSApp isActive] || getenv("LP32_BACKGROUND_TEST"));
        }
        if (getenv("LP32_TRACE_GL_FRAMES")) {
            static uint64_t frames, previous, worst;
            static unsigned slow_frames;
            uint64_t now=hitch_now();
            if(previous) {
                uint64_t duration=now-previous;
                if(duration>worst)worst=duration;
                if(duration>50000000)++slow_frames;
            }
            previous=now;
            if (++frames <= 3 || frames % 120 == 0) {
                GLint viewport[4], framebuffer, swap_interval = -1;
                if(context) CGLGetParameter(context,kCGLCPSwapInterval,&swap_interval);
                glGetIntegerv(GL_VIEWPORT, viewport);
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
                fprintf(stderr, "compat32: CGL present=%llu end=%.6f context=%p current=%p status=%u viewport=%d,%d,%d,%d fbo=%d swap_interval=%d max_ms=%.2f slow50=%u\n",
                    (unsigned long long)frames, now / 1e9, context, CGLGetCurrentContext(),
                    (unsigned)*result, viewport[0], viewport[1], viewport[2], viewport[3], framebuffer, swap_interval,
                    worst/1e6,slow_frames);
                worst=0;slow_frames=0;
                if (compat_runtime32_frame_profile_enabled && getenv("LP32_FRAME_STATS_IMPORTS"))
                    compat_runtime32_report_import_profile((unsigned)strtoul(getenv("LP32_FRAME_STATS_IMPORTS"), NULL, 0));
            }
        }
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLGetShareGroup") ||
        LP32_NAME_IS(import_name, import_length, "_CGLGetPixelFormat")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
        void *pointer = !context ? NULL :
            LP32_NAME_IS(import_name, import_length, "_CGLGetShareGroup") ?
            (void *)CGLGetShareGroup(context) : (void *)CGLGetPixelFormat(context);
        *result = objc_bridge32_guest_pointer(pointer);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLEnable") ||
        LP32_NAME_IS(import_name, import_length, "_CGLDisable") ||
        LP32_NAME_IS(import_name, import_length, "_CGLIsEnabled")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
        CGLError error = kCGLBadContext;
        if (context) {
            CGLContextEnable capability = (CGLContextEnable)arguments[1];
            if (LP32_NAME_IS(import_name, import_length, "_CGLEnable"))
                error = CGLEnable(context, capability);
            else if (LP32_NAME_IS(import_name, import_length, "_CGLDisable"))
                error = CGLDisable(context, capability);
            else
                error = CGLIsEnabled(context, capability, (GLint *)(uintptr_t)arguments[2]);
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLGetParameter")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
        *result = context ? CGLGetParameter(context, arguments[1], (void *)(uintptr_t)arguments[2]) : kCGLBadContext;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLGetCurrentContext")) {
        *result = fast_CGLGetCurrentContext(arguments, 0);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGLSetCurrentContext") ||
        LP32_NAME_IS(import_name, import_length, "_CGLLockContext") ||
        LP32_NAME_IS(import_name, import_length, "_CGLUnlockContext") ||
        LP32_NAME_IS(import_name, import_length, "_CGLSetParameter")) {
        NSValue *wrapper = object_for_argument(arguments[0]);
        CGLContextObj context = wrapper ? [wrapper pointerValue] : NULL;
        CGLError error;
        if (LP32_NAME_IS(import_name, import_length, "_CGLSetCurrentContext")) {
            error = CGLSetCurrentContext(context);
        } else if (LP32_NAME_IS(import_name, import_length, "_CGLLockContext")) {
            error = CGLLockContext(context);
        } else if (LP32_NAME_IS(import_name, import_length, "_CGLUnlockContext")) {
            error = CGLUnlockContext(context);
        } else {
            const GLint *values = (const GLint *)(uintptr_t)arguments[2];
            GLint override_value = 0;
            if (arguments[1] == kCGLCPSwapInterval && values &&
                frame_pacer_takes_over_swap_interval(*values)) {
                values = &override_value;
            }
            error = CGLSetParameter(context, arguments[1], values);
        }
        *result = (uint32_t)error;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetError")) {
        *result = glGetError();
        if (*result != GL_NO_ERROR && trace_gl_render_call()) {
            fprintf(stderr, "compat32: glGetError swap=%llu error=%04llx\n",
                    (unsigned long long)objc_bridge_swap_count,
                    (unsigned long long)*result);
        }
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glActiveTexture") ||
        LP32_NAME_IS(import_name, import_length, "glActiveTextureARB")) {
        *result = fast_glActiveTexture(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glClientActiveTexture") ||
        LP32_NAME_IS(import_name, import_length, "glClientActiveTextureARB")) {
        *result = fast_glClientActiveTexture(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenBuffers") ||
        LP32_NAME_IS(import_name, import_length, "glGenBuffersARB")) {
        glGenBuffers(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteBuffers") ||
        LP32_NAME_IS(import_name, import_length, "glDeleteBuffersARB")) {
        const GLuint *buffers = (const void *)(uintptr_t)arguments[1];
        CGLContextObj context = CGLGetCurrentContext();
        for (GLsizei index = 0; index < (GLsizei)arguments[0]; ++index) {
            discard_buffer_state(context, buffers[index]);
        }
        glDeleteBuffers(arguments[0], buffers);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBindBuffer") ||
        LP32_NAME_IS(import_name, import_length, "glBindBufferARB")) {
        *result = fast_glBindBuffer(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBufferData") ||
        LP32_NAME_IS(import_name, import_length, "glBufferDataARB")) {
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glBufferData swap=%llu target=%04x size=%d "
                "data=%08x usage=%04x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (int32_t)arguments[1], arguments[2], arguments[3]);
        }
        glBufferData(arguments[0], (GLsizeiptr)(int32_t)arguments[1],
                     (const void *)(uintptr_t)arguments[2], arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBufferSubData") ||
        LP32_NAME_IS(import_name, import_length, "glBufferSubDataARB")) {
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glBufferSubData swap=%llu target=%04x offset=%d "
                "size=%d data=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (int32_t)arguments[1], (int32_t)arguments[2], arguments[3]);
        }
        glBufferSubData(arguments[0], (GLintptr)(int32_t)arguments[1],
                        (GLsizeiptr)(int32_t)arguments[2],
                        (const void *)(uintptr_t)arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBufferParameteriAPPLE")) {
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_state *state =
            buffer_state_for_name(context, buffer, true);
        if (state && arguments[1] == GL_BUFFER_FLUSHING_UNMAP_APPLE) {
            state->flush_on_unmap = arguments[2] != 0;
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glBufferParameteriAPPLE swap=%llu "
                    "target=%04x buffer=%u pname=%04x value=%u "
                    "flush_unmap=%d\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    buffer, arguments[1], arguments[2],
                    state ? state->flush_on_unmap : -1);
        }
        glBufferParameteriAPPLE(arguments[0], arguments[1], arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glMapBuffer") ||
        LP32_NAME_IS(import_name, import_length, "glMapBufferARB")) {
        GLint byte_count = 0;
        glGetBufferParameteriv(arguments[0], GL_BUFFER_SIZE, &byte_count);
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_state *state =
            buffer_state_for_name(context, buffer, true);
        struct guest_buffer_mapping *mapping = state ? begin_buffer_mapping(
            context, arguments[0], buffer, state) : NULL;
        if (!mapping || byte_count < 0) {
            deactivate_buffer_mapping(mapping);
            *result = 0;
            return 1;
        }
        size_t size = (size_t)byte_count;
        if (size > mapping->capacity) {
            uint32_t storage = compat_runtime32_reallocate(
                mapping->storage, size ? size : 1);
            if (!storage) {
                deactivate_buffer_mapping(mapping);
                *result = 0;
                return 1;
            }
            mapping->storage = storage;
            mapping->capacity = size;
        }
        mapping->access = arguments[1];
        mapping->access_flags = 0;
        mapping->buffer_size = size;
        mapping->mapped_offset = 0;
        mapping->size = size;
        mapping->range_mapping = false;
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glMapBuffer swap=%llu target=%04x "
                    "buffer=%u access=%04x size=%zu staging=%08x "
                    "flush_unmap=%d\n",
                    (unsigned long long)objc_bridge_swap_count,
                    mapping->target, buffer, mapping->access, mapping->size,
                    mapping->storage, state->flush_on_unmap);
        }
        if (size && (mapping->access != GL_WRITE_ONLY ||
                     state->flush_on_unmap)) {
            glGetBufferSubData(mapping->target, 0, (GLsizeiptr)size,
                               (void *)(uintptr_t)mapping->storage);
        }
        *result = mapping->storage;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glMapBufferRange")) {
        GLint byte_count = 0;
        glGetBufferParameteriv(arguments[0], GL_BUFFER_SIZE, &byte_count);
        int32_t signed_offset = (int32_t)arguments[1];
        int32_t signed_size = (int32_t)arguments[2];
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_state *state =
            buffer_state_for_name(context, buffer, true);
        struct guest_buffer_mapping *mapping = state ? begin_buffer_mapping(
            context, arguments[0], buffer, state) : NULL;
        bool valid = mapping && byte_count >= 0 && signed_offset >= 0 &&
            signed_size >= 0 && (size_t)signed_offset <= (size_t)byte_count &&
            (size_t)signed_size <= (size_t)byte_count - (size_t)signed_offset;
        if (!valid) {
            deactivate_buffer_mapping(mapping);
            *result = 0;
            return 1;
        }
        size_t size = (size_t)signed_size;
        if (size > mapping->capacity) {
            uint32_t storage = compat_runtime32_reallocate(
                mapping->storage, size ? size : 1);
            if (!storage) {
                deactivate_buffer_mapping(mapping);
                *result = 0;
                return 1;
            }
            mapping->storage = storage;
            mapping->capacity = size;
        }
        mapping->access = 0;
        mapping->access_flags = arguments[3];
        mapping->buffer_size = (size_t)byte_count;
        mapping->mapped_offset = (size_t)signed_offset;
        mapping->size = size;
        mapping->range_mapping = true;
        bool readable = (mapping->access_flags & GL_MAP_READ_BIT) != 0;
        bool preserve = (mapping->access_flags &
                         (GL_MAP_INVALIDATE_RANGE_BIT |
                          GL_MAP_INVALIDATE_BUFFER_BIT)) == 0;
        if (size && (readable || preserve)) {
            glGetBufferSubData(mapping->target,
                               (GLintptr)mapping->mapped_offset,
                               (GLsizeiptr)mapping->size,
                               (void *)(uintptr_t)mapping->storage);
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glMapBufferRange swap=%llu target=%04x "
                    "buffer=%u offset=%zu size=%zu access=%04x "
                    "staging=%08x preserve=%d\n",
                    (unsigned long long)objc_bridge_swap_count,
                    mapping->target, buffer, mapping->mapped_offset,
                    mapping->size, mapping->access_flags, mapping->storage,
                    preserve);
        }
        *result = mapping->storage;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glUnmapBuffer") ||
        LP32_NAME_IS(import_name, import_length, "glUnmapBufferARB")) {
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_mapping *mapping =
            active_buffer_mapping(context, arguments[0], buffer);
        struct guest_buffer_state *state = mapping ? mapping->state : NULL;
        if (!mapping || !state || !mapping->storage) {
            *result = GL_FALSE;
            return 1;
        }
        bool writable = mapping->range_mapping ?
            (mapping->access_flags & GL_MAP_WRITE_BIT) != 0 :
            mapping->access != GL_READ_ONLY;
        bool explicit_flush = mapping->range_mapping &&
            (mapping->access_flags & GL_MAP_FLUSH_EXPLICIT_BIT) != 0;
        bool upload_all = writable && !explicit_flush &&
            (mapping->range_mapping || state->flush_on_unmap);
        if (mapping->size && upload_all) {
            glBufferSubData(mapping->target,
                            (GLintptr)mapping->mapped_offset,
                            (GLsizeiptr)mapping->size,
                            (const void *)(uintptr_t)mapping->storage);
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glUnmapBuffer swap=%llu target=%04x "
                    "buffer=%u offset=%zu size=%zu access=%04x "
                    "range=%d uploaded_all=%d\n",
                    (unsigned long long)objc_bridge_swap_count,
                    mapping->target, buffer, mapping->mapped_offset,
                    mapping->size,
                    mapping->range_mapping ? mapping->access_flags :
                                             mapping->access,
                    mapping->range_mapping, upload_all);
        }
        deactivate_buffer_mapping(mapping);
        *result = GL_TRUE;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glFlushMappedBufferRangeAPPLE")) {
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_mapping *mapping =
            active_buffer_mapping(context, arguments[0], buffer);
        struct guest_buffer_state *state = mapping ? mapping->state : NULL;
        size_t offset = arguments[1];
        size_t size = arguments[2];
        bool valid = mapping && state && mapping->storage &&
            mapping->access != GL_READ_ONLY && offset <= mapping->size &&
            size <= mapping->size - offset;
        if (valid) {
            glBufferSubData(mapping->target, (GLintptr)offset,
                            (GLsizeiptr)size,
                            (const void *)(uintptr_t)(mapping->storage + offset));
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glFlushMappedBufferRangeAPPLE swap=%llu "
                    "target=%04x buffer=%u offset=%zu size=%zu valid=%d\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    buffer, offset, size, valid);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glFlushMappedBufferRange")) {
        CGLContextObj context = CGLGetCurrentContext();
        GLuint buffer = bound_buffer_for_target(arguments[0]);
        struct guest_buffer_mapping *mapping =
            active_buffer_mapping(context, arguments[0], buffer);
        int32_t signed_offset = (int32_t)arguments[1];
        int32_t signed_size = (int32_t)arguments[2];
        size_t offset = signed_offset >= 0 ? (size_t)signed_offset : SIZE_MAX;
        size_t size = signed_size >= 0 ? (size_t)signed_size : SIZE_MAX;
        bool valid = mapping && mapping->range_mapping && mapping->storage &&
            (mapping->access_flags & GL_MAP_WRITE_BIT) != 0 &&
            offset <= mapping->size && size <= mapping->size - offset;
        if (valid) {
            glBufferSubData(mapping->target,
                            (GLintptr)(mapping->mapped_offset + offset),
                            (GLsizeiptr)size,
                            (const void *)(uintptr_t)(mapping->storage + offset));
        }
        if (trace_gl_buffer_call()) {
            fprintf(stderr,
                    "compat32: glFlushMappedBufferRange swap=%llu "
                    "target=%04x buffer=%u relative_offset=%zu "
                    "absolute_offset=%zu size=%zu valid=%d\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    buffer, offset,
                    valid ? mapping->mapped_offset + offset : SIZE_MAX,
                    size, valid);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetBufferParameteriv") ||
        LP32_NAME_IS(import_name, import_length, "glGetBufferParameterivARB")) {
        glGetBufferParameteriv(arguments[0], arguments[1],
                               (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glVertexAttribPointer") ||
        LP32_NAME_IS(import_name, import_length, "glVertexAttribPointerARB")) {
        *result = fast_glVertexAttribPointer(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glEnableVertexAttribArray") ||
        LP32_NAME_IS(import_name, import_length, "glEnableVertexAttribArrayARB")) {
        *result = fast_glEnableVertexAttribArray(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDisableVertexAttribArray") ||
        LP32_NAME_IS(import_name, import_length, "glDisableVertexAttribArrayARB")) {
        *result = fast_glDisableVertexAttribArray(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glTexImage3D") ||
        LP32_NAME_IS(import_name, import_length, "glTexImage3DEXT")) {
        ++gl_frame_diagnostics.texture_uploads;
        note_texture_level_change(arguments[0]);
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glTexImage3D swap=%llu target=%04x level=%d "
                "internal=%04x size=%dx%dx%d border=%d format=%04x "
                "type=%04x pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], arguments[2],
                (GLsizei)arguments[3], (GLsizei)arguments[4],
                (GLsizei)arguments[5], (GLint)arguments[6], arguments[7],
                arguments[8], arguments[9]);
        }
        glTexImage3D(arguments[0], (GLint)arguments[1],
                     (GLint)arguments[2], (GLsizei)arguments[3],
                     (GLsizei)arguments[4], (GLsizei)arguments[5],
                     (GLint)arguments[6], arguments[7], arguments[8],
                     (const void *)(uintptr_t)arguments[9]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glTexSubImage3D") ||
        LP32_NAME_IS(import_name, import_length, "glTexSubImage3DEXT")) {
        ++gl_frame_diagnostics.texture_uploads;
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glTexSubImage3D swap=%llu target=%04x level=%d "
                "offset={%d,%d,%d} size=%dx%dx%d format=%04x type=%04x "
                "pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], (GLint)arguments[2],
                (GLint)arguments[3], (GLint)arguments[4],
                (GLsizei)arguments[5], (GLsizei)arguments[6],
                (GLsizei)arguments[7], arguments[8], arguments[9],
                arguments[10]);
        }
        glTexSubImage3D(
            arguments[0], (GLint)arguments[1], (GLint)arguments[2],
            (GLint)arguments[3], (GLint)arguments[4],
            (GLsizei)arguments[5], (GLsizei)arguments[6],
            (GLsizei)arguments[7], arguments[8], arguments[9],
            (const void *)(uintptr_t)arguments[10]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glCopyTexSubImage3D") ||
        LP32_NAME_IS(import_name, import_length, "glCopyTexSubImage3DEXT")) {
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glCopyTexSubImage3D swap=%llu target=%04x "
                "level=%d offset={%d,%d,%d} source={%d,%d} size=%dx%d\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], (GLint)arguments[2],
                (GLint)arguments[3], (GLint)arguments[4],
                (GLint)arguments[5], (GLint)arguments[6],
                (GLsizei)arguments[7], (GLsizei)arguments[8]);
        }
        glCopyTexSubImage3D(
            arguments[0], (GLint)arguments[1], (GLint)arguments[2],
            (GLint)arguments[3], (GLint)arguments[4],
            (GLint)arguments[5], (GLint)arguments[6],
            (GLsizei)arguments[7], (GLsizei)arguments[8]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glCompressedTexImage2D") ||
        LP32_NAME_IS(import_name, import_length, "glCompressedTexImage2DARB")) {
        ++gl_frame_diagnostics.texture_uploads;
        note_texture_level_change(arguments[0]);
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glCompressedTexImage2D swap=%llu target=%04x "
                "level=%d internal=%04x size=%dx%d border=%d bytes=%d "
                "pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], arguments[2], (GLsizei)arguments[3],
                (GLsizei)arguments[4], (GLint)arguments[5],
                (GLsizei)arguments[6], arguments[7]);
        }
        glCompressedTexImage2D(arguments[0], (GLint)arguments[1], arguments[2],
                               (GLsizei)arguments[3], (GLsizei)arguments[4],
                               (GLint)arguments[5], (GLsizei)arguments[6],
                               (const void *)(uintptr_t)arguments[7]);
        repair_bound_texture_mipmap_range(arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glCompressedTexSubImage2D") ||
        LP32_NAME_IS(import_name, import_length, "glCompressedTexSubImage2DARB")) {
        ++gl_frame_diagnostics.texture_uploads;
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glCompressedTexSubImage2D swap=%llu target=%04x "
                "level=%d offset={%d,%d} size=%dx%d format=%04x "
                "bytes=%d pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                (GLint)arguments[1], (GLint)arguments[2],
                (GLint)arguments[3], (GLsizei)arguments[4],
                (GLsizei)arguments[5], arguments[6],
                (GLsizei)arguments[7], arguments[8]);
        }
        glCompressedTexSubImage2D(
            arguments[0], (GLint)arguments[1], (GLint)arguments[2],
            (GLint)arguments[3], (GLsizei)arguments[4],
            (GLsizei)arguments[5], arguments[6], (GLsizei)arguments[7],
            (const void *)(uintptr_t)arguments[8]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDrawRangeElements") ||
        LP32_NAME_IS(import_name, import_length, "glDrawRangeElementsEXT")) {
        *result = fast_glDrawRangeElements(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenQueries") ||
        LP32_NAME_IS(import_name, import_length, "glGenQueriesARB")) {
        glGenQueries((GLsizei)arguments[0],
                     (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteQueries") ||
        LP32_NAME_IS(import_name, import_length, "glDeleteQueriesARB")) {
        glDeleteQueries((GLsizei)arguments[0],
                        (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glIsQuery") ||
        LP32_NAME_IS(import_name, import_length, "glIsQueryARB")) {
        *result = glIsQuery((GLuint)arguments[0]); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBeginQuery") ||
        LP32_NAME_IS(import_name, import_length, "glBeginQueryARB")) {
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glBeginQuery swap=%llu target=%04x id=%u\n",
                    (unsigned long long)objc_bridge_swap_count,
                    arguments[0], arguments[1]);
        }
        glBeginQuery(arguments[0], (GLuint)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glEndQuery") ||
        LP32_NAME_IS(import_name, import_length, "glEndQueryARB")) {
        if (trace_gl_render_call()) {
            fprintf(stderr, "compat32: glEndQuery swap=%llu target=%04x\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0]);
        }
        glEndQuery(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetQueryiv") ||
        LP32_NAME_IS(import_name, import_length, "glGetQueryivARB")) {
        glGetQueryiv(arguments[0], arguments[1],
                     (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetQueryObjectiv") ||
        LP32_NAME_IS(import_name, import_length, "glGetQueryObjectivARB")) {
        glGetQueryObjectiv((GLuint)arguments[0], arguments[1],
                           (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetQueryObjectuiv") ||
        LP32_NAME_IS(import_name, import_length, "glGetQueryObjectuivARB")) {
        glGetQueryObjectuiv((GLuint)arguments[0], arguments[1],
                            (GLuint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenFramebuffers") ||
        LP32_NAME_IS(import_name, import_length, "glGenFramebuffersEXT")) {
        glGenFramebuffers(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteFramebuffers") ||
        LP32_NAME_IS(import_name, import_length, "glDeleteFramebuffersEXT")) {
        glDeleteFramebuffers(arguments[0],
                             (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBindFramebuffer") ||
        LP32_NAME_IS(import_name, import_length, "glBindFramebufferEXT")) {
        ++gl_frame_diagnostics.framebuffer_binds;
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glBindFramebuffer swap=%llu target=%04x fbo=%u\n",
                    (unsigned long long)objc_bridge_swap_count,
                    arguments[0], arguments[1]);
        }
        glBindFramebuffer(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glCheckFramebufferStatus") ||
        LP32_NAME_IS(import_name, import_length, "glCheckFramebufferStatusEXT")) {
        *result = glCheckFramebufferStatus(arguments[0]);
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glCheckFramebufferStatus swap=%llu "
                    "target=%04x status=%04llx\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    (unsigned long long)*result);
        }
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glFramebufferTexture2D") ||
        LP32_NAME_IS(import_name, import_length, "glFramebufferTexture2DEXT")) {
        ++gl_frame_diagnostics.framebuffer_attachments;
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glFramebufferTexture2D swap=%llu target=%04x "
                    "attachment=%04x textarget=%04x texture=%u level=%d\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0],
                    arguments[1], arguments[2], arguments[3],
                    (GLint)arguments[4]);
        }
        glFramebufferTexture2D(arguments[0], arguments[1], arguments[2],
                               arguments[3], (GLint)arguments[4]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlitFramebuffer") ||
        LP32_NAME_IS(import_name, import_length, "glBlitFramebufferEXT")) {
        ++gl_frame_diagnostics.framebuffer_blits;
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glBlitFramebuffer swap=%llu "
                    "src={%d,%d,%d,%d} dst={%d,%d,%d,%d} "
                    "mask=%04x filter=%04x\n",
                    (unsigned long long)objc_bridge_swap_count,
                    (GLint)arguments[0], (GLint)arguments[1],
                    (GLint)arguments[2], (GLint)arguments[3],
                    (GLint)arguments[4], (GLint)arguments[5],
                    (GLint)arguments[6], (GLint)arguments[7],
                    arguments[8], arguments[9]);
        }
        glBlitFramebuffer((GLint)arguments[0], (GLint)arguments[1],
                          (GLint)arguments[2], (GLint)arguments[3],
                          (GLint)arguments[4], (GLint)arguments[5],
                          (GLint)arguments[6], (GLint)arguments[7],
                          arguments[8], arguments[9]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenRenderbuffers") ||
        LP32_NAME_IS(import_name, import_length, "glGenRenderbuffersEXT")) {
        glGenRenderbuffers(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteRenderbuffers") ||
        LP32_NAME_IS(import_name, import_length, "glDeleteRenderbuffersEXT")) {
        glDeleteRenderbuffers(arguments[0],
                              (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBindRenderbuffer") ||
        LP32_NAME_IS(import_name, import_length, "glBindRenderbufferEXT")) {
        glBindRenderbuffer(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glRenderbufferStorage") ||
        LP32_NAME_IS(import_name, import_length, "glRenderbufferStorageEXT")) {
        glRenderbufferStorage(arguments[0], arguments[1], arguments[2],
                              arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glFramebufferRenderbuffer") ||
        LP32_NAME_IS(import_name, import_length, "glFramebufferRenderbufferEXT")) {
        ++gl_frame_diagnostics.framebuffer_attachments;
        glFramebufferRenderbuffer(arguments[0], arguments[1], arguments[2],
                                  arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenerateMipmap") ||
        LP32_NAME_IS(import_name, import_length, "glGenerateMipmapEXT")) {
        note_texture_level_change(arguments[0]);
        glGenerateMipmap(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glEnableIndexedEXT")) {
        glEnableIndexedEXT(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDisableIndexedEXT")) {
        glDisableIndexedEXT(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glColorMaskIndexedEXT")) {
        glColorMaskIndexedEXT(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDrawBuffers") ||
        LP32_NAME_IS(import_name, import_length, "glDrawBuffersARB")) {
        const GLenum *buffers = (const void *)(uintptr_t)arguments[1];
        if (trace_gl_render_call()) {
            GLsizei count = (GLsizei)arguments[0];
            fprintf(stderr,
                    "compat32: glDrawBuffers swap=%llu count=%d "
                    "buffers={%04x,%04x,%04x,%04x}\n",
                    (unsigned long long)objc_bridge_swap_count, count,
                    count > 0 ? buffers[0] : 0, count > 1 ? buffers[1] : 0,
                    count > 2 ? buffers[2] : 0, count > 3 ? buffers[3] : 0);
        }
        glDrawBuffers(arguments[0], buffers);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGenProgramsARB")) {
        glGenProgramsARB(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glDeleteProgramsARB")) {
        forget_fragment_program_samplers(
            (GLsizei)arguments[0],
            (const GLuint *)(uintptr_t)arguments[1]);
        glDeleteProgramsARB(arguments[0],
                            (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBindProgramARB")) {
        *result = fast_glBindProgramARB(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramStringARB")) {
        GLsizei source_size = (GLsizei)arguments[2];
        const void *source = (const void *)(uintptr_t)arguments[3];
        maybe_dump_gl_program(arguments[0], source_size, source);
        char *plain_targets = NULL;
        if (arguments[0] == GL_FRAGMENT_PROGRAM_ARB && source_size > 0 &&
            arguments[1] == GL_PROGRAM_FORMAT_ASCII_ARB &&
            !getenv("LP32_KEEP_ARB_SHADOW_TARGETS")) {
            size_t plain_size = 0;
            size_t rewrites = 0;
            plain_targets = arb_program_plain_shadow_targets(
                source, (size_t)source_size, &plain_size, &rewrites);
            if (plain_targets && plain_size <= INT_MAX) {
                static unsigned int reports;
                if (reports < 12) {
                    gl_trace_printf(
                        "compat32: rewrote %zu SHADOW texture target(s) to "
                        "plain targets in fragment program %u\n",
                        rewrites, trace_fragment_program);
                    ++reports;
                }
                source = plain_targets;
                source_size = (GLsizei)plain_size;
            } else {
                free(plain_targets);
                plain_targets = NULL;
            }
        }
        if (arguments[0] == GL_FRAGMENT_PROGRAM_ARB && source_size > 0) {
            GLint program = (GLint)trace_fragment_program;
            if (program > 0) {
                remember_fragment_program_samplers(
                    (GLuint)program, source, (size_t)source_size);
            }
        }
        size_t guarded_size = 0;
        size_t rsq_guard_count = 0;
        size_t rcp_guard_count = 0;
        char *guarded = NULL;
        if (guard_undefined_arb_math() && source_size > 0 &&
            arguments[1] == GL_PROGRAM_FORMAT_ASCII_ARB) {
            guarded = arb_program_guard_undefined_math(
                source, (size_t)source_size, &guarded_size,
                &rsq_guard_count, &rcp_guard_count);
        }
        if (guarded && guarded_size <= INT_MAX) {
            static unsigned int reports;
            if (reports < 12) {
                gl_trace_printf(
                    "compat32: guarded ARB math rsq=%zu rcp=%zu "
                    "target=%04x originalBytes=%d guardedBytes=%zu\n",
                    rsq_guard_count, rcp_guard_count,
                    arguments[0], source_size, guarded_size);
                ++reports;
            }
            glProgramStringARB(arguments[0], arguments[1],
                               (GLsizei)guarded_size, guarded);
        } else {
            glProgramStringARB(arguments[0], arguments[1], source_size,
                               source);
        }
        free(guarded);
        report_arb_program_load_failure(arguments[0], source, source_size);
        free(plain_targets);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramLocalParameter4fvARB")) {
        *result = fast_glProgramLocalParameter4fvARB(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramLocalParameters4fvEXT")) {
        *result = fast_glProgramLocalParameters4fvEXT(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramEnvParameter4fvARB")) {
        *result = fast_glProgramEnvParameter4fvARB(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glProgramEnvParameters4fvEXT")) {
        *result = fast_glProgramEnvParameters4fvEXT(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetProgramivARB")) {
        glGetProgramivARB(arguments[0], arguments[1],
                          (GLint *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glGetProgramStringARB")) {
        glGetProgramStringARB(arguments[0], arguments[1],
                              (void *)(uintptr_t)arguments[2]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glIsProgramARB")) {
        *result = glIsProgramARB(arguments[0]); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetStringi")) {
        extern const GLubyte *glGetStringi(GLenum, GLuint);
        const GLubyte *value = glGetStringi(arguments[0], arguments[1]);
        *result = value ? intern_guest_cstring((const char *)value) : 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetString")) {
        *result = guest_gl_string(arguments[0]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glEnable")) {
        *result = fast_glEnable(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDisable")) {
        *result = fast_glDisable(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClear")) {
        ++gl_frame_diagnostics.clear_calls;
        if (trace_gl_render_call()) {
            GLint framebuffer = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
            fprintf(stderr,
                    "compat32: glClear swap=%llu mask=%04x fbo=%d\n",
                    (unsigned long long)objc_bridge_swap_count,
                    arguments[0], framebuffer);
        }
        glClear(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlendEquation") ||
        LP32_NAME_IS(import_name, import_length, "glBlendEquationEXT")) {
        *result = fast_glBlendEquation(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlendFuncSeparate") ||
        LP32_NAME_IS(import_name, import_length, "glBlendFuncSeparateEXT")) {
        *result = fast_glBlendFuncSeparate(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlendEquationSeparate") ||
        LP32_NAME_IS(import_name, import_length, "glBlendEquationSeparateEXT")) {
        glBlendEquationSeparate(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glBlendColor") ||
        LP32_NAME_IS(import_name, import_length, "glBlendColorEXT")) {
        glBlendColor(guest_float_argument(arguments[0]),
                     guest_float_argument(arguments[1]),
                     guest_float_argument(arguments[2]),
                     guest_float_argument(arguments[3]));
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glStencilFuncSeparate")) {
        glStencilFuncSeparate(arguments[0], arguments[1], arguments[2],
                              arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glStencilMaskSeparate")) {
        glStencilMaskSeparate(arguments[0], arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "glStencilOpSeparate") ||
        LP32_NAME_IS(import_name, import_length, "glStencilOpSeparateATI")) {
        glStencilOpSeparate(arguments[0], arguments[1], arguments[2],
                            arguments[3]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClearColor")) {
        const float *v = (const void *)arguments;
        glClearColor(v[0], v[1], v[2], v[3]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClearDepth")) {
        double value; memcpy(&value, arguments, sizeof(value));
        glClearDepth(value); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glClearStencil")) { glClearStencil(arguments[0]); *result = 0; return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glAlphaFunc")) {
        *result = fast_glAlphaFunc(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glBindTexture")) {
        *result = fast_glBindTexture(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glBlendFunc")) { *result = fast_glBlendFunc(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glColorMask")) { *result = fast_glColorMask(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glCullFace")) { *result = fast_glCullFace(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glDeleteTextures")) {
        if (trace_gl_render_call()) {
            const GLuint *textures = (const void *)(uintptr_t)arguments[1];
            gl_trace_printf(
                "compat32: glDeleteTextures swap=%llu count=%u first=%u\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                arguments[0] && textures ? textures[0] : 0);
        }
        forget_mipmap_repairs(
            (GLsizei)arguments[0],
            (const GLuint *)(uintptr_t)arguments[1]);
        glDeleteTextures(arguments[0],
                         (const GLuint *)(uintptr_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDepthFunc")) { *result = fast_glDepthFunc(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glDepthMask")) { *result = fast_glDepthMask(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glDrawArrays")) {
        *result = fast_glDrawArrays(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glDrawBuffer")) {
        if (trace_gl_render_call()) {
            fprintf(stderr, "compat32: glDrawBuffer swap=%llu buffer=%04x\n",
                    (unsigned long long)objc_bridge_swap_count, arguments[0]);
        }
        glDrawBuffer(arguments[0]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glFlush")) { glFlush(); *result = 0; return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glFrontFace")) { *result = fast_glFrontFace(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glGenTextures")) {
        glGenTextures(arguments[0], (GLuint *)(uintptr_t)arguments[1]);
        if (trace_gl_render_call()) {
            const GLuint *textures = (const void *)(uintptr_t)arguments[1];
            gl_trace_printf(
                "compat32: glGenTextures swap=%llu count=%u first=%u\n",
                (unsigned long long)objc_bridge_swap_count, arguments[0],
                arguments[0] && textures ? textures[0] : 0);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glGetFloatv")) { *result = fast_glGetFloatv(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glGetIntegerv")) { *result = fast_glGetIntegerv(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glPolygonOffset")) {
        *result = fast_glPolygonOffset(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glReadBuffer")) { glReadBuffer(arguments[0]); *result = 0; return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glScissor")) { *result = fast_glScissor(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glStencilFunc")) { *result = fast_glStencilFunc(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glStencilMask")) { *result = fast_glStencilMask(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glStencilOp")) { *result = fast_glStencilOp(arguments, 0); return 1; }
    if (LP32_NAME_IS(import_name, import_length, "_glTexEnvf")) {
        *result = fast_glTexEnvf(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexImage2D")) {
        ++gl_frame_diagnostics.texture_uploads;
        note_texture_level_change(arguments[0]);
        if (trace_gl_render_call()) {
            gl_trace_printf(
                "compat32: glTexImage2D swap=%llu target=%04x level=%d "
                "internal=%04x size=%dx%d border=%d format=%04x "
                "type=%04x pixels=%08x\n",
                (unsigned long long)objc_bridge_swap_count,
                arguments[0], (GLint)arguments[1], arguments[2],
                (GLsizei)arguments[3], (GLsizei)arguments[4],
                (GLint)arguments[5], arguments[6], arguments[7],
                arguments[8]);
        }
        glTexImage2D(arguments[0], arguments[1], arguments[2], arguments[3],
                     arguments[4], arguments[5], arguments[6], arguments[7],
                     (const void *)(uintptr_t)arguments[8]);
        repair_bound_texture_mipmap_range(arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexParameterf")) {
        float value; memcpy(&value, arguments + 2, sizeof(value));
        set_texture_parameter_f(arguments[0], arguments[1], value);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexParameterfv")) {
        const GLfloat *values = (const GLfloat *)(uintptr_t)arguments[2];
        if (arguments[1] == GL_TEXTURE_MIN_FILTER ||
            arguments[1] == GL_TEXTURE_BASE_LEVEL ||
            arguments[1] == GL_TEXTURE_MAX_LEVEL) {
            set_texture_parameter_f(arguments[0], arguments[1], values[0]);
        } else {
            glTexParameterfv(arguments[0], arguments[1], values);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexParameteri")) {
        *result = fast_glTexParameteri(arguments, 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glViewport")) {
        if (trace_gl_render_call()) {
            fprintf(stderr,
                    "compat32: glViewport swap=%llu value={%d,%d,%d,%d}\n",
                    (unsigned long long)objc_bridge_swap_count,
                    (GLint)arguments[0], (GLint)arguments[1],
                    (GLsizei)arguments[2], (GLsizei)arguments[3]);
        }
        glViewport(arguments[0], arguments[1], arguments[2], arguments[3]);
        *result = 0; return 1;
    }
    /* Fixed-function immediate mode used by the Clone Wars Feral layer for
       its loading/letterbox quads. */
    if (LP32_NAME_IS(import_name, import_length, "_glPushAttrib")) {
        glPushAttrib(arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glPopAttrib")) {
        glPopAttrib();
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glBegin")) {
        glBegin(arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glEnd")) {
        glEnd();
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glColor4f")) {
        glColor4f(guest_float_argument(arguments[0]),
                  guest_float_argument(arguments[1]),
                  guest_float_argument(arguments[2]),
                  guest_float_argument(arguments[3]));
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glVertex3f")) {
        glVertex3f(guest_float_argument(arguments[0]),
                   guest_float_argument(arguments[1]),
                   guest_float_argument(arguments[2]));
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glTexCoord2fv")) {
        glTexCoord2fv((const GLfloat *)(uintptr_t)arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glLoadIdentity")) {
        glLoadIdentity();
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glMatrixMode")) {
        glMatrixMode(arguments[0]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glCopyTexSubImage2D")) {
        glCopyTexSubImage2D(arguments[0], (GLint)arguments[1],
                            (GLint)arguments[2], (GLint)arguments[3],
                            (GLint)arguments[4], (GLint)arguments[5],
                            (GLsizei)arguments[6], (GLsizei)arguments[7]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_glFinish")) {
        glFinish();
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_IORegistryEntrySearchCFProperty")) {
        NSString *key = object_for_argument(arguments[2]);
        uint32_t identifier = 0;
        if ([key isEqualToString:@"vendor-id"]) identifier = UINT32_C(0x106b);
        else if ([key isEqualToString:@"device-id"]) identifier = 1;
        NSData *data = [NSData dataWithBytes:&identifier length:sizeof(identifier)];
        *result = proxy_for_object(data);
        return 1;
    }

    /* The original updater would replace the compatibility executable with
     * an unsupported i386 download. Keep automatic update checks disabled. */
    if (LP32_NAME_IS(import_name, import_length, "_SUSparkleCallNSApplicationLoad")) {
        *result = NSApplicationLoad(); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_SUSparkleWillCheckForUpdates") ||
        LP32_NAME_IS(import_name, import_length, "_SUSparkleShouldCheckForUpdates") ||
        LP32_NAME_IS(import_name, import_length, "_SUSparkleAppShouldQuit") ||
        LP32_NAME_IS(import_name, import_length, "_SUSparkleInitializeForCarbon")) {
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGAssociateMouseAndMouseCursorPosition")) {
        *result = (background_test_mode() || (lp32_suppress_background_input() && !arguments[0])) ? 0 : CGAssociateMouseAndMouseCursorPosition(arguments[0] != 0); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGEventSourceKeyState")) {
        *result = (background_test_mode() || getenv("LP32_TEST_KEY_FIFO")) ? objc_bridge32_test_key_down(arguments[1]) :
            lp32_suppress_background_input() ? false : CGEventSourceKeyState((CGEventSourceStateID)arguments[0], (CGKeyCode)arguments[1]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGWarpMouseCursorPosition")) {
        const float *xy = (const void *)arguments;
        *result = (background_test_mode() || lp32_suppress_background_input()) ? kCGErrorSuccess : CGWarpMouseCursorPosition(CGPointMake(xy[0], xy[1]));
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSPointInRect")) {
        const float *f = (const void *)arguments;
        *result = NSPointInRect(NSMakePoint(f[0], f[1]), NSMakeRect(f[2], f[3], f[4], f[5]));
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSDisableScreenUpdates") ||
        LP32_NAME_IS(import_name, import_length, "_NSEnableScreenUpdates")) {
        if (!background_test_mode()) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            if (strstr(import_name, "Disable")) NSDisableScreenUpdates();
            else NSEnableScreenUpdates();
#pragma clang diagnostic pop
        }
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGDisplayRestoreColorSyncSettings")) {
        if(!getenv("LP32_BACKGROUND_TEST"))CGDisplayRestoreColorSyncSettings();*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGGetDisplayTransferByTable")) {
        *result=(uint32_t)CGGetDisplayTransferByTable(arguments[0],arguments[1],(void *)(uintptr_t)arguments[2],(void *)(uintptr_t)arguments[3],(void *)(uintptr_t)arguments[4],(void *)(uintptr_t)arguments[5]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGGetDisplayTransferByFormula")) {
        *result=(uint32_t)CGGetDisplayTransferByFormula(arguments[0],(void *)(uintptr_t)arguments[1],(void *)(uintptr_t)arguments[2],(void *)(uintptr_t)arguments[3],(void *)(uintptr_t)arguments[4],(void *)(uintptr_t)arguments[5],(void *)(uintptr_t)arguments[6],(void *)(uintptr_t)arguments[7],(void *)(uintptr_t)arguments[8],(void *)(uintptr_t)arguments[9]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGSetDisplayTransferByFormula")) {
        float f[9];memcpy(f,arguments+1,sizeof(f));
        *result=getenv("LP32_BACKGROUND_TEST")?0:(uint32_t)CGSetDisplayTransferByFormula(arguments[0],f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CGSetDisplayTransferByTable")) {
        *result=getenv("LP32_BACKGROUND_TEST")?0:(uint32_t)CGSetDisplayTransferByTable(arguments[0],arguments[1],(void *)(uintptr_t)arguments[2],(void *)(uintptr_t)arguments[3],(void *)(uintptr_t)arguments[4]);return 1;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (LP32_NAME_IS(import_name, import_length, "_CFXMLTreeCreateFromData")) {
        CFXMLTreeRef tree = CFXMLTreeCreateFromData(NULL, (CFDataRef)object_for_argument(arguments[1]),
            (CFURLRef)object_for_argument(arguments[2]), arguments[3], (int32_t)arguments[4]);
        *result = proxy_for_object((id)tree); if (tree) CFRelease(tree); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFXMLTreeGetNode")) {
        *result = proxy_for_object((id)CFXMLTreeGetNode((CFXMLTreeRef)object_for_argument(arguments[0]))); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFXMLNodeGetTypeCode")) {
        *result = CFXMLNodeGetTypeCode((CFXMLNodeRef)object_for_argument(arguments[0])); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFXMLNodeGetString")) {
        *result = proxy_for_object((id)CFXMLNodeGetString((CFXMLNodeRef)object_for_argument(arguments[0]))); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFXMLNodeGetInfoPtr")) {
        id object = object_for_argument(arguments[0]);
        CFXMLNodeRef node = (CFXMLNodeRef)object;
        const void *info = CFXMLNodeGetInfoPtr(node);
        if (!info) { *result = 0; return 1; }
        uint32_t words[5] = {0};
        switch (CFXMLNodeGetTypeCode(node)) {
            case kCFXMLNodeTypeElement: {
                const CFXMLElementInfo *element = info;
                words[0] = proxy_for_object((id)element->attributes);
                words[1] = proxy_for_object((id)element->attributeOrder);
                words[2] = element->isEmpty; break;
            }
            case kCFXMLNodeTypeDocument: {
                const CFXMLDocumentInfo *document = info;
                words[0] = proxy_for_object((id)document->sourceURL); words[1] = document->encoding; break;
            }
            case kCFXMLNodeTypeProcessingInstruction:
                words[0] = proxy_for_object((id)((const CFXMLProcessingInstructionInfo *)info)->dataString); break;
            case kCFXMLNodeTypeEntityReference:
                words[0] = (uint32_t)((const CFXMLEntityReferenceInfo *)info)->entityType; break;
            default: return 0;
        }
        static char info_key;
        LP32DataBuffer *buffer = objc_getAssociatedObject(object, &info_key);
        if (!buffer) {
            buffer = [[[LP32DataBuffer alloc] init] autorelease]; buffer->guest = compat_runtime32_allocate(sizeof(words), 1);
            objc_setAssociatedObject(object, &info_key, buffer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        if (buffer->guest) memcpy((void *)(uintptr_t)buffer->guest, words, sizeof(words));
        *result = buffer->guest; return 1;
    }
#pragma clang diagnostic pop
    if (LP32_NAME_IS(import_name, import_length, "_CFTreeGetChildCount")) {
        *result = CFTreeGetChildCount((CFTreeRef)object_for_argument(arguments[0])); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFTreeGetChildAtIndex")) {
        *result = proxy_for_object((id)CFTreeGetChildAtIndex((CFTreeRef)object_for_argument(arguments[0]), (int32_t)arguments[1])); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFTreeApplyFunctionToChildren")) {
        CFTreeRef tree = (CFTreeRef)object_for_argument(arguments[0]);
        CFIndex count = CFTreeGetChildCount(tree);
        for (CFIndex i = 0; i < count; ++i) {
            uint32_t args[] = {proxy_for_object((id)CFTreeGetChildAtIndex(tree, i)), arguments[2]};
            compat_runtime32_call(arguments[1], args, 2);
        }
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CFErrorCopyFailureReason") || LP32_NAME_IS(import_name,import_length,"_CFErrorCopyDescription") ||
        LP32_NAME_IS(import_name,import_length,"_CFErrorCopyRecoverySuggestion") || LP32_NAME_IS(import_name,import_length,"_CFErrorCopyUserInfo")) {
        CFErrorRef error=(CFErrorRef)object_for_argument(arguments[0]);CFTypeRef value;
        if(LP32_NAME_IS(import_name,import_length,"_CFErrorCopyFailureReason"))value=CFErrorCopyFailureReason(error);
        else if(LP32_NAME_IS(import_name,import_length,"_CFErrorCopyDescription"))value=CFErrorCopyDescription(error);
        else if(LP32_NAME_IS(import_name,import_length,"_CFErrorCopyRecoverySuggestion"))value=CFErrorCopyRecoverySuggestion(error);
        else value=CFErrorCopyUserInfo(error);
        *result=proxy_for_object((id)value);if(value)CFRelease(value);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CFErrorGetCode")) {*result=(uint32_t)CFErrorGetCode((CFErrorRef)object_for_argument(arguments[0]));return 1;}
    if (LP32_NAME_IS(import_name,import_length,"_CFErrorGetDomain")) {*result=proxy_for_object((id)CFErrorGetDomain((CFErrorRef)object_for_argument(arguments[0])));return 1;}
    if (LP32_NAME_IS(import_name,import_length,"_CFURLCreateBookmarkDataFromFile") || LP32_NAME_IS(import_name,import_length,"_CFURLCreateBookmarkData") ||
        LP32_NAME_IS(import_name,import_length,"_CFURLCreateByResolvingBookmarkData") || LP32_NAME_IS(import_name,import_length,"_CFURLWriteBookmarkDataToFile")) {
        CFErrorRef error=NULL;CFTypeRef value=NULL;uint32_t error_address;
        bool writing=LP32_NAME_IS(import_name,import_length,"_CFURLWriteBookmarkDataToFile");
        if(LP32_NAME_IS(import_name,import_length,"_CFURLCreateBookmarkDataFromFile")){
            value=CFURLCreateBookmarkDataFromFile(NULL,(CFURLRef)object_for_argument(arguments[1]),&error);error_address=arguments[2];
        } else if(LP32_NAME_IS(import_name,import_length,"_CFURLCreateBookmarkData")){
            value=CFURLCreateBookmarkData(NULL,(CFURLRef)object_for_argument(arguments[1]),arguments[2],(CFArrayRef)object_for_argument(arguments[3]),(CFURLRef)object_for_argument(arguments[4]),&error);error_address=arguments[5];
        } else if(writing){
            *result=CFURLWriteBookmarkDataToFile((CFDataRef)object_for_argument(arguments[0]),(CFURLRef)object_for_argument(arguments[1]),arguments[2],&error);error_address=arguments[3];
        } else {
            value=CFURLCreateByResolvingBookmarkData(NULL,(CFDataRef)object_for_argument(arguments[1]),arguments[2],(CFURLRef)object_for_argument(arguments[3]),(CFArrayRef)object_for_argument(arguments[4]),(void *)(uintptr_t)arguments[5],&error);error_address=arguments[6];
        }
        if(!writing)*result=proxy_for_object((id)value);
        if(error_address)*(uint32_t *)(uintptr_t)error_address=proxy_for_object((id)error);
        if(error)CFRelease(error);if(value)CFRelease(value);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CFURLCreateCopyAppendingPathExtension") || LP32_NAME_IS(import_name,import_length,"_CFURLCreateCopyDeletingPathExtension")) {
        CFURLRef value=LP32_NAME_IS(import_name,import_length,"_CFURLCreateCopyAppendingPathExtension")?
            CFURLCreateCopyAppendingPathExtension(NULL,(CFURLRef)object_for_argument(arguments[1]),(CFStringRef)object_for_argument(arguments[2])):
            CFURLCreateCopyDeletingPathExtension(NULL,(CFURLRef)object_for_argument(arguments[1]));
        *result=proxy_for_object((id)value);if(value)CFRelease(value);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFPropertyListCreateFromXMLData")) {
        CFStringRef error=NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CFPropertyListRef value=CFPropertyListCreateFromXMLData(NULL,(CFDataRef)object_for_argument(arguments[1]),arguments[2],&error);
#pragma clang diagnostic pop
        if(getenv("LP32_TRACE_HID")){
            static unsigned traced_plists;
            if(!value || traced_plists++<5){
                CFDataRef data=(CFDataRef)object_for_argument(arguments[1]);
                fprintf(stderr,"compat32: plist parse bytes=%ld dictionary=%ld error=%s\n",
                    data?CFDataGetLength(data):-1L,
                    value && CFGetTypeID(value)==CFDictionaryGetTypeID()?CFDictionaryGetCount(value):-1L,
                    error?[(NSString *)error UTF8String]:"(none)");
            }
        }
        *result=proxy_for_object((id)value);
        if(arguments[3])*(uint32_t *)(uintptr_t)arguments[3]=proxy_for_object((id)error);
        if(error)CFRelease(error);if(value)CFRelease(value);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFPreferencesCopyAppValue")) {
        CFPropertyListRef value=CFPreferencesCopyAppValue((CFStringRef)object_for_argument(arguments[0]),(CFStringRef)object_for_argument(arguments[1]));
        *result=proxy_for_object((id)value);if(value)CFRelease(value);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFPreferencesGetAppBooleanValue")) {
        *result=CFPreferencesGetAppBooleanValue((CFStringRef)object_for_argument(arguments[0]),(CFStringRef)object_for_argument(arguments[1]),(void *)(uintptr_t)arguments[2]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFPreferencesCopyValue")) {
        CFPropertyListRef value = CFPreferencesCopyValue((CFStringRef)object_for_argument(arguments[0]),
            (CFStringRef)object_for_argument(arguments[1]), (CFStringRef)object_for_argument(arguments[2]),
            (CFStringRef)object_for_argument(arguments[3]));
        *result = proxy_for_object((id)value); if (value) CFRelease(value); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFPreferencesSetAppValue")) {
        CFPreferencesSetAppValue((CFStringRef)object_for_argument(arguments[0]),
            (CFPropertyListRef)object_for_argument(arguments[1]),(CFStringRef)object_for_argument(arguments[2]));
        *result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFPreferencesAppSynchronize")) {
        *result=CFPreferencesAppSynchronize((CFStringRef)object_for_argument(arguments[0]));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFPreferencesSynchronize")) {
        *result=CFPreferencesSynchronize((CFStringRef)object_for_argument(arguments[0]),
            (CFStringRef)object_for_argument(arguments[1]),(CFStringRef)object_for_argument(arguments[2]));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFAllocatorCreate")) {
        struct allocator_context32 *guest = (void *)(uintptr_t)arguments[1];
        if (!guest || guest->version) { *result = 0; return 1; }
        CFAllocatorContext context = {0, guest, allocator_retain32, allocator_release32,
            allocator_describe32, allocator_allocate32, allocator_reallocate32,
            allocator_deallocate32, allocator_preferred32};
        CFAllocatorRef allocator = CFAllocatorCreate(NULL, &context);
        *result = proxy_for_object((id)allocator);
        if (allocator) CFRelease(allocator);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_SecRequirementCreateWithString") ||
        LP32_NAME_IS(import_name, import_length, "_SecRequirementCreateWithData")) {
        SecRequirementRef requirement = NULL;
        OSStatus status = LP32_NAME_IS(import_name, import_length, "_SecRequirementCreateWithString") ?
            SecRequirementCreateWithString((CFStringRef)object_for_argument(arguments[0]), arguments[1], &requirement) :
            SecRequirementCreateWithData((CFDataRef)object_for_argument(arguments[0]), arguments[1], &requirement);
        if (arguments[2]) *(uint32_t *)(uintptr_t)arguments[2] = proxy_for_object((id)requirement);
        if (requirement) CFRelease(requirement);
        *result = (uint32_t)status; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_SecCodeCopySelf")) {
        SecCodeRef code = NULL;
        OSStatus status = SecCodeCopySelf(arguments[0], &code);
        if (arguments[1]) *(uint32_t *)(uintptr_t)arguments[1] = proxy_for_object((id)code);
        if (code) CFRelease(code);
        *result = (uint32_t)status; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_SecCodeCheckValidity")) {
        *result = (uint32_t)SecCodeCheckValidity((SecCodeRef)object_for_argument(arguments[0]),
            arguments[1], (SecRequirementRef)object_for_argument(arguments[2]));
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFAllocatorGetDefault")) {
        *result = proxy_for_object((id)CFAllocatorGetDefault());
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_CFBundleGetMainBundle")) {
        *result = proxy_for_object(legacy_game_bundle());
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleGetInfoDictionary")) {
        NSBundle *bundle=object_for_argument(arguments[0]);
        *result=proxy_for_object([bundle infoDictionary]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyAuxiliaryExecutableURL")) {
        NSBundle *bundle=object_for_argument(arguments[0]);
        *result=proxy_for_object([bundle URLForAuxiliaryExecutable:object_for_argument(arguments[1])]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyBundleLocalizations")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        *result = proxy_for_object([bundle localizations]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyResourceURLsOfType")) {
        NSBundle *bundle=object_for_argument(arguments[0]);
        NSString *extension=object_for_argument(arguments[1]);
        NSString *subdirectory=object_for_argument(arguments[2]);
        NSArray *urls=[bundle URLsForResourcesWithExtension:extension subdirectory:subdirectory];
        if(getenv("LP32_TRACE_HID"))fprintf(stderr,"compat32: CFBundle resources extension=%s directory=%s count=%lu\n",
            extension?[extension UTF8String]:"(null)",subdirectory?[subdirectory UTF8String]:"(null)",(unsigned long)[urls count]);
        *result=proxy_for_object(urls);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleLoadExecutable")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        *result = [bundle load]; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyPreferredLocalizationsFromArray")) {
        CFArrayRef preferred = CFBundleCopyPreferredLocalizationsFromArray((CFArrayRef)object_for_argument(arguments[0]));
        *result = proxy_for_object((id)preferred);
        if (preferred) CFRelease(preferred);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyExecutableURL")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        *result = proxy_for_object([bundle executableURL]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyResourcesDirectoryURL")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        *result = proxy_for_object([bundle resourceURL]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyBuiltInPlugInsURL")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        *result = proxy_for_object([bundle builtInPlugInsURL]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCreate")) {
        NSURL *url = object_for_argument(arguments[1]);
        NSBundle *bundle = url ? [NSBundle bundleWithURL:url] : nil;
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: CFBundleCreate %s -> %s\n",
                    [[url path] UTF8String], bundle ? "ok" : "nil");
        }
        *result = proxy_for_object(bundle);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleGetFunctionPointerForName")) {
        /* Used to look up OpenGL entry points through the framework bundle.
           Hand back a guest thunk that dispatches through the import chains,
           exactly like dlsym does. */
        NSString *name = object_for_argument(arguments[1]);
        const char *symbol = [name UTF8String];
        uint32_t thunk = 0;
        if (symbol && strcmp(symbol, "MainEntry") == 0) {
            /* Clone Wars' main loads Resources/Content.loader, Feral's
               pre-game launcher (a 32-bit Mach-O bundle built on QuickTime,
               AGL, OpenPlay and Sparkle), and only continues into the game
               when its MainEntry returns 1.  The launcher's settings UI has
               no modern counterpart here; the game reads its configuration
               itself, so the bridge answers for the launcher. */
            thunk = compat_runtime32_guest_callback(kFeralMainEntryCallbackName);
        } else if (symbol && (carbon_bridge32_has_symbol(symbol) || dlsym(RTLD_DEFAULT, symbol))) {
            thunk = compat_runtime32_guest_callback(symbol);
        }
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr, "compat32: CFBundleGetFunctionPointerForName %s -> %#x\n",
                    symbol ? symbol : "(null)",thunk);
        }
        *result = thunk;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyLocalizedString")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        NSString *key = object_for_argument(arguments[1]);
        NSString *value = [bundle localizedStringForKey:key value:object_for_argument(arguments[2]) table:object_for_argument(arguments[3])];
        if (getenv("LP32_TRACE_LOCALIZATION")) fprintf(stderr,"compat32: localized bundle=%s key=%s table=%s result=%s\n",bundle.bundlePath.UTF8String,key.UTF8String,[(NSString *)object_for_argument(arguments[3]) UTF8String],value.UTF8String);
        *result = proxy_for_object(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSHomeDirectory")) {
        const char *test_home = getenv("LP32_TEST_HOME_DIR");
        *result = proxy_for_object(test_home && test_home[0] ?
            [NSString stringWithUTF8String:test_home] : NSHomeDirectory());
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateFromFSRef")) {
        UInt8 path[PATH_MAX];
        int16_t (*makePath)(const void *,UInt8 *,uint32_t)=dlsym(RTLD_DEFAULT,"FSRefMakePath");
        *result=makePath && !makePath((const void *)(uintptr_t)arguments[1],path,sizeof(path)) ?
            proxy_for_object([NSURL fileURLWithPath:[NSString stringWithUTF8String:(const char *)path]]) : 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLGetFSRef")) {
        const char *path=[[object_for_argument(arguments[0]) path] fileSystemRepresentation];
        int16_t (*makeRef)(const UInt8 *,void *,void *)=dlsym(RTLD_DEFAULT,"FSPathMakeRef");
        *result=path && makeRef && !makeRef((const UInt8 *)path,(void *)(uintptr_t)arguments[1],NULL);
        if(getenv("LP32_TRACE_HID") && path && strstr(path,"InputDevices"))
            fprintf(stderr,"compat32: CFURLGetFSRef path=%s result=%llu\n",path,(unsigned long long)*result);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateWithFileSystemPath") ||
        LP32_NAME_IS(import_name, import_length, "_CFURLCreateWithFileSystemPathRelativeToBase")) {
        CFURLRef base=LP32_NAME_IS(import_name,import_length,"_CFURLCreateWithFileSystemPathRelativeToBase")?
            (CFURLRef)object_for_argument(arguments[4]):NULL;
        CFURLRef url=CFURLCreateWithFileSystemPathRelativeToBase(NULL,
            (CFStringRef)object_for_argument(arguments[1]),arguments[2],arguments[3]!=0,base);
        *result=proxy_for_object((id)url);if(url)CFRelease(url);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateFromFileSystemRepresentation")) {
        CFURLRef url=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)(uintptr_t)arguments[1],
            (int32_t)arguments[2],arguments[3]!=0);*result=proxy_for_object((id)url);if(url)CFRelease(url);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCopyAbsoluteURL")) {
        CFURLRef url=CFURLCopyAbsoluteURL((CFURLRef)object_for_argument(arguments[0]));
        *result=proxy_for_object((id)url);if(url)CFRelease(url);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateWithString")) {
        CFURLRef url=CFURLCreateWithString(NULL,(CFStringRef)object_for_argument(arguments[1]),
            (CFURLRef)object_for_argument(arguments[2]));
        *result=proxy_for_object((id)url);if(url)CFRelease(url);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCopyHostName")) {
        CFStringRef host=CFURLCopyHostName((CFURLRef)object_for_argument(arguments[0]));
        *result=proxy_for_object((id)host);if(host)CFRelease(host);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCopyLastPathComponent")) {
        *result=proxy_for_object([object_for_argument(arguments[0]) lastPathComponent]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateCopyAppendingPathComponent")) {
        CFURLRef url=CFURLCreateCopyAppendingPathComponent(NULL,(CFURLRef)object_for_argument(arguments[1]),
            (CFStringRef)object_for_argument(arguments[2]),arguments[3]!=0);
        *result=proxy_for_object((id)url);if(url)CFRelease(url);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateCopyDeletingLastPathComponent")) {
        CFURLRef url=CFURLCreateCopyDeletingLastPathComponent(NULL,(CFURLRef)object_for_argument(arguments[1]));
        *result=proxy_for_object((id)url);if(url)CFRelease(url);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLHasDirectoryPath")) {
        *result=CFURLHasDirectoryPath((CFURLRef)object_for_argument(arguments[0]));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyBundleURL")) {
        *result=proxy_for_object([object_for_argument(arguments[0]) bundleURL]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleGetValueForInfoDictionaryKey")) {
        *result=proxy_for_object([object_for_argument(arguments[0]) objectForInfoDictionaryKey:object_for_argument(arguments[1])]);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCopyFileSystemPath")) {
        NSURL *url = object_for_argument(arguments[0]);
        CFStringRef path = CFURLCopyFileSystemPath((CFURLRef)url, arguments[1]);
        *result = proxy_for_object((id)path);
        if (path) CFRelease(path);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetCharacterAtIndex")) {
        NSString *string = object_for_argument(arguments[0]);
        *result = [string characterAtIndex:arguments[1]];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyResourceURL")) {
        NSBundle *bundle = object_for_argument(arguments[0]);
        NSString *name = object_for_argument(arguments[1]);
        NSString *extension = object_for_argument(arguments[2]);
        NSString *subdirectory = object_for_argument(arguments[3]);
        NSURL *url=[bundle URLForResource:name withExtension:extension subdirectory:subdirectory];
        if(getenv("LP32_TRACE_HID") && (strstr([name UTF8String]?:"","Input") || strstr([subdirectory UTF8String]?:"","Input")))
            fprintf(stderr,"compat32: CFBundle resource name=%s extension=%s directory=%s path=%s\n",
                [name UTF8String]?:"(null)",[extension UTF8String]?:"(null)",
                [subdirectory UTF8String]?:"(null)",[[url path] UTF8String]?:"(null)");
        *result = proxy_for_object(url);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLGetFileSystemRepresentation")) {
        NSURL *url = object_for_argument(arguments[0]);
        char *buffer = (void *)(uintptr_t)arguments[2];
        size_t capacity = arguments[3];
        const char *path = [[url path] fileSystemRepresentation];
        if (!buffer || !path || strlen(path) + 1 > capacity) {
            *result = 0;
        } else {
            memcpy(buffer, path, strlen(path) + 1);
            *result = 1;
        }
        if(getenv("LP32_TRACE_HID") && path && strstr(path,"InputDevices"))
            fprintf(stderr,"compat32: CFURLGetFileSystemRepresentation path=%s result=%llu\n",path,(unsigned long long)*result);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFRelease") ||
        LP32_NAME_IS(import_name, import_length, "_CFRetain") ||
        LP32_NAME_IS(import_name, import_length, "_dispatch_release") ||
        LP32_NAME_IS(import_name, import_length, "_dispatch_retain")) {
        bool retain = LP32_NAME_IS(import_name, import_length, "_CFRetain") ||
                      LP32_NAME_IS(import_name, import_length, "_dispatch_retain");
        id legacy = objc_legacy32_object(arguments[0]);
        if (legacy) {
            if (retain) [legacy retain]; else [legacy release];
            *result = retain ? arguments[0] : 0;
            return 1;
        }
        id released = nil;
        pthread_mutex_lock(&proxy_mutex);
        for (uint32_t i = 0; i < proxy_count; ++i) {
            struct proxy_entry *entry = &proxies[i];
            if (entry->handle != arguments[0] || !entry->object) continue;
            if (retain) ++entry->cf_owners;
            else if (entry->cf_owners && --entry->cf_owners == 0 && !entry->pinned) {
                released = entry->object;
                memset(entry, 0, sizeof(*entry));
                publish_proxy((uint32_t)(entry-proxies));
            }
            break;
        }
        pthread_mutex_unlock(&proxy_mutex);
        if (proxy_object_is_retained(released)) [released release];
        *result = retain ? arguments[0] : 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFGetTypeID")) {
        id object = object_for_argument(arguments[0]);
        *result = object ? CFGetTypeID((CFTypeRef)object) : 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateMutable")) {
        CFMutableStringRef string = CFStringCreateMutable(NULL, (CFIndex)(int32_t)arguments[1]);
        *result = proxy_for_object((id)string);
        if (string) CFRelease(string);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateMutableCopy")) {
        CFStringRef source=(CFStringRef)object_for_argument(arguments[2]);
        /* The game's DirectInput registry reader sometimes passes an
         * unresolved guest string token here. CoreFoundation crashes on a
         * NULL source; an empty mutable string lets the reader continue. */
        CFMutableStringRef string = CFStringCreateMutableCopy(NULL, (int32_t)arguments[1],
            source?source:CFSTR(""));
        *result = proxy_for_object((id)string);
        if (string) CFRelease(string);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateCopy")) {
        *result = proxy_for_object([[object_for_argument(arguments[1]) copy] autorelease]); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithPascalString")) {
        CFStringRef string = CFStringCreateWithPascalString(NULL,
            (const UInt8 *)(uintptr_t)arguments[1], arguments[2]);
        *result = proxy_for_object((id)string);
        if (string) CFRelease(string);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithCharacters")) {
        CFStringRef string = CFStringCreateWithCharacters(NULL,
            (const UniChar *)(uintptr_t)arguments[1], (int32_t)arguments[2]);
        *result = proxy_for_object((id)string);
        if (string) CFRelease(string);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithSubstring")) {
        CFStringRef string = CFStringCreateWithSubstring(NULL,
            (CFStringRef)object_for_argument(arguments[1]),
            CFRangeMake((int32_t)arguments[2], (int32_t)arguments[3]));
        *result = proxy_for_object((id)string);
        if (string) CFRelease(string);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithFormat") ||
        LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithFormatAndArguments")) {
        const uint32_t *args = LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithFormat") ? arguments + 3 : (const void *)(uintptr_t)arguments[3];
        CFStringRef string = format_cf_string32((CFStringRef)object_for_argument(arguments[2]), args);
        if (!string) return 0;
        *result = proxy_for_object((id)string); CFRelease(string); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringLowercase") ||
        LP32_NAME_IS(import_name, import_length, "_CFStringUppercase")) {
        CFMutableStringRef string = (CFMutableStringRef)object_for_argument(arguments[0]);
        CFLocaleRef locale = (CFLocaleRef)object_for_argument(arguments[1]);
        if (LP32_NAME_IS(import_name, import_length, "_CFStringLowercase")) CFStringLowercase(string, locale);
        else CFStringUppercase(string, locale);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetIntValue")) {
        *result = (uint32_t)CFStringGetIntValue((CFStringRef)object_for_argument(arguments[0])); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetDoubleValue")) {
        *result = compat_runtime32_return_double(CFStringGetDoubleValue((CFStringRef)object_for_argument(arguments[0]))); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringAppend")) {
        CFStringAppend((CFMutableStringRef)object_for_argument(arguments[0]),
                       (CFStringRef)object_for_argument(arguments[1])); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringInsert")) {
        CFStringInsert((CFMutableStringRef)object_for_argument(arguments[0]),
            (int32_t)arguments[1],(CFStringRef)object_for_argument(arguments[2]));
        *result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringDelete")) {
        CFStringDelete((CFMutableStringRef)object_for_argument(arguments[0]),
                       CFRangeMake((int32_t)arguments[1], (int32_t)arguments[2])); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringReplace")) {
        CFStringReplace((CFMutableStringRef)object_for_argument(arguments[0]),
                        CFRangeMake((int32_t)arguments[1], (int32_t)arguments[2]),
                        (CFStringRef)object_for_argument(arguments[3])); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetCharacters")) {
        CFStringGetCharacters((CFStringRef)object_for_argument(arguments[0]),
            CFRangeMake((int32_t)arguments[1], (int32_t)arguments[2]),
            (UniChar *)(uintptr_t)arguments[3]); *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetPascalString")) {
        *result = CFStringGetPascalString((CFStringRef)object_for_argument(arguments[0]),
            (UInt8 *)(uintptr_t)arguments[1], (int32_t)arguments[2], arguments[3]); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFHash")) {
        *result = (uint32_t)CFHash((CFTypeRef)object_for_argument(arguments[0])); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetBytes")) {
        CFIndex used=0;
        *result = (uint32_t)CFStringGetBytes((CFStringRef)object_for_argument(arguments[0]),
            CFRangeMake((int32_t)arguments[1],(int32_t)arguments[2]), arguments[3],
            (UInt8)arguments[4],arguments[5]!=0,(UInt8 *)(uintptr_t)arguments[6],
            (int32_t)arguments[7],arguments[8]?&used:NULL);
        if(arguments[8])*(int32_t *)(uintptr_t)arguments[8]=(int32_t)used;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringFindWithOptions")) {
        CFRange range;
        *result=CFStringFindWithOptions((CFStringRef)object_for_argument(arguments[0]),
            (CFStringRef)object_for_argument(arguments[1]),
            CFRangeMake((int32_t)arguments[2],(int32_t)arguments[3]),arguments[4],&range);
        if(*result && arguments[5]){int32_t *p=(void *)(uintptr_t)arguments[5];p[0]=(int32_t)range.location;p[1]=(int32_t)range.length;}
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetLength")) {
        NSString *string = object_for_argument(arguments[0]);
        *result = (uint32_t)[string length];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetSystemEncoding")) {
        *result = CFStringGetSystemEncoding();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringGetCString")) {
        NSString *string = object_for_argument(arguments[0]);
        char *buffer = (void *)(uintptr_t)arguments[1];
        CFIndex capacity = (int32_t)arguments[2];
        *result = CFStringGetCString((CFStringRef)string, buffer, capacity,
                                     arguments[3]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCompare")) {
        if (!object_for_argument(arguments[0]) || !object_for_argument(arguments[1])) {
            fprintf(stderr,"compat32: unresolved CFStringCompare arguments %#x %#x\n",arguments[0],arguments[1]);
            return 0;
        }
        NSString *left = object_for_argument(arguments[0]);
        NSString *right = object_for_argument(arguments[1]);
        *result = (uint32_t)CFStringCompare((CFStringRef)left,
                                            (CFStringRef)right, arguments[2]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringFind")) {
        NSString *string = object_for_argument(arguments[0]);
        NSString *needle = object_for_argument(arguments[1]);
        CFRange range = string && needle ?
            CFStringFind((CFStringRef)string, (CFStringRef)needle,
                         arguments[2]) : CFRangeMake(kCFNotFound, 0);
        *result = (uint32_t)(int32_t)range.location |
                  ((uint64_t)(uint32_t)range.length << 32);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithBytes")) {
        const UInt8 *bytes = (const void *)(uintptr_t)arguments[1];
        CFIndex length = (int32_t)arguments[2];
        CFStringRef string = CFStringCreateWithBytes(NULL, bytes, length,
                                                     arguments[3], false);
        *result = proxy_for_object([(NSString *)string autorelease]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayCreateMutable") || LP32_NAME_IS(import_name, import_length, "_CFArrayCreate")) {
        bool mutable=LP32_NAME_IS(import_name,import_length,"_CFArrayCreateMutable");
        if(!compat_runtime32_pointer_import_matches(arguments[mutable?2:3],"_kCFTypeArrayCallBacks"))return 0;
        CFArrayRef array;
        if(mutable)array=CFArrayCreateMutable(NULL,(int32_t)arguments[1],&kCFTypeArrayCallBacks);
        else{
            uint32_t count=arguments[2];const uint32_t *guest=(void *)(uintptr_t)arguments[1];
            const void **values=calloc(count?count:1,sizeof(*values));if(!values)return 0;
            for(uint32_t i=0;i<count;++i)values[i]=object_for_argument(guest[i]);
            array=CFArrayCreate(NULL,values,count,&kCFTypeArrayCallBacks);free(values);
        }
        *result=proxy_for_object((id)array);if(array)CFRelease(array);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayCreateCopy") || LP32_NAME_IS(import_name, import_length, "_CFArrayCreateMutableCopy")) {
        CFArrayRef array=LP32_NAME_IS(import_name,import_length,"_CFArrayCreateCopy")?
            CFArrayCreateCopy(NULL,(CFArrayRef)object_for_argument(arguments[1])):
            CFArrayCreateMutableCopy(NULL,(int32_t)arguments[1],(CFArrayRef)object_for_argument(arguments[2]));
        *result=proxy_for_object((id)array);if(array)CFRelease(array);return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CFArrayAppendValue") || LP32_NAME_IS(import_name,import_length,"_CFArrayInsertValueAtIndex") ||
        LP32_NAME_IS(import_name,import_length,"_CFArraySetValueAtIndex") || LP32_NAME_IS(import_name,import_length,"_CFArrayRemoveValueAtIndex") ||
        LP32_NAME_IS(import_name,import_length,"_CFArrayRemoveAllValues")) {
        CFMutableArrayRef array=(CFMutableArrayRef)object_for_argument(arguments[0]);
        if(LP32_NAME_IS(import_name,import_length,"_CFArrayAppendValue"))CFArrayAppendValue(array,object_for_argument(arguments[1]));
        else if(LP32_NAME_IS(import_name,import_length,"_CFArrayInsertValueAtIndex"))CFArrayInsertValueAtIndex(array,(int32_t)arguments[1],object_for_argument(arguments[2]));
        else if(LP32_NAME_IS(import_name,import_length,"_CFArraySetValueAtIndex"))CFArraySetValueAtIndex(array,(int32_t)arguments[1],object_for_argument(arguments[2]));
        else if(LP32_NAME_IS(import_name,import_length,"_CFArrayRemoveValueAtIndex"))CFArrayRemoveValueAtIndex(array,(int32_t)arguments[1]);
        else CFArrayRemoveAllValues(array);*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayGetCount")) {
        NSArray *array = object_for_argument(arguments[0]);
        *result = (uint32_t)[array count];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayGetTypeID")) {
        *result = CFArrayGetTypeID();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFArrayGetValueAtIndex")) {
        NSArray *array = object_for_argument(arguments[0]);
        NSUInteger index = arguments[1];
        *result = index < [array count] ? proxy_for_object([array objectAtIndex:index]) : 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CFDictionaryGetCount")) {
        *result=(uint32_t)CFDictionaryGetCount((CFDictionaryRef)object_for_argument(arguments[0]));return 1;
    }
    if (LP32_NAME_IS(import_name,import_length,"_CFDictionaryGetKeysAndValues")) {
        CFDictionaryRef dictionary=(CFDictionaryRef)object_for_argument(arguments[0]);CFIndex count=CFDictionaryGetCount(dictionary);
        const void **values=calloc(count?count*2:1,sizeof(*values));if(!values)return 0;
        CFDictionaryGetKeysAndValues(dictionary,values,values+count);
        for(CFIndex i=0;i<count;++i){
            if(arguments[1])((uint32_t *)(uintptr_t)arguments[1])[i]=proxy_for_object((id)values[i]);
            if(arguments[2])((uint32_t *)(uintptr_t)arguments[2])[i]=proxy_for_object((id)values[i+count]);
        }
        free(values);*result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryCreateMutable") ||
        LP32_NAME_IS(import_name, import_length, "_CFDictionaryCreate")) {
        bool mutable = LP32_NAME_IS(import_name, import_length, "_CFDictionaryCreateMutable");
        unsigned key_index = mutable ? 2 : 4, value_index = mutable ? 3 : 5;
        /* Custom/raw callbacks require their own value representation. Trap
           unsupported tables instead of treating guest addresses as CF objects. */
        if (!compat_runtime32_pointer_import_matches(arguments[key_index], "_kCFTypeDictionaryKeyCallBacks") ||
            !compat_runtime32_pointer_import_matches(arguments[value_index], "_kCFTypeDictionaryValueCallBacks")) return 0;
        CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(NULL, mutable ? (int32_t)arguments[1] : 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!mutable && dictionary) {
            const uint32_t *keys=(void *)(uintptr_t)arguments[1], *values=(void *)(uintptr_t)arguments[2];
            for(uint32_t i=0;i<arguments[3];++i)CFDictionaryAddValue(dictionary,object_for_argument(keys[i]),object_for_argument(values[i]));
            CFDictionaryRef immutable=CFDictionaryCreateCopy(NULL,dictionary);CFRelease(dictionary);
            *result=proxy_for_object((id)immutable);if(immutable)CFRelease(immutable);return 1;
        }
        *result=proxy_for_object((id)dictionary);if(dictionary)CFRelease(dictionary);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryCreateCopy") ||
        LP32_NAME_IS(import_name, import_length, "_CFDictionaryCreateMutableCopy")) {
        bool mutable=LP32_NAME_IS(import_name, import_length, "_CFDictionaryCreateMutableCopy");
        CFDictionaryRef dictionary=mutable ? CFDictionaryCreateMutableCopy(NULL,(int32_t)arguments[1],
            (CFDictionaryRef)object_for_argument(arguments[2])) : CFDictionaryCreateCopy(NULL,(CFDictionaryRef)object_for_argument(arguments[1]));
        *result=proxy_for_object((id)dictionary);if(dictionary)CFRelease(dictionary);return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryAddValue")) {
        CFDictionaryAddValue((CFMutableDictionaryRef)object_for_argument(arguments[0]),object_for_argument(arguments[1]),object_for_argument(arguments[2]));
        *result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryGetCountOfKey")) {
        *result=(uint32_t)CFDictionaryGetCountOfKey((CFDictionaryRef)object_for_argument(arguments[0]),object_for_argument(arguments[1]));return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryGetValue")) {
        NSDictionary *dictionary = object_for_argument(arguments[0]);
        id key = object_for_argument(arguments[1]);
        id value = [dictionary objectForKey:key];
        if (getenv("LP32_TRACE_HID") && [dictionary isKindOfClass:[NSDictionary class]] &&
            [key isKindOfClass:[NSString class]] &&
            [[dictionary objectForKey:@"VendorID"] intValue] == 1356 &&
            [[dictionary objectForKey:@"ProductID"] intValue] == 2508)
            fprintf(stderr,"compat32: HID profile lookup key=%s value=%s product=%s\n",
                [[key description] UTF8String],[[value description] UTF8String],
                [[[dictionary objectForKey:@"ProductID"] description] UTF8String]);
        if (getenv("LP32_TRACE_DISPLAY") &&
            [dictionary objectForKey:@"Width"]) {
            fprintf(stderr, "compat32: display dictionary get %s -> %s\n",
                    [[key description] UTF8String],
                    [[value description] UTF8String]);
        }
        *result = proxy_for_object(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryGetTypeID")) {
        *result = CFDictionaryGetTypeID();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionaryGetValueIfPresent")) {
        NSDictionary *dictionary = object_for_argument(arguments[0]);
        id key = object_for_argument(arguments[1]);
        id value = [dictionary objectForKey:key];
        uint32_t *output = (void *)(uintptr_t)arguments[2];
        if (output) *output = proxy_for_object(value);
        *result = value != nil;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDictionarySetValue")) {
        NSMutableDictionary *dictionary = object_for_argument(arguments[0]);
        id key = object_for_argument(arguments[1]);
        id value = object_for_argument(arguments[2]);
        if (key && value) [dictionary setObject:value forKey:key];
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataCreate")) {
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)(uintptr_t)arguments[1], (int32_t)arguments[2]);
        *result = proxy_for_object((id)data); if (data) CFRelease(data); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataCreateMutable")) {
        CFMutableDataRef data = CFDataCreateMutable((CFAllocatorRef)object_for_argument(arguments[0]),
                                                   (int32_t)arguments[1]);
        *result = proxy_for_object((id)data);
        if (data) CFRelease(data);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataSetLength")) {
        CFDataSetLength((CFMutableDataRef)object_for_argument(arguments[0]), (int32_t)arguments[1]);
        *result = 0; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataGetLength")) {
        *result = CFDataGetLength((CFDataRef)object_for_argument(arguments[0])); return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataGetBytes")) {
        CFDataGetBytes((CFDataRef)object_for_argument(arguments[0]),
            CFRangeMake((int32_t)arguments[1], (int32_t)arguments[2]),
            (UInt8 *)(uintptr_t)arguments[3]);
        *result=0;return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataGetMutableBytePtr")) {
        UInt8 *bytes = CFDataGetMutableBytePtr((CFMutableDataRef)object_for_argument(arguments[0]));
        if ((uintptr_t)bytes > UINT32_MAX) return 0; /* Never truncate writable host memory. */
        *result = (uintptr_t)bytes; return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataGetTypeID")) {
        *result = CFDataGetTypeID();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataGetBytePtr")) {
        NSData *data = object_for_argument(arguments[0]);
        *result = (uintptr_t)[data bytes] <= UINT32_MAX ?
            (uintptr_t)[data bytes] : guest_bytes_for_data(data);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBooleanGetValue")) {
        NSNumber *number = object_for_argument(arguments[0]);
        *result = [number boolValue];
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleCopyCurrent")) {
        *result = proxy_for_object([NSLocale currentLocale]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleCopyPreferredLanguages")) {
        *result = proxy_for_object([NSLocale preferredLanguages]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleCreate")) {
        NSString *identifier = object_for_argument(arguments[1]);
        NSLocale *locale = [[[NSLocale alloc] initWithLocaleIdentifier:identifier]
                            autorelease];
        *result = proxy_for_object(locale);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleGetIdentifier")) {
        NSLocale *locale = object_for_argument(arguments[0]);
        *result = proxy_for_object([locale localeIdentifier]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFLocaleGetValue")) {
        NSLocale *locale = object_for_argument(arguments[0]);
        NSString *key = object_for_argument(arguments[1]);
        *result = proxy_for_object([locale objectForKey:key]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFEqual")) {
        CFTypeRef left = (CFTypeRef)object_for_argument(arguments[0]);
        CFTypeRef right = (CFTypeRef)object_for_argument(arguments[1]);
        *result = left && right ? CFEqual(left, right) : left == right;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFAttributedStringCreate")) {
        CFAttributedStringRef value = CFAttributedStringCreate(NULL,
            (CFStringRef)object_for_argument(arguments[1]),
            (CFDictionaryRef)object_for_argument(arguments[2]));
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleGetBundleWithIdentifier")) {
        *result = proxy_for_object((id)CFBundleGetBundleWithIdentifier(
            (CFStringRef)object_for_argument(arguments[0])));
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleIsExecutableLoaded")) {
        *result = CFBundleIsExecutableLoaded(
            (CFBundleRef)object_for_argument(arguments[0]));
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleOpenBundleResourceMap")) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        *result = (uint32_t)(int32_t)CFBundleOpenBundleResourceMap(
            (CFBundleRef)object_for_argument(arguments[0]));
#pragma clang diagnostic pop
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFBundleCopyPrivateFrameworksURL")) {
        CFURLRef value = CFBundleCopyPrivateFrameworksURL(
            (CFBundleRef)object_for_argument(arguments[0]));
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length,
                     "_CFBundleCopyLocalizationsForPreferences")) {
        CFArrayRef value = CFBundleCopyLocalizationsForPreferences(
            (CFArrayRef)object_for_argument(arguments[0]),
            (CFArrayRef)object_for_argument(arguments[1]));
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataCreateWithBytesNoCopy")) {
        CFDataRef value = CFDataCreate(NULL,
            (const UInt8 *)(uintptr_t)arguments[1], (CFIndex)(int32_t)arguments[2]);
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataIncreaseLength")) {
        CFDataIncreaseLength((CFMutableDataRef)object_for_argument(arguments[0]),
                             (CFIndex)(int32_t)arguments[1]);
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDataReplaceBytes")) {
        CFDataReplaceBytes((CFMutableDataRef)object_for_argument(arguments[0]),
            CFRangeMake((int32_t)arguments[1], (int32_t)arguments[2]),
            (const UInt8 *)(uintptr_t)arguments[3], (CFIndex)(int32_t)arguments[4]);
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFStringCreateWithBytesNoCopy")) {
        CFStringRef value = CFStringCreateWithBytes(NULL,
            (const UInt8 *)(uintptr_t)arguments[1], (CFIndex)(int32_t)arguments[2],
            (CFStringEncoding)arguments[3], arguments[4] != 0);
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFURLCreateData")) {
        CFDataRef value = CFURLCreateData(NULL,
            (CFURLRef)object_for_argument(arguments[1]),
            (CFStringEncoding)arguments[2], arguments[3] != 0);
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length,
                     "_CFURLCreateDataAndPropertiesFromResource")) {
        CFDataRef data = NULL;
        CFDictionaryRef properties = NULL;
        SInt32 error = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        Boolean ok = CFURLCreateDataAndPropertiesFromResource(NULL,
            (CFURLRef)object_for_argument(arguments[1]),
            arguments[2] ? &data : NULL,
            arguments[3] ? &properties : NULL,
            (CFArrayRef)object_for_argument(arguments[4]),
            arguments[5] ? &error : NULL);
#pragma clang diagnostic pop
        if (arguments[2]) *(uint32_t *)(uintptr_t)arguments[2] =
            proxy_for_object((id)data);
        if (arguments[3]) *(uint32_t *)(uintptr_t)arguments[3] =
            proxy_for_object((id)properties);
        if (arguments[5]) *(SInt32 *)(uintptr_t)arguments[5] = error;
        if (data) CFRelease(data);
        if (properties) CFRelease(properties);
        *result = ok;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDateCompare")) {
        *result = (uint32_t)CFDateCompare(
            (CFDateRef)object_for_argument(arguments[0]),
            (CFDateRef)object_for_argument(arguments[1]),
            (void *)(uintptr_t)arguments[2]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFCalendarCreateWithIdentifier")) {
        CFCalendarRef value = CFCalendarCreateWithIdentifier(NULL,
            (CFStringRef)object_for_argument(arguments[1]));
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFCalendarSetTimeZone")) {
        CFCalendarSetTimeZone((CFCalendarRef)object_for_argument(arguments[0]),
            (CFTimeZoneRef)object_for_argument(arguments[1]));
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFCalendarComposeAbsoluteTime")) {
        CFCalendarRef calendar = (CFCalendarRef)object_for_argument(arguments[0]);
        CFAbsoluteTime value = 0;
        const char *components = (const char *)(uintptr_t)arguments[2];
        const int32_t *v = (const int32_t *)(arguments + 3);
        if (!components) return 0;
        size_t component_count = strnlen(components, 9);
        if (component_count > 8) return 0;
        Boolean ok = false;
        switch (component_count) {
        case 0: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components); break;
        case 1: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components, v[0]); break;
        case 2: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components, v[0], v[1]); break;
        case 3: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components, v[0], v[1], v[2]); break;
        case 4: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components, v[0], v[1], v[2], v[3]); break;
        case 5: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components, v[0], v[1], v[2], v[3], v[4]); break;
        case 6: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components, v[0], v[1], v[2], v[3], v[4], v[5]); break;
        case 7: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components, v[0], v[1], v[2], v[3], v[4], v[5], v[6]); break;
        case 8: ok = CFCalendarComposeAbsoluteTime(calendar, &value, components, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); break;
        }
        if (arguments[1]) memcpy((void *)(uintptr_t)arguments[1], &value, sizeof(value));
        *result = ok;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNumberFormatterCreate")) {
        CFNumberFormatterRef value = CFNumberFormatterCreate(NULL,
            (CFLocaleRef)object_for_argument(arguments[1]),
            (CFNumberFormatterStyle)arguments[2]);
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNumberFormatterSetProperty")) {
        CFNumberFormatterSetProperty(
            (CFNumberFormatterRef)object_for_argument(arguments[0]),
            (CFStringRef)object_for_argument(arguments[1]),
            (CFTypeRef)object_for_argument(arguments[2]));
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length,
                     "_CFNumberFormatterCreateStringWithNumber")) {
        CFStringRef value = CFNumberFormatterCreateStringWithNumber(NULL,
            (CFNumberFormatterRef)object_for_argument(arguments[1]),
            (CFNumberRef)object_for_argument(arguments[2]));
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length,
                     "_CFNumberFormatterCreateStringWithValue")) {
        uint32_t create[] = {0, arguments[2], arguments[3]};
        uint64_t number_token = 0;
        if (!objc_bridge32_dispatch("_CFNumberCreate", create, &number_token)) return 0;
        CFStringRef value = CFNumberFormatterCreateStringWithNumber(NULL,
            (CFNumberFormatterRef)object_for_argument(arguments[1]),
            (CFNumberRef)object_for_argument((uint32_t)number_token));
        *result = proxy_for_object((id)value);
        if (value) CFRelease(value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNotificationCenterGetDistributedCenter")) {
        *result = proxy_for_object((id)CFNotificationCenterGetDistributedCenter());
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNotificationCenterAddObserver")) {
        struct cf_notification32 *notification = malloc(sizeof(*notification));
        if (!notification) return 0;
        *notification = (struct cf_notification32){arguments[1], arguments[2]};
        CFNotificationCenterAddObserver(
            (CFNotificationCenterRef)object_for_argument(arguments[0]),
            notification, cf_notification_callback,
            (CFStringRef)object_for_argument(arguments[3]),
            object_for_argument(arguments[4]),
            (CFNotificationSuspensionBehavior)arguments[5]);
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFRunLoopPerformBlock")) {
        LP32GuestBlock *owner = [[LP32GuestBlock alloc] init];
        owner->guestBlock = blocks_bridge32_copy(arguments[2]);
        if (!owner->guestBlock) { [owner release]; return 0; }
        CFRunLoopPerformBlock(
            (CFRunLoopRef)object_for_argument(arguments[0]),
            (CFTypeRef)object_for_argument(arguments[1]),
            ^{ blocks_bridge32_invoke(owner->guestBlock); });
        [owner release];
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFDateFormatterCreate")) {
        CFLocaleRef locale = (CFLocaleRef)object_for_argument(arguments[1]);
        CFDateFormatterRef formatter = CFDateFormatterCreate(
            NULL, locale, (CFDateFormatterStyle)arguments[2],
            (CFDateFormatterStyle)arguments[3]);
        *result = proxy_for_object((id)formatter);
        if (formatter) CFRelease(formatter);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length,
                     "_CFDateFormatterCreateStringWithAbsoluteTime")) {
        CFAbsoluteTime time;
        memcpy(&time, arguments + 2, sizeof(time));
        CFStringRef string = CFDateFormatterCreateStringWithAbsoluteTime(
            NULL, (CFDateFormatterRef)object_for_argument(arguments[1]), time);
        *result = proxy_for_object((id)string);
        if (string) CFRelease(string);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length,
                     "_CFDateFormatterCreateStringWithDate")) {
        CFStringRef string = CFDateFormatterCreateStringWithDate(
            NULL, (CFDateFormatterRef)object_for_argument(arguments[1]),
            (CFDateRef)object_for_argument(arguments[2]));
        *result = proxy_for_object((id)string);
        if (string) CFRelease(string);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNumberCreate")) {
        CFNumberType requested_type = (CFNumberType)arguments[1];
        const void *guest_value = (const void *)(uintptr_t)arguments[2];
        CFNumberRef number = NULL;

        /* CFNumber's platform-sized types describe the caller's ABI.  The
           guest is i386, so long/CFIndex/NSInteger are four bytes and
           CGFloat is a float.  Passing those type tags straight to the
           64-bit host CoreFoundation makes it read eight bytes from the
           guest stack. */
        if (requested_type == kCFNumberLongType ||
            requested_type == kCFNumberCFIndexType ||
            requested_type == kCFNumberNSIntegerType) {
            int32_t value = 0;
            if (guest_value) memcpy(&value, guest_value, sizeof(value));
            number = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);
        } else if (requested_type == kCFNumberCGFloatType) {
            Float32 value = 0.0f;
            if (guest_value) memcpy(&value, guest_value, sizeof(value));
            number = CFNumberCreate(NULL, kCFNumberFloat32Type, &value);
        } else {
            number = CFNumberCreate(NULL, requested_type, guest_value);
        }
        *result = proxy_for_object([(NSNumber *)number autorelease]);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFNumberGetValue")) {
        NSNumber *number = object_for_argument(arguments[0]);
        CFNumberType requested_type = (CFNumberType)arguments[1];
        void *guest_output = (void *)(uintptr_t)arguments[2];
        Boolean converted = false;

        if (requested_type == kCFNumberLongType ||
            requested_type == kCFNumberCFIndexType ||
            requested_type == kCFNumberNSIntegerType) {
            int32_t value = 0;
            converted = CFNumberGetValue((CFNumberRef)number,
                                         kCFNumberSInt32Type, &value);
            if (guest_output) memcpy(guest_output, &value, sizeof(value));
        } else if (requested_type == kCFNumberCGFloatType) {
            Float32 value = 0.0f;
            converted = CFNumberGetValue((CFNumberRef)number,
                                         kCFNumberFloat32Type, &value);
            if (guest_output) memcpy(guest_output, &value, sizeof(value));
        } else {
            converted = CFNumberGetValue((CFNumberRef)number, requested_type,
                                         guest_output);
        }
        if (getenv("LP32_TRACE_DISPLAY")) {
            fprintf(stderr,
                    "compat32: CFNumberGetValue value=%s type=%u "
                    "output=0x%08x converted=%u\n",
                    [[number description] UTF8String], arguments[1],
                    arguments[2], converted);
        }
        *result = converted;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFAbsoluteTimeGetCurrent")) {
        double value = CFAbsoluteTimeGetCurrent();
        memcpy(result, &value, sizeof(value));
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFShow")) {
        id object = object_for_argument(arguments[0]);
        fprintf(stderr, "compat32: CFShow %s\n", [[object description] UTF8String]);
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_CFCopyDescription")) {
        CFTypeRef object=(CFTypeRef)object_for_argument(arguments[0]);
        CFStringRef description=object?CFCopyDescription(object):NULL;
        *result=proxy_for_object((id)description);if(description)CFRelease(description);return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_TISCopyCurrentKeyboardInputSource") ||
        LP32_NAME_IS(import_name, import_length, "_TISCopyCurrentKeyboardLayoutInputSource") ||
        LP32_NAME_IS(import_name, import_length, "_TISCopyCurrentASCIICapableKeyboardLayoutInputSource")) {
        TISInputSourceRef source = strstr(import_name, "ASCIICapable") ?
            TISCopyCurrentASCIICapableKeyboardLayoutInputSource() :
            strstr(import_name, "Layout") ? TISCopyCurrentKeyboardLayoutInputSource() :
            TISCopyCurrentKeyboardInputSource();
        *result = proxy_for_object((id)source);
        if (source) CFRelease(source);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_TISGetInputSourceProperty")) {
        TISInputSourceRef source =
            (TISInputSourceRef)object_for_argument(arguments[0]);
        CFStringRef key = (CFStringRef)object_for_argument(arguments[1]);
        CFTypeRef value = source && key ?
            TISGetInputSourceProperty(source, key) : NULL;
        *result = proxy_for_object((id)value);
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_LMGetKbdType")) {
        *result = LMGetKbdType();
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_UCKeyTranslate")) {
        const UCKeyboardLayout *layout =
            (const void *)(uintptr_t)arguments[0];
        *result = (uint32_t)UCKeyTranslate(
            layout, (UInt16)arguments[1], (UInt16)arguments[2], arguments[3],
            arguments[4], arguments[5],
            (UInt32 *)(uintptr_t)arguments[6], arguments[7],
            (UniCharCount *)(uintptr_t)arguments[8],
            (UniChar *)(uintptr_t)arguments[9]);
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_NSRunAlertPanel")) {
        NSString *title = object_for_argument(arguments[0]);
        NSString *message = object_for_argument(arguments[1]);
        fprintf(stderr, "compat32: legacy alert: %s — %s\n",
                [[title description] UTF8String], [[message description] UTF8String]);
        /* The original API is a blocking convenience wrapper.  Logging and
           accepting its sole default button preserves control flow without
           stalling an automated launch behind a modal compatibility alert. */
        *result = NSAlertFirstButtonReturn;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSLog")) {
        NSString *format = object_for_argument(arguments[0]);
        fprintf(stderr, "compat32: NSLog: %s\n", [[format description] UTF8String]);
        *result = 0;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSSearchPathForDirectoriesInDomains")) {
        NSArray *paths = NSSearchPathForDirectoriesInDomains(arguments[0],
                                                              arguments[1],
                                                              arguments[2] != 0);
        /* Test sessions redirect the save/config tree so an automated run can
           never touch the player's real saved games. */
        const char *override = getenv("LP32_APPLICATION_SUPPORT_DIR");
        if (override && override[0] &&
            arguments[0] == NSApplicationSupportDirectory &&
            (arguments[1] & NSUserDomainMask) != 0) {
            paths = @[[NSString stringWithUTF8String:override]];
        }
        *result = proxy_for_object(paths);
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_NSApplicationMain")) {
        objc_legacy32_register_classes();
        if(getenv("LP32_TEST_DISPLAY")) {
            NSTimer *placement = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
                (void)timer;
                for(NSWindow *window in NSApp.windows) place_test_window(window);
            }];
            [[NSRunLoop mainRunLoop] addTimer:placement forMode:NSRunLoopCommonModes];
            [[NSRunLoop mainRunLoop] addTimer:placement forMode:NSModalPanelRunLoopMode];
        }
        int argc = (int)arguments[0];
        const uint32_t *guest_argv = (const void *)(uintptr_t)arguments[1];
        if (argc < 0 || argc > 64 || !guest_argv) return 0;
        const char *host_argv[65] = {0};
        for (int index = 0; index < argc; ++index) {
            host_argv[index] = (const char *)(uintptr_t)guest_argv[index];
        }
        [[NSUserDefaults standardUserDefaults] setBool:YES
                                                forKey:@"ApplePersistenceIgnoreState"];
        *result = (uint32_t)NSApplicationMain(argc, host_argv);
        return 1;
    }

    if (LP32_NAME_IS(import_name, import_length, "_objc_msgSend") ||
        LP32_NAME_IS(import_name, import_length, "_objc_msgSend_fpret")) {
        if (objc_legacy32_message(arguments, result)) return 1;
        id receiver = object_for_receiver(arguments[0]);
        const char *selector_name = (const char *)(uintptr_t)arguments[1];

        if(receiver && selector_name && !strcmp(selector_name,"copyNextSampleBuffer") &&
            [receiver isKindOfClass:[AVAssetReaderOutput class]]) {
            CMSampleBufferRef sample=[(AVAssetReaderOutput *)receiver copyNextSampleBuffer];
            if(getenv("LP32_TRACE_MEDIA")) {
                static unsigned samples;
                if(samples++<8 || !(samples%120)) {
                    CVImageBufferRef pixel=sample?CMSampleBufferGetImageBuffer(sample):NULL;
                    OSType format=pixel?CVPixelBufferGetPixelFormatType(pixel):0;
                    fprintf(stderr,"compat32: movie sample=%u ptr=%p pts=%.3f pixel=%p size=%zux%zu format=%c%c%c%c planar=%d stride=%zu\n",samples,sample,
                        sample?CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)):0,pixel,
                        pixel?CVPixelBufferGetWidth(pixel):0,pixel?CVPixelBufferGetHeight(pixel):0,
                        (char)(format>>24),(char)(format>>16),(char)(format>>8),(char)format,
                        pixel?(int)CVPixelBufferIsPlanar(pixel):-1,pixel?CVPixelBufferGetBytesPerRow(pixel):0);
                }
            }
            bool previous=creating_cf_object;creating_cf_object=true;
            *result=proxy_for_object((id)sample);creating_cf_object=previous;
            if(sample)CFRelease(sample);return 1;
        }
        if (receiver && selector_name &&
            !strcmp(selector_name,"loadValuesAsynchronouslyForKeys:completionHandler:")) {
            LP32GuestBlock *owner=[[LP32GuestBlock alloc] init];
            owner->guestBlock=blocks_bridge32_copy(arguments[3]);
            if(!owner->guestBlock){[owner release];return 0;}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [(AVAsset *)receiver loadValuesAsynchronouslyForKeys:object_for_argument(arguments[2])
                completionHandler:^{ blocks_bridge32_invoke(owner->guestBlock); }];
#pragma clang diagnostic pop
            [owner release];*result=0;return 1;
        }
        if (receiver == [NSEvent class] && selector_name) {
            if (!strcmp(selector_name,"addLocalMonitorForEventsMatchingMask:handler:")) {
                *result=proxy_for_object(add_guest_event_monitor(arguments[2],arguments[3]));
                return 1;
            }
            if (!strcmp(selector_name,"removeMonitor:")) {
                remove_guest_event_monitor(object_for_argument(arguments[2]));
                *result=0;return 1;
            }
        }
        static int trace_selectors = -1;
        if (trace_selectors < 0) {
            trace_selectors = getenv("LP32_TRACE_OBJC_SELECTORS") != NULL;
        }
        if (trace_selectors && selector_name) {
            fprintf(stderr, "compat32: objc swap=%llu %s receiver=%s\n",
                    (unsigned long long)objc_bridge_swap_count, selector_name,
                    receiver ? class_getName(object_getClass(receiver)) : "(none)");
        }
        /* Guest pools span bridge calls. The dispatch pool has
           already drained any nested host pool by then, so keep its guest
           pool token inert; host temporaries retain their per-call lifetime. */
        if (selector_name) {
            static NSObject *pool_token;
            if (receiver == [NSAutoreleasePool class] &&
                (strcmp(selector_name, "alloc") == 0 || strcmp(selector_name, "new") == 0)) {
                if (!pool_token) pool_token = [[NSObject alloc] init];
                guest_autorelease_push();
                *result = proxy_for_object(pool_token);
                return 1;
            }
            if (pool_token && receiver == pool_token) {
                if(!strcmp(selector_name,"drain") || !strcmp(selector_name,"release"))guest_autorelease_pop();
                *result = (strcmp(selector_name, "drain") == 0 ||
                           strcmp(selector_name, "release") == 0) ? 0 : arguments[0];
                return 1;
            }
        }
        if (selector_name &&
            (!strcmp(selector_name,"retain") || !strcmp(selector_name,"release") || !strcmp(selector_name,"autorelease")) &&
            proxy_uses_guest_ownership(arguments[0])) {
            if(!strcmp(selector_name,"retain") || !strcmp(selector_name,"release")) {
                bool retain=!strcmp(selector_name,"retain");
                return objc_bridge32_dispatch(retain?"_CFRetain":"_CFRelease",arguments,result);
            }
            if(!strcmp(selector_name,"autorelease")) {
                guest_autorelease_add(arguments[0]);*result=arguments[0];return 1;
            }
        }
        if (selector_name && strcmp(selector_name,"runModal")==0 && [receiver isKindOfClass:[NSAlert class]]) {
            NSAlert *alert=receiver;
            fprintf(stderr,"compat32: launcher alert: %s | %s\n",alert.messageText.UTF8String,alert.informativeText.UTF8String);
            for(NSButton *button in alert.buttons)fprintf(stderr,"compat32: alert button: %s\n",button.title.UTF8String);
            [alert layout];place_test_window(alert.window);
            const char *test_message=getenv("LP32_TEST_ALERT_MESSAGE"), *test_button=getenv("LP32_TEST_ALERT_BUTTON");
            if(test_message && test_button && [alert.messageText isEqualToString:[NSString stringWithUTF8String:test_message]]) {
                for(NSButton *button in alert.buttons)if([button.title isEqualToString:[NSString stringWithUTF8String:test_button]]) {
                    NSTimer *click=[NSTimer timerWithTimeInterval:0.25 repeats:NO block:^(NSTimer *timer){(void)timer;[button performClick:nil];}];
                    [[NSRunLoop currentRunLoop] addTimer:click forMode:NSModalPanelRunLoopMode];break;
                }
            }
        }
        /* Unattended test runs must never take focus away from the user: the
           game activates itself and makes its window key at startup, so answer
           those without doing them (the window is still ordered in). */
        if (handle_test_activation(receiver, selector_name, result)) return 1;
        if (background_test_mode() && !strcmp(selector_name, "setView:") &&
            [receiver isKindOfClass:[NSOpenGLContext class]]) {
            NSView *view = object_for_argument(arguments[2]);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [(NSOpenGLContext *)receiver setView:view];
#pragma clang diagnostic pop
            if (view.window) simulate_test_window_activation(view.window);
            if (view.window) install_window_test_input(view.window);
            *result = 0;
            return 1;
        }
        if (getenv("LP32_TRACE_INPUT") && selector_name &&
            (strstr(selector_name, "Event") || strstr(selector_name, "event") ||
             strstr(selector_name, "key") || strstr(selector_name, "Key") ||
             strstr(selector_name, "character") ||
             strstr(selector_name, "modifier") ||
             strstr(selector_name, "button") || strstr(selector_name, "Button"))) {
            fprintf(stderr, "compat32: input ObjC %s receiver=%s\n",
                    selector_name,
                    receiver ? class_getName(object_getClass(receiver)) : "(none)");
        }
        if (selector_name && !strcmp(selector_name,"countByEnumeratingWithState:objects:count:"))
            return enumerate_guest(receiver,arguments,result);
        if (!arguments[0]) { *result = 0; return 1; }
        if (!receiver || !selector_name) {
            fprintf(stderr,
                    "compat32: Objective-C lookup failed receiver=0x%08x selector=%s\n",
                    arguments[0], selector_name ? selector_name : "(null)");
            return 0;
        }
        if (!strcmp(selector_name, "setMainFrameURL:") && getenv("LP32_TEST_WEB_CLICK_ID")) {
            NSString *identifier = [NSString stringWithUTF8String:getenv("LP32_TEST_WEB_CLICK_ID")];
            NSData *json = [NSJSONSerialization dataWithJSONObject:@[identifier] options:0 error:NULL];
            NSString *encoded = [[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] autorelease];
            NSString *script = [NSString stringWithFormat:
                @"(function(){var b=document.getElementById((%@)[0]);if(!b||b.disabled||!b.offsetParent)return '';b.click();return 'clicked';})()", encoded];
            NSDate *ready = [NSDate dateWithTimeIntervalSinceNow:12];
            NSTimer *click = [NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
                if ([ready timeIntervalSinceNow] > 0) return;
                NSString *value = [receiver performSelector:NSSelectorFromString(@"stringByEvaluatingJavaScriptFromString:") withObject:script];
                if ([value isEqualToString:@"clicked"]) {
                    fprintf(stderr, "compat32: test clicked web control %s\n", identifier.UTF8String);
                    [timer invalidate];
                }
            }];
            [[NSRunLoop mainRunLoop] addTimer:click forMode:NSRunLoopCommonModes];
        }
        if (!strcmp(selector_name, "arrayWithObjects:") || !strcmp(selector_name, "initWithObjects:") ||
            !strcmp(selector_name, "arrayWithObjects:count:") || !strcmp(selector_name, "initWithObjects:count:")) {
            bool counted = strstr(selector_name, "count:") != NULL;
            const uint32_t *values = counted ? (void *)(uintptr_t)arguments[2] : arguments + 2;
            size_t count = counted ? arguments[3] : 0;
            if (!counted) {while (count < 1024 && values[count]) ++count; if (count == 1024) return 0;}
            if (count > 1048576 || (count && !values)) return 0;
            id *objects = calloc(count ? count : 1, sizeof(id));
            if (!objects) return 0;
            for (size_t i = 0; i < count; ++i) {
                objects[i] = object_for_argument(values[i]);
                if (!objects[i]) { free(objects); return 0; }
            }
            id value = !strncmp(selector_name, "init", 4) ?
                [(NSArray *)receiver initWithObjects:objects count:count] :
                [(Class)receiver arrayWithObjects:objects count:count];
            free(objects); *result = proxy_for_object(value); return 1;
        }
        if (!strcmp(selector_name, "dictionaryWithObjects:forKeys:count:") ||
            !strcmp(selector_name, "initWithObjects:forKeys:count:")) {
            size_t count=arguments[4];
            const uint32_t *values=(void *)(uintptr_t)arguments[2];
            const uint32_t *guest_keys=(void *)(uintptr_t)arguments[3];
            if(count>1048576 || (count && (!values || !guest_keys)))return 0;
            id *objects=calloc(count?count:1,sizeof(id));
            id *keys=calloc(count?count:1,sizeof(id));
            if(!objects || !keys){free(objects);free(keys);return 0;}
            for(size_t i=0;i<count;++i) {
                objects[i]=object_for_argument(values[i]);keys[i]=object_for_argument(guest_keys[i]);
                if(!objects[i] || !keys[i]){free(objects);free(keys);return 0;}
            }
            id value=!strncmp(selector_name,"init",4) ?
                [(NSDictionary *)receiver initWithObjects:objects forKeys:keys count:count] :
                [(Class)receiver dictionaryWithObjects:objects forKeys:keys count:count];
            free(objects);free(keys);*result=proxy_for_object(value);return 1;
        }
        if (!strcmp(selector_name, "dictionaryWithObjectsAndKeys:") || !strcmp(selector_name, "initWithObjectsAndKeys:")) {
            size_t count = 0;
            const uint32_t *values = arguments + 2;
            while (count < 512 && values[count * 2]) ++count;
            if (count == 512) return 0;
            id objects[512], keys[512];
            for (size_t i = 0; i < count; ++i) {
                objects[i] = object_for_argument(values[i * 2]);
                keys[i] = object_for_argument(values[i * 2 + 1]);
                if (!objects[i] || !keys[i]) return 0;
            }
            id value = !strncmp(selector_name, "init", 4) ?
                [(NSDictionary *)receiver initWithObjects:objects forKeys:keys count:count] :
                [(Class)receiver dictionaryWithObjects:objects forKeys:keys count:count];
            *result = proxy_for_object(value); return 1;
        }
        if ((!strcmp(selector_name, "stringWithFormat:") || !strcmp(selector_name, "initWithFormat:")) &&
            (receiver == (id)[NSString class] || [receiver isKindOfClass:[NSString class]])) {
            CFStringRef formatted = format_cf_string32((CFStringRef)object_for_argument(arguments[2]), arguments + 3);
            if (!formatted) return 0;
            bool string_init=managed_string_init(receiver,arguments[0],selector_name);
            if(string_init)[receiver retain];
            id value = !strncmp(selector_name, "init", 4) ? [(NSString *)receiver initWithString:(id)formatted] : (id)formatted;
            *result = proxy_for_string_result(receiver,arguments[0],selector_name,value,string_init);
            CFRelease(formatted); return 1;
        }
        if (strcmp(selector_name, "CGLContextObj") == 0) {
            CGLContextObj context = [(NSOpenGLContext *)receiver CGLContextObj];
            *result = objc_bridge32_guest_pointer(context);
            return 1;
        }
        if (strcmp(selector_name, "terminate:") == 0 &&
            [receiver isKindOfClass:[NSApplication class]]) {
            /* See the exit import handling in compat_runtime.c.  AppKit's
               normal termination teardown can wait forever on a quarantined
               mixed-ABI AUGraph, so the confirmed game quit ends here. */
            fprintf(stderr, "compat32: guest requested AppKit termination\n");
            compat_runtime32_heap_report("AppKit-terminate");
            fflush(NULL);
            _Exit(EXIT_SUCCESS);
        }
        if (strcmp(selector_name,
                   "initWithContentRect:styleMask:backing:defer:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            if ([receiver isKindOfClass:[NSFullScreenWindow class]]) {
                host_rect = [game_window_screen() frame];
                fprintf(stderr,
                        "compat32: placing fullscreen window at "
                        "%.0f,%.0f %.0fx%.0f\n",
                        host_rect.origin.x, host_rect.origin.y,
                        host_rect.size.width, host_rect.size.height);
            }
            id value = [(NSWindow *)receiver initWithContentRect:host_rect
                                                       styleMask:arguments[6]
                                                         backing:arguments[7]
                                                           defer:arguments[8] != 0];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name, "setContentView:") == 0 &&
            [receiver isKindOfClass:[NSFullScreenWindow class]] &&
            [object_for_argument(arguments[2]) isKindOfClass:[OpenGLView class]]) {
            /* Host the GL view inside a plain container so the emulated
               display mode can letterbox it; the window's black background
               fills the bars.  The game keeps talking to the GL view. */
            OpenGLView *view = object_for_argument(arguments[2]);
            NSRect bounds = [[(NSWindow *)receiver contentView] bounds];
            NSView *container = [[[NSView alloc] initWithFrame:bounds] autorelease];
            [container setAutoresizesSubviews:NO];
            [(NSWindow *)receiver setContentView:container];
            [container addSubview:view];
            [view setFrame:[view letterboxedFrame]];
            *result = 0;
            return 1;
        }
        if ((strcmp(selector_name, "setFrame:display:") == 0 ||
             strcmp(selector_name, "setFrame:display:animate:") == 0) &&
            [receiver isKindOfClass:[NSFullScreenWindow class]]) {
            /* Clone Wars' Feral layer re-anchors its fullscreen window at
               the main display's origin after creating it; keep it on the
               display the bridge chose. */
            NSRect host_rect = [game_screen_for_window(receiver) frame];
            [(NSWindow *)receiver setFrame:host_rect display:arguments[6] != 0];
            for (NSView *subview in [[(NSWindow *)receiver contentView] subviews]) {
                if ([subview isKindOfClass:[OpenGLView class]]) {
                    [(OpenGLView *)subview applyGuestDisplayMode];
                }
            }
            *result = 0;
            return 1;
        }
        if (strcmp(selector_name, "initWithFrame:") == 0 ||
            strcmp(selector_name, "initFullscreen:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            id value = strcmp(selector_name, "initFullscreen:") == 0 ?
                [(OpenGLView *)receiver initFullscreen:host_rect] :
                [(NSView *)receiver initWithFrame:host_rect];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name, "mainScreen") == 0 &&
            receiver == (id)[NSScreen class]) {
            *result = proxy_for_object(preferred_game_screen());
            return 1;
        }
        if (strcmp(selector_name, "initWithFrame:shareContext:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            NSOpenGLContext *share = object_for_argument(arguments[6]);
            id value = [(OpenGLView *)receiver initWithFrame:host_rect
                                                shareContext:share];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name,
                   "initWithFrame:shareContext:openGLDisplayMask:"
                   "sampleBuffers:samples:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            NSOpenGLContext *share = object_for_argument(arguments[6]);
            id value = [(OpenGLView *)receiver initWithFrame:host_rect
                                                shareContext:share
                                           openGLDisplayMask:arguments[7]
                                               sampleBuffers:(int)arguments[8]
                                                     samples:(int)arguments[9]];
            *result = proxy_for_object(value);
            return 1;
        }
        if (strcmp(selector_name,
                   "initFullscreen:openGLDisplayMask:sampleBuffers:samples:") == 0) {
            const float *rect = (const void *)(arguments + 2);
            NSRect host_rect = NSMakeRect(rect[0], rect[1], rect[2], rect[3]);
            id value = [(OpenGLView *)receiver initFullscreen:host_rect
                                            openGLDisplayMask:arguments[6]
                                                sampleBuffers:(int)arguments[7]
                                                      samples:(int)arguments[8]];
            *result = proxy_for_object(value);
            return 1;
        }
        bool guest_event_wait = false;
        if (strcmp(selector_name,
                   "nextEventMatchingMask:untilDate:inMode:dequeue:") == 0 &&
            [receiver isKindOfClass:[NSApplication class]]) {
            guest_event_wait = pthread_main_np();
            /*
             * The guest pumps its own event loop (mask 0xffffffff, default
             * mode) after finishLaunching and never enters -[NSApplication
             * run].  Apple events (Dock "Quit", a quit from the Finder or a
             * script) are received and then parked in the Carbon high-level
             * event queue, which only -run drains, so a Dock quit did nothing
             * and the only way out was Force Quit.
             */
            static bool logged_mask;
            if (!logged_mask) {
                logged_mask = true;
                id until = object_for_argument(arguments[3]);
                id mode = object_for_argument(arguments[4]);
                fprintf(stderr, "compat32: guest event pump mask=0x%08x "
                        "until=%s mode=%s dequeue=%u\n", arguments[2],
                        until ? [[until description] UTF8String] : "(nil)",
                        mode ? [[mode description] UTF8String] : "(nil)",
                        arguments[5]);
            }
            {
                /* Apple events wait in the Carbon high-level event queue
                   (the log says "Enqueuing event to local high-level event
                   queue") and nothing in this process drains it: AppKit does
                   that from -[NSApplication run], which the guest never
                   enters.  Drain it here, then the handlers run. */
                EventTypeSpec kinds = { kEventClassAppleEvent, kEventAppleEvent };
                EventRef carbon_event = NULL;
                while (ReceiveNextEvent(1, &kinds, kEventDurationNoWait, true,
                                        &carbon_event) == noErr) {
                    OSStatus status = AEProcessEvent(carbon_event);
                    fprintf(stderr, "compat32: processed a queued Apple event "
                            "(status %d)\n", (int)status);
                    ReleaseEvent(carbon_event);
                }
            }
        }
        if(getenv("LP32_TRACE_EVENTS") && !strcmp(selector_name,"postEvent:atStart:")) {
            NSEvent *e=object_for_argument(arguments[2]);
            if(e.type==NSEventTypeApplicationDefined)fprintf(stderr,"POST t=%.6f tid=%u token=%08x data=%lx,%lx subtype=%d atStart=%u\n",CFAbsoluteTimeGetCurrent(),pthread_mach_thread_np(pthread_self()),arguments[2],(long)e.data1,(long)e.data2,(int)e.subtype,arguments[3]);
        }
        if (selector_name[0] == 'p' &&
            strcmp(selector_name, "postEvent:atStart:") == 0 &&
            !pthread_main_np() && receiver == NSApp &&
            !getenv("LP32_QUEUED_APP_EVENTS")) {
            NSEvent *event = object_for_argument(arguments[2]);
            if (event && event.type == NSEventTypeApplicationDefined) {
                deliver_cross_thread_event(event, arguments[3] != 0);
                *result = 0;
                return 1;
            }
        }
        if (guest_event_wait) ++guest_event_wait_depth;
        bool invoked = invoke_simple_message(receiver, selector_name,
                                             arguments, result, NULL, NULL);
        if (guest_event_wait) --guest_event_wait_depth;
        if(getenv("LP32_TRACE_MEDIA") && !strcmp(selector_name,"status") &&
           [receiver isKindOfClass:[AVAssetReader class]]) {
            static unsigned traced;
            if(traced++<20 || *result!=1)
                fprintf(stderr,"compat32: movie status=%llu error=%s\n",(unsigned long long)*result,
                    [[[(AVAssetReader *)receiver error] description] UTF8String]);
        }
        if(getenv("LP32_TRACE_MEDIA") && !strcmp(selector_name,"startReading") &&
           [receiver isKindOfClass:[AVAssetReader class]])
            fprintf(stderr,"compat32: movie start=%llu error=%s outputs=%lu range=%.3f/%.3f\n",(unsigned long long)*result,
                [[[(AVAssetReader *)receiver error] description] UTF8String],
                (unsigned long)[[(AVAssetReader *)receiver outputs] count],
                CMTimeGetSeconds([(AVAssetReader *)receiver timeRange].start),CMTimeGetSeconds([(AVAssetReader *)receiver timeRange].duration));
        if (!invoked) {
            fprintf(stderr,
                    "compat32: Objective-C bridge cannot invoke %s on %s\n",
                    selector_name, class_getName(object_getClass(receiver)));
        }
        return invoked;
    }

    if (LP32_NAME_IS(import_name, import_length, "_objc_msgSend_stret")) {
        if (objc_legacy32_message_stret(arguments, result)) return 1;
        struct guest_rect *destination = (void *)(uintptr_t)arguments[0];
        id receiver = object_for_receiver(arguments[1]);
        const char *selector_name = (const char *)(uintptr_t)arguments[2];
        if (selector_name && strcmp(selector_name,"operatingSystemVersion")==0 && destination) {
            NSOperatingSystemVersion version=receiver?[(NSProcessInfo *)receiver operatingSystemVersion]:(NSOperatingSystemVersion){0,0,0};
            int32_t guest[]={(int32_t)version.majorVersion,(int32_t)version.minorVersion,(int32_t)version.patchVersion};
            memcpy(destination,guest,sizeof(guest));*result=0;return 1;
        }
        if (!arguments[1] && destination) {
            memset(destination, 0, sizeof(*destination));
            *result = 0;
            return 1;
        }
        if (!destination || !receiver || !selector_name) return 0;

        NSRect rect;
        if (strcmp(selector_name, "frame") == 0 ||
            strcmp(selector_name, "visibleFrame") == 0) {
            if (guest_display_mode_active() &&
                [receiver isKindOfClass:[NSScreen class]] &&
                display_id_for_screen(receiver) == guest_display_mode_display) {
                /* After a mode switch the old API reported the new mode as
                   the screen size; keep the guest's view of it consistent. */
                rect = [(NSScreen *)receiver frame];
                rect.size = guest_display_mode;
            } else if (strcmp(selector_name, "frame") == 0) {
                rect = [(NSScreen *)receiver frame];
            } else {
                rect = [(NSScreen *)receiver visibleFrame];
            }
        } else if (strcmp(selector_name,
                          "contentRectForFrameRect:styleMask:") == 0) {
            const float *guest_input = (const void *)(arguments + 3);
            NSRect input = NSMakeRect(guest_input[0], guest_input[1],
                                      guest_input[2], guest_input[3]);
            rect = [NSWindow contentRectForFrameRect:input
                                           styleMask:arguments[7]];
        } else {
            return invoke_simple_message(receiver, selector_name, arguments + 1,
                                          result, NULL, destination);
        }
        destination->x = (float)rect.origin.x;
        destination->y = (float)rect.origin.y;
        destination->width = (float)rect.size.width;
        destination->height = (float)rect.size.height;
        *result = 0;
        return 1;
    }

    return 0;
}

uint32_t objc_bridge32_pointer_import(const char *import_name)
{
    const size_t import_length = strlen(import_name);
    if (LP32_NAME_IS(import_name, import_length, "_AVMediaTypeAudio")) return proxy_for_object(AVMediaTypeAudio);
    if (LP32_NAME_IS(import_name, import_length, "_AVMediaTypeVideo")) return proxy_for_object(AVMediaTypeVideo);
    if (LP32_NAME_IS(import_name, import_length, "_kCVPixelBufferPixelFormatTypeKey")) return proxy_for_object((id)kCVPixelBufferPixelFormatTypeKey);
    if (LP32_NAME_IS(import_name, import_length, "_kCFAllocatorSystemDefault")) return proxy_for_object((id)kCFAllocatorSystemDefault);
    if (LP32_NAME_IS(import_name, import_length, "_kCFAllocatorNull")) return proxy_for_object((id)kCFAllocatorNull);
    uint32_t network_constant = cfnetwork_bridge32_pointer_import(import_name);
    if (network_constant) return network_constant;
    if (LP32_NAME_IS(import_name, import_length, "_kCFPreferencesAnyApplication")) return proxy_for_object((id)kCFPreferencesAnyApplication);
    if (LP32_NAME_IS(import_name, import_length, "_kCFPreferencesAnyHost")) return proxy_for_object((id)kCFPreferencesAnyHost);
    if (LP32_NAME_IS(import_name, import_length, "_kCFPreferencesAnyUser")) return proxy_for_object((id)kCFPreferencesAnyUser);
    if (LP32_NAME_IS(import_name, import_length, "_kCFPreferencesCurrentHost")) return proxy_for_object((id)kCFPreferencesCurrentHost);
    if (LP32_NAME_IS(import_name, import_length, "_kCFPreferencesCurrentUser")) return proxy_for_object((id)kCFPreferencesCurrentUser);
    if (LP32_NAME_IS(import_name, import_length, "_kCFPreferencesCurrentApplication")) return proxy_for_object((id)kCFPreferencesCurrentApplication);
    if (LP32_NAME_IS(import_name, import_length, "_kCFBooleanTrue")) return proxy_for_object((id)kCFBooleanTrue);
    if (LP32_NAME_IS(import_name, import_length, "_kCFBooleanFalse")) return proxy_for_object((id)kCFBooleanFalse);
    if (LP32_NAME_IS(import_name, import_length, "_NSFilePosixPermissions")) return proxy_for_object((id)NSFilePosixPermissions);
    if (LP32_NAME_IS(import_name, import_length, "_NSLocaleCountryCode")) return proxy_for_object((id)NSLocaleCountryCode);
    if (LP32_NAME_IS(import_name, import_length, "_NSLocalizedDescriptionKey")) return proxy_for_object((id)NSLocalizedDescriptionKey);
    if (LP32_NAME_IS(import_name, import_length, "_NSPOSIXErrorDomain")) return proxy_for_object((id)NSPOSIXErrorDomain);
    if (LP32_NAME_IS(import_name, import_length, "_NSPasteboardTypeString")) return proxy_for_object((id)NSPasteboardTypeString);
    if (LP32_NAME_IS(import_name, import_length, "_kCTForegroundColorFromContextAttributeName")) return proxy_for_object((id)kCTForegroundColorFromContextAttributeName);
    if (LP32_NAME_IS(import_name, import_length, "_NSRunLoopCommonModes")) return proxy_for_object((id)NSRunLoopCommonModes);
    if (LP32_NAME_IS(import_name, import_length, "_NSUnderlyingErrorKey")) return proxy_for_object((id)NSUnderlyingErrorKey);
    if (LP32_NAME_IS(import_name, import_length, "_NSWindowDidBecomeMainNotification")) return proxy_for_object((id)NSWindowDidBecomeMainNotification);
    if (LP32_NAME_IS(import_name, import_length, "_NSWindowDidDeminiaturizeNotification")) return proxy_for_object((id)NSWindowDidDeminiaturizeNotification);
    if (LP32_NAME_IS(import_name, import_length, "_NSWindowDidEnterFullScreenNotification")) return proxy_for_object((id)NSWindowDidEnterFullScreenNotification);
    if (LP32_NAME_IS(import_name, import_length, "_NSWindowDidExitFullScreenNotification")) return proxy_for_object((id)NSWindowDidExitFullScreenNotification);
    if (LP32_NAME_IS(import_name, import_length, "_NSWindowDidMiniaturizeNotification")) return proxy_for_object((id)NSWindowDidMiniaturizeNotification);
    if (LP32_NAME_IS(import_name, import_length, "_NSWindowDidResignMainNotification")) return proxy_for_object((id)NSWindowDidResignMainNotification);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (LP32_NAME_IS(import_name, import_length, "_NSWorkspaceLaunchConfigurationArguments")) return proxy_for_object((id)NSWorkspaceLaunchConfigurationArguments);
#pragma clang diagnostic pop
    if (LP32_NAME_IS(import_name, import_length, "_kTISNotifySelectedKeyboardInputSourceChanged")) return proxy_for_object((id)kTISNotifySelectedKeyboardInputSourceChanged);
    if (LP32_NAME_IS(import_name, import_length, "_kTISPropertyInputSourceID")) return proxy_for_object((id)kTISPropertyInputSourceID);
    if (LP32_NAME_IS(import_name, import_length, "_NSApp")) {
        Class application = objc_legacy32_application_class();
        return proxy_for_object([application sharedApplication]);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kCFLocaleCountryCode")) {
        return proxy_for_object((NSString *)kCFLocaleCountryCode);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kCFLocaleLanguageCode")) {
        return proxy_for_object((NSString *)kCFLocaleLanguageCode);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kCFRunLoopCommonModes")) return proxy_for_object((id)kCFRunLoopCommonModes);
    if (LP32_NAME_IS(import_name, import_length, "_kCFRunLoopDefaultMode")) {
        return proxy_for_object((NSString *)kCFRunLoopDefaultMode);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kTISPropertyInputSourceLanguages")) {
        return proxy_for_object((NSString *)kTISPropertyInputSourceLanguages);
    }
    if (LP32_NAME_IS(import_name, import_length, "_kTISPropertyUnicodeKeyLayoutData")) {
        return proxy_for_object((NSString *)kTISPropertyUnicodeKeyLayoutData);
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSControlTextDidChangeNotification")) {
        return proxy_for_object(NSControlTextDidChangeNotification);
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSDefaultRunLoopMode")) {
        return proxy_for_object(NSDefaultRunLoopMode);
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSLinkAttributeName")) {
        return proxy_for_object(NSLinkAttributeName);
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSViewGlobalFrameDidChangeNotification")) {
        return proxy_for_object(@"NSViewGlobalFrameDidChangeNotification");
    }
    if (LP32_NAME_IS(import_name, import_length, "_NSWindowWillCloseNotification")) {
        return proxy_for_object(NSWindowWillCloseNotification);
    }
    /* These linked Core Foundation data imports are object references.  The
       two numeric time intervals and callback tables use separate guest cells. */
    if (!strncmp(import_name, "_kCFBundle", 10) ||
        !strncmp(import_name, "_kCFGregorianCalendar", 21) ||
        !strncmp(import_name, "_kCFHTTPVersion", 15) ||
        !strncmp(import_name, "_kCFLocaleCurrency", 18) ||
        !strcmp(import_name, "_kCFNull") ||
        !strncmp(import_name, "_kCFNumberFormatter", 19) ||
        !strncmp(import_name, "_kCFProxy", 9) ||
        !strncmp(import_name, "_kCFStream", 10) ||
        !strncmp(import_name, "_kCFStringTransform", 19)) {
        const id *symbol = dlsym(RTLD_DEFAULT, import_name + 1);
        return symbol ? proxy_for_object(*symbol) : 0;
    }
    return 0;
}

int objc_bridge32_run_gl_parameter_self_test(void)
{
    if (split_program_parameter_batches()) {
        fputs("gl-parameter-selftest: default batch policy is split\n",
              stderr);
        return -1;
    }
    const GLfloat finite_values[] = {
        1.0f, -2.0f, 3.5f, 0.0f,
        4.0f, 5.0f, 6.0f, 7.0f,
    };
    size_t replaced = SIZE_MAX;
    const GLfloat *safe = safe_program_parameter_values(
        finite_values, 2, &replaced);
    if (safe != finite_values || replaced != 0) {
        fputs("gl-parameter-selftest: finite fast path failed\n", stderr);
        return -1;
    }

    const GLfloat nonfinite_values[] = {
        1.0f, NAN, 3.5f, INFINITY,
        -INFINITY, 5.0f, 6.0f, 7.0f,
    };
    safe = safe_program_parameter_values(nonfinite_values, 2, &replaced);
    if (safe == nonfinite_values || replaced != 3 ||
        safe[0] != 1.0f || safe[1] != 0.0f || safe[2] != 3.5f ||
        safe[3] != 0.0f || safe[4] != 0.0f || safe[5] != 5.0f ||
        safe[6] != 6.0f || safe[7] != 7.0f) {
        fputs("gl-parameter-selftest: non-finite containment failed\n",
              stderr);
        return -1;
    }
    if (!isnan(nonfinite_values[1]) || !isinf(nonfinite_values[3]) ||
        !isinf(nonfinite_values[4])) {
        fputs("gl-parameter-selftest: modified source values\n", stderr);
        return -1;
    }

    puts("gl-parameter-selftest: PASS (atomic batch policy and finite containment)");
    return 0;
}

int objc_bridge32_run_date_formatter_self_test(void)
{
    @autoreleasepool {
        uint64_t result;
        uint32_t create[] = {0, objc_bridge32_guest_object(
            [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]),
            kCFDateFormatterShortStyle, kCFDateFormatterShortStyle};
        if (!objc_bridge32_dispatch("_CFDateFormatterCreate", create, &result) ||
            !result) return -1;
        uint32_t formatter = (uint32_t)result;
        double seconds = 123456.0;
        uint32_t absolute[] = {0, formatter, 0, 0};
        memcpy(absolute + 2, &seconds, sizeof(seconds));
        if (!objc_bridge32_dispatch("_CFDateFormatterCreateStringWithAbsoluteTime",
                                    absolute, &result) || !result) return -1;
        NSString *first = objc_bridge32_host_object((uint32_t)result);
        uint32_t date[] = {0, formatter, objc_bridge32_guest_object(
            [NSDate dateWithTimeIntervalSinceReferenceDate:seconds])};
        if (!objc_bridge32_dispatch("_CFDateFormatterCreateStringWithDate",
                                    date, &result) || !result) return -1;
        NSString *second = objc_bridge32_host_object((uint32_t)result);
        if (![first length] || ![first isEqualToString:second]) return -1;
        uint32_t release[] = {formatter};
        if (!objc_bridge32_dispatch("_CFRelease", release, &result)) return -1;

        uint32_t guest = compat_runtime32_allocate(64, 1);
        if (!guest) return -1;
        uint32_t property = objc_bridge32_pointer_import(
            "_kCFNumberFormatterMaxFractionDigits");
        uint32_t calendar_id = objc_bridge32_pointer_import("_kCFGregorianCalendar");
        if (!property || !calendar_id) return -1;
        const char *linked_object_constants[] = {
            "_kCFBundleExecutableKey", "_kCFBundleIdentifierKey",
            "_kCFBundleNameKey", "_kCFBundleVersionKey",
            "_kCFGregorianCalendar", "_kCFHTTPVersion1_1",
            "_kCFLocaleCurrencyCode", "_kCFNull",
            "_kCFNumberFormatterCurrencyCode",
            "_kCFNumberFormatterMaxFractionDigits",
            "_kCFProxyAutoConfigurationURLKey", "_kCFProxyHostNameKey",
            "_kCFProxyPortNumberKey", "_kCFProxyTypeAutoConfigurationURL",
            "_kCFProxyTypeHTTP", "_kCFProxyTypeKey",
            "_kCFStreamPropertyAppendToFile", "_kCFStreamPropertyHTTPProxy",
            "_kCFStreamPropertyHTTPProxyHost", "_kCFStreamPropertyHTTPProxyPort",
            "_kCFStreamPropertyHTTPResponseHeader", "_kCFStreamPropertySSLSettings",
            "_kCFStreamSSLIsServer", "_kCFStreamSSLLevel",
            "_kCFStreamSSLPeerName", "_kCFStreamSSLValidatesCertificateChain",
            "_kCFStreamSocketSecurityLevelNegotiatedSSL",
            "_kCFStringTransformStripDiacritics",
        };
        for (unsigned i = 0; i < sizeof(linked_object_constants) /
                               sizeof(linked_object_constants[0]); ++i) {
            if (!objc_bridge32_pointer_import(linked_object_constants[i])) {
                fprintf(stderr, "missing Core Foundation constant: %s\n",
                        linked_object_constants[i]);
                return -1;
            }
        }
        uint32_t number_create[] = {0, create[1], kCFNumberFormatterDecimalStyle};
        if (!objc_bridge32_dispatch("_CFNumberFormatterCreate", number_create,
                                    &result) || !result) return -1;
        uint32_t number_formatter = (uint32_t)result;
        uint32_t set_property[] = {number_formatter, property,
                                   objc_bridge32_guest_object(@2)};
        if (!objc_bridge32_dispatch("_CFNumberFormatterSetProperty", set_property,
                                    &result)) return -1;
        *(int32_t *)(uintptr_t)guest = 12345;
        uint32_t number_value[] = {0, number_formatter, kCFNumberSInt32Type, guest};
        if (!objc_bridge32_dispatch("_CFNumberFormatterCreateStringWithValue",
                                    number_value, &result) || !result ||
            ![(NSString *)objc_bridge32_host_object((uint32_t)result) length]) return -1;
        uint32_t calendar_create[] = {0, calendar_id};
        if (!objc_bridge32_dispatch("_CFCalendarCreateWithIdentifier",
                                    calendar_create, &result) || !result) return -1;
        uint32_t calendar = (uint32_t)result;
        uint32_t time_zone[] = {calendar, objc_bridge32_guest_object(
            [NSTimeZone timeZoneForSecondsFromGMT:0])};
        if (!objc_bridge32_dispatch("_CFCalendarSetTimeZone", time_zone, &result))
            return -1;
        memcpy((void *)(uintptr_t)(guest + 16), "yMdHms", 7);
        uint32_t compose[] = {calendar, guest + 8, guest + 16,
                              2002, 1, 2, 3, 4, 5};
        if (!objc_bridge32_dispatch("_CFCalendarComposeAbsoluteTime", compose,
                                    &result) || !result ||
            !isfinite(*(double *)(uintptr_t)(guest + 8))) return -1;
        uint32_t dates[] = {
            objc_bridge32_guest_object([NSDate dateWithTimeIntervalSinceReferenceDate:1]),
            objc_bridge32_guest_object([NSDate dateWithTimeIntervalSinceReferenceDate:2]),
            0};
        if (!objc_bridge32_dispatch("_CFDateCompare", dates, &result) ||
            (int32_t)result != kCFCompareLessThan) return -1;
        uint32_t resource[] = {
            0, objc_bridge32_guest_object([NSURL fileURLWithPath:@"/etc/hosts"]),
            guest + 24, 0, 0, guest + 28};
        if (!objc_bridge32_dispatch("_CFURLCreateDataAndPropertiesFromResource",
                                    resource, &result) || !result) return -1;
        uint32_t data_token = *(uint32_t *)(uintptr_t)(guest + 24);
        if (!data_token || ![(NSData *)objc_bridge32_host_object(data_token) length])
            return -1;
        release[0] = data_token;
        if (!objc_bridge32_dispatch("_CFRelease", release, &result)) return -1;
        release[0] = number_formatter;
        if (!objc_bridge32_dispatch("_CFRelease", release, &result)) return -1;
        release[0] = calendar;
        if (!objc_bridge32_dispatch("_CFRelease", release, &result)) return -1;
        compat_runtime32_deallocate(guest);
        puts("Core Foundation bridge PASS (formatting, calendar, file resources, constants)");
        return 0;
    }
}

static void *signal_test_semaphore(void *raw)
{
    usleep(10000);
    dispatch_semaphore_signal((dispatch_semaphore_t)raw);
    return NULL;
}
int objc_bridge32_run_dispatch_fast_self_test(void)
{
    @autoreleasepool {
        const char *names[] = {"dispatch_time", "dispatch_semaphore_wait", "dispatch_semaphore_signal"};
        for (unsigned i=0;i<3;++i) {
            char decorated[64];snprintf(decorated,sizeof(decorated),"_%s",names[i]);
            if (!objc_bridge32_fast_import(names[i]) || objc_bridge32_fast_import(names[i]) != objc_bridge32_fast_import(decorated)) return -1;
        }
        uint64_t whens[] = {UINT64_C(100000000000), DISPATCH_TIME_FOREVER};
        int64_t deltas[] = {0, -1000000000, 1000000000, INT64_MAX, INT64_MIN};
        for (unsigned i=0;i<2;++i) for(unsigned j=0;j<5;++j) {
            uint32_t a[4];memcpy(a,&whens[i],8);memcpy(a+2,&deltas[j],8);
            uint64_t expected=dispatch_time(whens[i],deltas[j]), result;
            if (fast_dispatch_time(a,0)!=expected || !objc_bridge32_dispatch("_dispatch_time",a,&result) || result!=expected) return -1;
        }
        dispatch_semaphore_t semaphore=dispatch_semaphore_create(0);
        uint32_t token=proxy_for_object((id)semaphore), a[]={token,0,0};
        if (!token) return -1;
        if (!fast_dispatch_semaphore_wait(a,0)) return -1; /* Poll must time out. */
        uint64_t result;
        if (!objc_bridge32_dispatch("_dispatch_semaphore_signal",a,&result) || fast_dispatch_semaphore_wait(a,0)) return -1;
        fast_dispatch_semaphore_signal(a,0);
        if (!objc_bridge32_dispatch("_dispatch_semaphore_wait",a,&result) || result) return -1;
        uint64_t expired=dispatch_time(DISPATCH_TIME_NOW,-1000000);memcpy(a+1,&expired,8);
        if(!fast_dispatch_semaphore_wait(a,0))return -1;
        fast_dispatch_semaphore_signal(a,0);
        if(fast_dispatch_semaphore_wait(a,0))return -1; /* Still consume available signals. */
        uint64_t deadline=dispatch_time(DISPATCH_TIME_NOW,2000000);memcpy(a+1,&deadline,8);
        if (!fast_dispatch_semaphore_wait(a,0)) return -1;
        pthread_t worker; if(pthread_create(&worker,NULL,signal_test_semaphore,semaphore))return -1;
        deadline=dispatch_time(DISPATCH_TIME_NOW,1000000000);memcpy(a+1,&deadline,8);
        if (fast_dispatch_semaphore_wait(a,0) || pthread_join(worker,NULL))return -1;
        deadline=DISPATCH_TIME_FOREVER;memcpy(a+1,&deadline,8);
        fast_dispatch_semaphore_signal(a,0);if(fast_dispatch_semaphore_wait(a,0))return -1;
        uint32_t clock_args[]={0,0,0,0};
        for(unsigned pass=0;pass<4;++pass) {
            uint64_t start=hitch_now();
            for(unsigned i=0;i<100000;++i) {
                if(pass%2)result=fast_dispatch_time(clock_args,0);
                else if(!objc_bridge32_dispatch("_dispatch_time",clock_args,&result))return -1;
            }
            fprintf(stderr,"dispatch-fast-selftest: timeout %s %.1f ns/call\n",pass%2?"direct":"chained",(hitch_now()-start)/100000.0);
        }
        dispatch_release(semaphore);
        puts("dispatch-fast-selftest: PASS (aliases, 64-bit signed deadlines, overflow, polling, timeout, worker wakeup, queued signal)");
        return 0;
    }
}

static void *pointer_proxy_test_worker(void *pointer)
{
    uint32_t token = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        uint32_t next = objc_bridge32_guest_pointer(pointer);
        if (!next || (token && next != token)) return NULL;
        token = next;
    }
    return (void *)(uintptr_t)token;
}

int objc_bridge32_run_pointer_self_test(void)
{
    @autoreleasepool {
        if (objc_bridge32_guest_pointer(NULL)) return -1;
        void *pointer = (void *)UINT64_C(0x123456789abcdef0);
        pthread_t workers[4];
        uint32_t before = proxy_count, token = 0;
        for (unsigned i = 0; i < 4; ++i)
            if (pthread_create(&workers[i], NULL, pointer_proxy_test_worker, pointer)) return -1;
        for (unsigned i = 0; i < 4; ++i) {
            void *value;
            if (pthread_join(workers[i], &value) || !value) return -1;
            if (token && token != (uint32_t)(uintptr_t)value) return -1;
            token = (uint32_t)(uintptr_t)value;
        }
        if (proxy_count != before + 1 || [(NSValue *)object_for_receiver(token) pointerValue] != pointer) return -1;
        /* Different addresses remain distinct, even after hash collisions. */
        for (uintptr_t i = 1; i <= 2048; ++i) {
            void *p = (void *)(UINT64_C(0x700000000000) + i * 4096);
            uint32_t t = objc_bridge32_guest_pointer(p);
            if (!t || t == token || [(NSValue *)object_for_receiver(t) pointerValue] != p) return -1;
        }
        before = proxy_count;
        for (uintptr_t i = 1; i <= 2048; ++i)
            if (!objc_bridge32_guest_pointer((void *)(UINT64_C(0x700000000000) + i * 4096))) return -1;
        if (proxy_count != before) return -1;
        uint64_t ignored;
        objc_bridge32_dispatch("_CFRetain", &token, &ignored);
        objc_bridge32_dispatch("_CFRelease", &token, &ignored);
        if (objc_bridge32_guest_pointer(pointer) != token || [(NSValue *)object_for_receiver(token) pointerValue] != pointer) return -1;

        NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFAAccelerated, 0};
        NSOpenGLPixelFormat *format = [[[NSOpenGLPixelFormat alloc] initWithAttributes:attrs] autorelease];
        NSOpenGLContext *first = [[[NSOpenGLContext alloc] initWithFormat:format shareContext:nil] autorelease];
        NSOpenGLContext *second = [[[NSOpenGLContext alloc] initWithFormat:format shareContext:first] autorelease];
        if (!first || !second) return -1;
        CGLContextObj contexts[] = {first.CGLContextObj, second.CGLContextObj, NULL, first.CGLContextObj};
        lp32_fast_import_fn fast = objc_bridge32_fast_import("_CGLGetCurrentContext");
        if (!fast || fast != objc_bridge32_fast_import("CGLGetCurrentContext")) return -1;
        uint32_t args[] = {0, objc_bridge32_guest_selector("CGLContextObj")};
        uint32_t first_token = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (CGLSetCurrentContext(contexts[i])) return -1;
            uint64_t value, direct = fast(NULL, 0);
            if (!objc_bridge32_dispatch("_CGLGetCurrentContext", NULL, &value) || value != direct) return -1;
            if (!contexts[i]) { if (direct) return -1; continue; }
            if (i == 0) first_token = (uint32_t)direct;
            if ((i == 1 && direct == first_token) || (i == 3 && direct != first_token)) return -1;
            args[0] = proxy_for_object(i == 1 ? second : first);
            if (!objc_bridge32_dispatch("_objc_msgSend", args, &value) || value != direct) return -1;
            if ([(NSValue *)object_for_receiver((uint32_t)value) pointerValue] != contexts[i]) return -1;
        }
        /* Setting through the fast path switches contexts, and restating the
           current context leaves it current. */
        lp32_fast_import_fn set_fast = objc_bridge32_fast_import("_CGLSetCurrentContext");
        uint32_t second_token = objc_bridge32_guest_pointer(contexts[1]);
        if (!set_fast || !second_token) return -1;
        for (unsigned pass = 0; pass < 2; ++pass) {
            uint32_t set_args[] = {pass ? first_token : second_token};
            if (set_fast(set_args, 0) != kCGLNoError ||
                set_fast(set_args, 0) != kCGLNoError ||
                CGLGetCurrentContext() != contexts[pass ? 0 : 1]) return -1;
        }
        uint32_t clear_args[] = {0};
        if (set_fast(clear_args, 0) != kCGLNoError || CGLGetCurrentContext()) return -1;
        if (CGLSetCurrentContext(contexts[0])) return -1;
        GLuint refs = CGLGetContextRetainCount(contexts[0]);
        before = proxy_count;
        uint64_t start = hitch_now();
        for (unsigned i = 0; i < 100000; ++i)
            if (fast(NULL, 0) != first_token) return -1;
        fprintf(stderr, "pointer-selftest: interned current-context %.1f ns/call, %u new proxies/100000 calls\n", (hitch_now() - start) / 100000.0, proxy_count - before);
        if (proxy_count != before || CGLGetContextRetainCount(contexts[0]) != refs) return -1;
        before = proxy_count; start = hitch_now();
        for (unsigned i = 0; i < 1000; ++i) {
            @autoreleasepool { if (!proxy_for_object([NSValue valueWithPointer:CGLGetCurrentContext()])) return -1; }
        }
        fprintf(stderr, "pointer-selftest: previous boxing %.1f ns/call, %u new proxies/1000 calls\n", (hitch_now() - start) / 1000.0, proxy_count - before);
        CGLSetCurrentContext(NULL);
        /* Destroying/recreating contexts must not retain resources or return a
         * different raw address if the driver's allocator reuses an address. */
        for (unsigned i = 0; i < 128; ++i) {
            CGLContextObj temporary;
            if (CGLCreateContext(format.CGLPixelFormatObj, NULL, &temporary)) return -1;
            GLuint retain_count = CGLGetContextRetainCount(temporary);
            uint32_t t = objc_bridge32_guest_pointer(temporary);
            if ([(NSValue *)object_for_receiver(t) pointerValue] != temporary || CGLGetContextRetainCount(temporary) != retain_count) return -1;
            CGLDestroyContext(temporary);
        }
        puts("pointer-selftest: PASS (concurrent interning, distinct addresses, ownership, context switches, direct/chained/ObjC queries, bounded repetition, resource lifetime/reuse)");
        return 0;
    }
}

int objc_bridge32_run_gl_buffer_self_test(void)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLPixelFormatAttribute attributes[] = {
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj format = NULL;
    GLint format_count = 0;
    CGLContextObj context = NULL;
    CGLContextObj shared_context = NULL;
    CGLError error = CGLChoosePixelFormat(attributes, &format, &format_count);
    if (error != kCGLNoError || !format || format_count < 1) {
        fprintf(stderr, "gl-buffer-selftest: pixel format failed: %d\n", error);
        if (format) CGLDestroyPixelFormat(format);
        return -1;
    }
    error = CGLCreateContext(format, NULL, &context);
    if (error == kCGLNoError && context) {
        error = CGLCreateContext(format, context, &shared_context);
    }
    CGLDestroyPixelFormat(format);
    if (error != kCGLNoError || !context || !shared_context) {
        fprintf(stderr, "gl-buffer-selftest: context creation failed: %d\n",
                error);
        if (shared_context) CGLDestroyContext(shared_context);
        if (context) CGLDestroyContext(context);
        return -1;
    }
    error = CGLSetCurrentContext(context);
#pragma clang diagnostic pop
    if (error != kCGLNoError) {
        fprintf(stderr, "gl-buffer-selftest: make-current failed: %d\n", error);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGLDestroyContext(shared_context);
        CGLDestroyContext(context);
#pragma clang diagnostic pop
        return -1;
    }

    uint32_t context_callback = compat_runtime32_guest_callback("_CGLGetCurrentContext");
    uint32_t context_token = objc_bridge32_guest_pointer(context);
    if (!context_callback || !context_token) return -1;
    for (unsigned i = 0; i < 16; ++i)
        if (compat_runtime32_call(context_callback, NULL, 0) != context_token) return -1;

    enum { kTestSize = 32 };
    unsigned char initial_a[kTestSize];
    unsigned char initial_b[kTestSize];
    unsigned char actual[kTestSize];
    memset(initial_a, 0x11, sizeof(initial_a));
    memset(initial_b, 0x22, sizeof(initial_b));
    GLuint buffers[2] = {0, 0};
    glGenBuffers(2, buffers);
    uint64_t dispatch_result = 0;
    int status = 0;

    /* Both symbol spellings must use identical handlers, including names
       reached through dlsym and through a Mach-O import stub. */
    const char *fast_names[] = {"CGLGetCurrentContext", "glBindBuffer", "glBindBufferARB", "glDrawArrays",
        "glDrawRangeElements", "glVertexAttribPointer", "glBindTexture",
        "glProgramEnvParameters4fvEXT", "glDepthFunc"};
    for (unsigned i = 0; i < sizeof(fast_names) / sizeof(fast_names[0]); ++i) {
        char decorated[128]; snprintf(decorated, sizeof(decorated), "_%s", fast_names[i]);
        lp32_fast_import_fn plain = objc_bridge32_fast_import(fast_names[i]);
        if (!plain || plain != objc_bridge32_fast_import(decorated)) {
            fprintf(stderr, "gl-buffer-selftest: missing fast alias %s\n", decorated);
            status = -1; goto cleanup;
        }
    }
    const uint32_t bind_args[] = {GL_ARRAY_BUFFER, buffers[0]};
    lp32_fast_import_fn bind = objc_bridge32_fast_import("_glBindBuffer");
    bind(bind_args, 0);
    GLint bound = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound);
    if ((GLuint)bound != buffers[0] || glGetError() != GL_NO_ERROR) {
        fputs("gl-buffer-selftest: fast binding did not reach GL\n", stderr);
        status = -1; goto cleanup;
    }
    /* Controlled dispatch benchmark; no game, presentation, or audio. */
    uint64_t before = hitch_now();
    for (unsigned i = 0; i < 100000; ++i)
        objc_bridge32_dispatch("_glBindBuffer", bind_args, &dispatch_result);
    double chained_ns = (hitch_now() - before) / 100000.0;
    before = hitch_now();
    for (unsigned i = 0; i < 100000; ++i) bind(bind_args, 0);
    double fast_ns = (hitch_now() - before) / 100000.0;
    fprintf(stderr, "gl-buffer-selftest: bind dispatch chained=%.0f ns fast=%.0f ns\n",
            chained_ns, fast_ns);

    uint32_t extensions = guest_gl_string(GL_EXTENSIONS);
    if (!extensions) {
        fputs("gl-buffer-selftest: extension string is missing\n", stderr);
        status = -1;
        goto cleanup;
    }
    for (unsigned query = 0; query < 10000; ++query) {
        if (guest_gl_string(GL_EXTENSIONS) != extensions) {
            fputs("gl-buffer-selftest: extension query leaks guest storage\n", stderr);
            status = -1;
            goto cleanup;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, kTestSize, initial_a, GL_DYNAMIC_DRAW);
    const uint32_t disable_flush[] = {
        GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_FALSE,
    };
    objc_bridge32_dispatch("glBufferParameteriAPPLE", disable_flush,
                           &dispatch_result);
    const uint32_t map_arguments[] = {GL_ARRAY_BUFFER, GL_WRITE_ONLY};
    objc_bridge32_dispatch("glMapBuffer", map_arguments, &dispatch_result);
    uint32_t first_mapping = (uint32_t)dispatch_result;
    if (!first_mapping) {
        fputs("gl-buffer-selftest: first map failed\n", stderr);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)first_mapping, 0xa1, kTestSize);
    const uint32_t flush_arguments[] = {GL_ARRAY_BUFFER, 8, 8};
    objc_bridge32_dispatch("glFlushMappedBufferRangeAPPLE", flush_arguments,
                           &dispatch_result);
    const uint32_t unmap_arguments[] = {GL_ARRAY_BUFFER};
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        unsigned char expected = index >= 8 && index < 16 ? 0xa1 : 0x11;
        if (actual[index] != expected) {
            fprintf(stderr,
                    "gl-buffer-selftest: explicit range mismatch at %zu: "
                    "%02x != %02x\n",
                    index, actual[index], expected);
            status = -1;
            goto cleanup;
        }
    }

    /* Per-object state must follow the VBO into a shared loading context. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    error = CGLSetCurrentContext(shared_context);
#pragma clang diagnostic pop
    if (error != kCGLNoError) {
        fprintf(stderr,
                "gl-buffer-selftest: shared make-current failed: %d\n", error);
        status = -1;
        goto cleanup;
    }
    if (guest_gl_string(GL_EXTENSIONS) != extensions) {
        fputs("gl-buffer-selftest: shared context duplicated extension storage\n", stderr);
        status = -1;
        goto cleanup;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    objc_bridge32_dispatch("glMapBuffer", map_arguments, &dispatch_result);
    if (!dispatch_result) {
        fputs("gl-buffer-selftest: shared-context map failed\n", stderr);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)(uint32_t)dispatch_result, 0xc3, kTestSize);
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        unsigned char expected = index >= 8 && index < 16 ? 0xa1 : 0x11;
        if (actual[index] != expected) {
            fprintf(stderr,
                    "gl-buffer-selftest: shared policy mismatch at %zu: "
                    "%02x != %02x\n",
                    index, actual[index], expected);
            status = -1;
            goto cleanup;
        }
    }

    /* A second buffer on the same target must retain the default TRUE. */
    glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ARRAY_BUFFER, kTestSize, initial_b, GL_DYNAMIC_DRAW);
    objc_bridge32_dispatch("glMapBuffer", map_arguments, &dispatch_result);
    uint32_t second_mapping = (uint32_t)dispatch_result;
    if (!second_mapping) {
        fprintf(stderr,
                "gl-buffer-selftest: second mapping failed: %08x\n",
                second_mapping);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)second_mapping, 0xb2, kTestSize);
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        if (actual[index] != 0xb2) {
            fprintf(stderr,
                    "gl-buffer-selftest: default unmap mismatch at %zu: %02x\n",
                    index, actual[index]);
            status = -1;
            goto cleanup;
        }
    }

    /* Core map-range flush offsets are relative to the mapped window. */
    glBufferData(GL_ARRAY_BUFFER, kTestSize, initial_b, GL_DYNAMIC_DRAW);
    const uint32_t map_range_arguments[] = {
        GL_ARRAY_BUFFER, 4, 16,
        GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT,
    };
    objc_bridge32_dispatch("glMapBufferRange", map_range_arguments,
                           &dispatch_result);
    uint32_t range_mapping = (uint32_t)dispatch_result;
    if (!range_mapping) {
        fputs("gl-buffer-selftest: explicit range map failed\n", stderr);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)range_mapping, 0xd4, 16);
    const uint32_t range_flush_arguments[] = {GL_ARRAY_BUFFER, 3, 5};
    objc_bridge32_dispatch("glFlushMappedBufferRange", range_flush_arguments,
                           &dispatch_result);
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        unsigned char expected = index >= 7 && index < 12 ? 0xd4 : 0x22;
        if (actual[index] != expected) {
            fprintf(stderr,
                    "gl-buffer-selftest: relative range mismatch at %zu: "
                    "%02x != %02x\n",
                    index, actual[index], expected);
            status = -1;
            goto cleanup;
        }
    }

    /* A non-explicit writable range is committed in full on unmap. */
    const uint32_t implicit_range_arguments[] = {
        GL_ARRAY_BUFFER, 12, 8,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT,
    };
    objc_bridge32_dispatch("glMapBufferRange", implicit_range_arguments,
                           &dispatch_result);
    range_mapping = (uint32_t)dispatch_result;
    if (!range_mapping) {
        fputs("gl-buffer-selftest: implicit range map failed\n", stderr);
        status = -1;
        goto cleanup;
    }
    memset((void *)(uintptr_t)range_mapping, 0xe5, 8);
    objc_bridge32_dispatch("glUnmapBuffer", unmap_arguments, &dispatch_result);
    memset(actual, 0, sizeof(actual));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, kTestSize, actual);
    for (size_t index = 0; index < kTestSize; ++index) {
        unsigned char expected = index >= 12 && index < 20 ? 0xe5 :
            (index >= 7 && index < 12 ? 0xd4 : 0x22);
        if (actual[index] != expected) {
            fprintf(stderr,
                    "gl-buffer-selftest: implicit range mismatch at %zu: "
                    "%02x != %02x\n",
                    index, actual[index], expected);
            status = -1;
            goto cleanup;
        }
    }

cleanup:
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGLSetCurrentContext(context);
#pragma clang diagnostic pop
        uint32_t guest_buffers = compat_runtime32_allocate(sizeof(buffers), 0);
        if (guest_buffers) {
            memcpy((void *)(uintptr_t)guest_buffers, buffers, sizeof(buffers));
            const uint32_t delete_arguments[] = {2, guest_buffers};
            objc_bridge32_dispatch("glDeleteBuffers", delete_arguments,
                                   &dispatch_result);
            compat_runtime32_deallocate(guest_buffers);
        } else {
            glDeleteBuffers(2, buffers);
            status = -1;
        }
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(shared_context);
    CGLDestroyContext(context);
#pragma clang diagnostic pop
    if (status == 0) {
        puts("gl-buffer-selftest: PASS (per-object Apple and core mapped-range staging)");
    }
    return status;
}

static size_t compressed_texture_bytes(GLsizei width, GLsizei height,
                                       size_t block_bytes)
{
    size_t blocks_wide = (size_t)(width > 4 ? width : 4) / 4;
    size_t blocks_high = (size_t)(height > 4 ? height : 4) / 4;
    return blocks_wide * blocks_high * block_bytes;
}

static void fill_compressed_texture_pattern(unsigned char *bytes, size_t size,
                                            unsigned int seed)
{
    uint32_t value = UINT32_C(0x9e3779b9) ^ seed;
    for (size_t index = 0; index < size; ++index) {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        bytes[index] = (unsigned char)(value >> 24);
    }
}

static int run_compressed_texture_case(CGLContextObj upload_context,
                                       CGLContextObj render_context,
                                       GLenum format, size_t block_bytes,
                                       GLsizei width, GLsizei height,
                                       bool sliced, unsigned int seed)
{
    size_t byte_count = compressed_texture_bytes(width, height, block_bytes);
    unsigned char *expected = malloc(byte_count);
    unsigned char *actual = malloc(byte_count);
    unsigned char *decoded = malloc((size_t)width * (size_t)height * 4);
    uint32_t guest_bytes = compat_runtime32_allocate(byte_count, 0);
    if (!expected || !actual || !decoded || !guest_bytes) {
        fputs("gl-texture-selftest: allocation failed\n", stderr);
        free(decoded);
        free(actual);
        free(expected);
        if (guest_bytes) compat_runtime32_deallocate(guest_bytes);
        return -1;
    }
    fill_compressed_texture_pattern(expected, byte_count, seed);
    memcpy((void *)(uintptr_t)guest_bytes, expected, byte_count);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLError error = CGLSetCurrentContext(upload_context);
#pragma clang diagnostic pop
    if (error != kCGLNoError) {
        fprintf(stderr, "gl-texture-selftest: upload make-current failed: %d\n",
                error);
        compat_runtime32_deallocate(guest_bytes);
        free(decoded);
        free(actual);
        free(expected);
        return -1;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    uint64_t dispatch_result = 0;
    int status = 0;
    if (!sliced) {
        const uint32_t arguments[] = {
            GL_TEXTURE_2D, 0, format, (uint32_t)width, (uint32_t)height, 0,
            (uint32_t)byte_count, guest_bytes,
        };
        objc_bridge32_dispatch("glCompressedTexImage2D", arguments,
                               &dispatch_result);
    } else {
        const uint32_t allocate_arguments[] = {
            GL_TEXTURE_2D, 0, format, (uint32_t)width, (uint32_t)height, 0,
            (uint32_t)byte_count, 0,
        };
        objc_bridge32_dispatch("glCompressedTexImage2D", allocate_arguments,
                               &dispatch_result);
        GLsizei y = 0;
        size_t source_offset = 0;
        while (y < height) {
            GLsizei rows = height - y > 256 ? 256 : height - y;
            size_t slice_bytes = compressed_texture_bytes(
                width, rows, block_bytes);
            const uint32_t update_arguments[] = {
                GL_TEXTURE_2D, 0, 0, (uint32_t)y,
                (uint32_t)width, (uint32_t)rows, format,
                (uint32_t)slice_bytes, guest_bytes + (uint32_t)source_offset,
            };
            objc_bridge32_dispatch("glCompressedTexSubImage2D",
                                   update_arguments, &dispatch_result);
            source_offset += slice_bytes;
            y += rows;
        }
        if (source_offset != byte_count) {
            fputs("gl-texture-selftest: sliced byte accounting failed\n",
                  stderr);
            status = -1;
        }
    }
    glFinish();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    error = CGLSetCurrentContext(render_context);
#pragma clang diagnostic pop
    if (error != kCGLNoError) {
        fprintf(stderr, "gl-texture-selftest: render make-current failed: %d\n",
                error);
        status = -1;
        goto cleanup;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint stored_width = 0;
    GLint stored_height = 0;
    GLint stored_format = 0;
    GLint stored_size = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                             &stored_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                             &stored_height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                             &stored_format);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                             GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &stored_size);
    if (stored_width != width || stored_height != height ||
        stored_format != (GLint)format || stored_size != (GLint)byte_count) {
        fprintf(stderr,
                "gl-texture-selftest: metadata mismatch sliced=%d "
                "size=%dx%d/%dx%d format=%04x/%04x bytes=%d/%zu\n",
                sliced, stored_width, stored_height, width, height,
                stored_format, format, stored_size, byte_count);
        status = -1;
        goto cleanup;
    }
    memset(actual, 0, byte_count);
    glGetCompressedTexImage(GL_TEXTURE_2D, 0, actual);
    if (memcmp(actual, expected, byte_count) != 0) {
        size_t mismatch = 0;
        while (mismatch < byte_count && actual[mismatch] == expected[mismatch]) {
            ++mismatch;
        }
        fprintf(stderr,
                "gl-texture-selftest: compressed payload mismatch "
                "sliced=%d format=%04x at=%zu actual=%02x expected=%02x\n",
                sliced, format, mismatch,
                mismatch < byte_count ? actual[mismatch] : 0,
                mismatch < byte_count ? expected[mismatch] : 0);
        status = -1;
        goto cleanup;
    }
    /* Force the driver to decode the texture as well as preserve its blocks. */
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded);
    {
        GLenum gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            fprintf(stderr,
                    "gl-texture-selftest: decode readback failed "
                    "sliced=%d format=%04x error=%04x\n",
                    sliced, format, gl_error);
            status = -1;
        }
    }

cleanup:
    glDeleteTextures(1, &texture);
    compat_runtime32_deallocate(guest_bytes);
    free(decoded);
    free(actual);
    free(expected);
    return status;
}

static int run_3d_texture_dispatch_case(CGLContextObj upload_context,
                                        CGLContextObj render_context)
{
    enum {
        kWidth = 4,
        kHeight = 3,
        kDepth = 2,
        kBytesPerPixel = 4,
        kByteCount = kWidth * kHeight * kDepth * kBytesPerPixel,
    };
    unsigned char expected[kByteCount];
    unsigned char actual[kByteCount];
    for (size_t index = 0; index < sizeof(expected); ++index) {
        expected[index] = (unsigned char)(index * 37u + 11u);
    }

    uint32_t guest_bytes = compat_runtime32_allocate(sizeof(expected), 0);
    if (!guest_bytes) {
        fputs("gl-texture-selftest: 3D guest allocation failed\n", stderr);
        return -1;
    }
    memcpy((void *)(uintptr_t)guest_bytes, expected, sizeof(expected));

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLError context_error = CGLSetCurrentContext(upload_context);
#pragma clang diagnostic pop
    if (context_error != kCGLNoError) {
        fprintf(stderr,
                "gl-texture-selftest: 3D upload make-current failed: %d\n",
                context_error);
        compat_runtime32_deallocate(guest_bytes);
        return -1;
    }

    GLuint texture = 0;
    int status = 0;
    uint64_t dispatch_result = 0;
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_3D, texture);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const uint32_t image_arguments[] = {
        GL_TEXTURE_3D, 0, GL_RGBA8, kWidth, kHeight, kDepth, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, guest_bytes,
    };
    if (!objc_bridge32_dispatch("glTexImage3D", image_arguments,
                                &dispatch_result)) {
        fputs("gl-texture-selftest: glTexImage3D was not dispatched\n",
              stderr);
        status = -1;
        goto cleanup_3d;
    }
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        fprintf(stderr,
                "gl-texture-selftest: glTexImage3D failed: %04x\n",
                gl_error);
        status = -1;
        goto cleanup_3d;
    }
    glFinish();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    context_error = CGLSetCurrentContext(render_context);
#pragma clang diagnostic pop
    if (context_error != kCGLNoError) {
        fprintf(stderr,
                "gl-texture-selftest: 3D render make-current failed: %d\n",
                context_error);
        status = -1;
        goto cleanup_3d;
    }
    glBindTexture(GL_TEXTURE_3D, texture);
    GLint stored_width = 0;
    GLint stored_height = 0;
    GLint stored_depth = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_WIDTH,
                             &stored_width);
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_HEIGHT,
                             &stored_height);
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH,
                             &stored_depth);
    memset(actual, 0, sizeof(actual));
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR || stored_width != kWidth ||
        stored_height != kHeight || stored_depth != kDepth ||
        memcmp(actual, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "gl-texture-selftest: 3D upload mismatch error=%04x "
                "size=%dx%dx%d expected=%dx%dx%d bytes_equal=%d\n",
                gl_error, stored_width, stored_height, stored_depth,
                kWidth, kHeight, kDepth,
                memcmp(actual, expected, sizeof(expected)) == 0);
        status = -1;
        goto cleanup_3d;
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    context_error = CGLSetCurrentContext(upload_context);
#pragma clang diagnostic pop
    if (context_error != kCGLNoError) {
        status = -1;
        goto cleanup_3d;
    }
    static const unsigned char replacement[] = {0xde, 0xad, 0xbe, 0xef};
    memcpy((void *)(uintptr_t)guest_bytes, replacement, sizeof(replacement));
    const uint32_t update_arguments[] = {
        GL_TEXTURE_3D, 0, 1, 1, 1, 1, 1, 1,
        GL_RGBA, GL_UNSIGNED_BYTE, guest_bytes,
    };
    if (!objc_bridge32_dispatch("glTexSubImage3DEXT", update_arguments,
                                &dispatch_result)) {
        fputs("gl-texture-selftest: glTexSubImage3DEXT was not dispatched\n",
              stderr);
        status = -1;
        goto cleanup_3d;
    }
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        fprintf(stderr,
                "gl-texture-selftest: glTexSubImage3DEXT failed: %04x\n",
                gl_error);
        status = -1;
        goto cleanup_3d;
    }
    memcpy(expected + (((1 * kHeight + 1) * kWidth + 1) * kBytesPerPixel),
           replacement, sizeof(replacement));
    glFinish();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    context_error = CGLSetCurrentContext(render_context);
#pragma clang diagnostic pop
    if (context_error != kCGLNoError) {
        status = -1;
        goto cleanup_3d;
    }
    glBindTexture(GL_TEXTURE_3D, texture);
    memset(actual, 0, sizeof(actual));
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR ||
        memcmp(actual, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "gl-texture-selftest: 3D subimage mismatch error=%04x "
                "bytes_equal=%d\n",
                gl_error, memcmp(actual, expected, sizeof(expected)) == 0);
        status = -1;
    }

cleanup_3d:
    if (texture) glDeleteTextures(1, &texture);
    compat_runtime32_deallocate(guest_bytes);
    return status;
}

/* Deleting from colliding probe runs must leave every other entry
   reachable. Names 8192 apart share a home slot. Runs without a context. */
static int run_mipmap_repair_table_case(void)
{
    enum { kNames = 1500, kAliases = 3 };
    int status = 0;
    pthread_mutex_lock(&mipmap_repair_mutex);
    for (GLuint name = 1; name <= kNames; ++name) {
        for (GLuint alias = 0; alias < kAliases; ++alias) {
            struct mipmap_repair_entry *entry = mipmap_repair_entry_for(
                NULL, GL_TEXTURE_2D, name + alias * 8192u, true);
            if (!entry) status = -1;
            else entry->requested_maximum = (GLint)(name + alias * 8192u);
        }
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);
    GLuint removed[kNames];
    GLsizei removed_count = 0;
    for (GLuint name = 1; name <= kNames; ++name) {
        removed[removed_count++] = name + (name % kAliases) * 8192u;
    }
    forget_mipmap_repairs(removed_count, removed);
    pthread_mutex_lock(&mipmap_repair_mutex);
    for (GLuint name = 1; name <= kNames && status == 0; ++name) {
        for (GLuint alias = 0; alias < kAliases; ++alias) {
            GLuint texture = name + alias * 8192u;
            const struct mipmap_repair_entry *entry = mipmap_repair_entry_for(
                NULL, GL_TEXTURE_2D, texture, false);
            bool expected = alias != name % kAliases;
            if (expected != (entry != NULL) ||
                (entry && entry->requested_maximum != (GLint)texture)) {
                fprintf(stderr, "gl-texture-selftest: mip table entry %u "
                        "%s after deletion\n", texture,
                        expected ? "lost" : "kept");
                status = -1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);
    GLuint all[kNames * kAliases];
    for (GLuint index = 0; index < kNames * kAliases; ++index) {
        all[index] = 1 + index % kNames + (index / kNames) * 8192u;
    }
    forget_mipmap_repairs(kNames * kAliases, all);
    pthread_mutex_lock(&mipmap_repair_mutex);
    for (size_t slot = 0; slot < kMipmapRepairCapacity; ++slot) {
        if (mipmap_repairs[slot].occupied) status = -1;
    }
    pthread_mutex_unlock(&mipmap_repair_mutex);
    if (status) fputs("gl-texture-selftest: mip table not empty\n", stderr);
    return status;
}

int objc_bridge32_run_gl_texture_self_test(void)
{
    if (run_mipmap_repair_table_case() != 0) return -1;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLPixelFormatAttribute attributes[] = {
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pixel_format = NULL;
    GLint format_count = 0;
    CGLContextObj render_context = NULL;
    CGLContextObj upload_context = NULL;
    CGLError error = CGLChoosePixelFormat(attributes, &pixel_format,
                                          &format_count);
    if (error == kCGLNoError && pixel_format && format_count > 0) {
        error = CGLCreateContext(pixel_format, NULL, &render_context);
    }
    if (error == kCGLNoError && render_context) {
        error = CGLCreateContext(pixel_format, render_context,
                                 &upload_context);
    }
    if (pixel_format) CGLDestroyPixelFormat(pixel_format);
#pragma clang diagnostic pop
    if (error != kCGLNoError || !render_context || !upload_context) {
        fprintf(stderr,
                "gl-texture-selftest: context creation failed: %d\n", error);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (upload_context) CGLDestroyContext(upload_context);
        if (render_context) CGLDestroyContext(render_context);
#pragma clang diagnostic pop
        return -1;
    }

    static const struct {
        GLenum format;
        size_t block_bytes;
    } cases[] = {
        {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 8},
        {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16},
    };
    int status = 0;
    for (unsigned int cycle = 0; cycle < 16 && status == 0; ++cycle) {
        for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
             ++index) {
            status = run_compressed_texture_case(
                upload_context, render_context, cases[index].format,
                cases[index].block_bytes, 512, 768, false,
                cycle * 17u + (unsigned int)index);
            if (status != 0) break;
            status = run_compressed_texture_case(
                upload_context, render_context, cases[index].format,
                cases[index].block_bytes, 512, 768, true,
                cycle * 17u + (unsigned int)index);
            if (status != 0) break;
            status = run_compressed_texture_case(
                upload_context, render_context, cases[index].format,
                cases[index].block_bytes, 64, 64, true,
                cycle * 31u + (unsigned int)index);
            if (status != 0) break;
        }
    }

    if (status == 0) {
        status = run_3d_texture_dispatch_case(upload_context,
                                              render_context);
    }

    if (status == 0) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        error = CGLSetCurrentContext(upload_context);
#pragma clang diagnostic pop
        if (error != kCGLNoError) {
            status = -1;
        } else {
            GLuint texture = 0;
            unsigned char blocks[2048];
            memset(blocks, 0x5a, sizeof(blocks));
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_LINEAR_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 6);
            for (GLint level = 0; level < 6; ++level) {
                GLsizei dimension = 64 >> level;
                size_t size = compressed_texture_bytes(
                    dimension, dimension, 8);
                glCompressedTexImage2D(
                    GL_TEXTURE_2D, level,
                    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                    dimension, dimension, 0, (GLsizei)size, blocks);
                repair_bound_texture_mipmap_range(GL_TEXTURE_2D);
            }
            GLint maximum = -1;
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                &maximum);
            if (maximum != 5) {
                fprintf(stderr,
                        "gl-texture-selftest: incomplete chain was not "
                        "clamped: max=%d expected=5\n", maximum);
                status = -1;
            }
            /* Restating the requested maximum, as the game does each bind,
               keeps the clamp, including through the float entry point. */
            set_texture_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 6);
            set_texture_parameter_f(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                    6.0f);
            set_texture_parameter_f(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                    (GLfloat)GL_LINEAR_MIPMAP_NEAREST);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                &maximum);
            if (maximum != 5) {
                fprintf(stderr,
                        "gl-texture-selftest: restated maximum undid "
                        "clamp: max=%d expected=5\n", maximum);
                status = -1;
            }
            /* An upload to another texture leaves this texture's scan
               current. */
            GLuint other_texture = 0;
            glGenTextures(1, &other_texture);
            glBindTexture(GL_TEXTURE_2D, other_texture);
            note_texture_level_change(GL_TEXTURE_2D);
            glCompressedTexImage2D(
                GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                4, 4, 0, 8, blocks);
            glDeleteTextures(1, &other_texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            if (mipmap_parameter_action(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                        6) != kMipmapParameterKeepClamp) {
                fputs("gl-texture-selftest: unrelated upload invalidated "
                      "mip scan\n", stderr);
                status = -1;
            }
            note_texture_level_change(GL_TEXTURE_2D);
            if (mipmap_parameter_action(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                        6) != kMipmapParameterApplyAndScan) {
                fputs("gl-texture-selftest: own upload did not invalidate "
                      "mip scan\n", stderr);
                status = -1;
            }
            glCompressedTexImage2D(
                GL_TEXTURE_2D, 6,
                GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                1, 1, 0, 8, blocks);
            repair_bound_texture_mipmap_range(GL_TEXTURE_2D);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                &maximum);
            if (maximum != 6) {
                fprintf(stderr,
                        "gl-texture-selftest: completed chain did not "
                        "restore requested max: max=%d expected=6\n",
                        maximum);
                status = -1;
            }
            if (mipmap_parameter_action(GL_TEXTURE_2D,
                                        GL_TEXTURE_MIN_FILTER,
                                        GL_LINEAR_MIPMAP_NEAREST) !=
                    kMipmapParameterSkip ||
                mipmap_parameter_action(GL_TEXTURE_2D,
                                        GL_TEXTURE_MAX_LEVEL, 6) !=
                    kMipmapParameterSkip) {
                fputs("gl-texture-selftest: unchanged texture state was "
                      "forwarded to the driver\n", stderr);
                status = -1;
            }
            forget_mipmap_repairs(1, &texture);
            glDeleteTextures(1, &texture);

            if (status == 0) {
                texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                                GL_LINEAR_MIPMAP_NEAREST);
                glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL,
                                6);
                for (GLint level = 0; level < 6; ++level) {
                    GLsizei dimension = 64 >> level;
                    size_t size = compressed_texture_bytes(
                        dimension, dimension, 8);
                    for (unsigned int face = 0; face < 6; ++face) {
                        GLenum face_target =
                            GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
                        glCompressedTexImage2D(
                            face_target, level,
                            GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                            dimension, dimension, 0, (GLsizei)size, blocks);
                        repair_bound_texture_mipmap_range(face_target);
                    }
                }
                glGetTexParameteriv(GL_TEXTURE_CUBE_MAP,
                                    GL_TEXTURE_MAX_LEVEL, &maximum);
                if (maximum != 5) {
                    fprintf(stderr,
                            "gl-texture-selftest: incomplete cube chain was "
                            "not clamped: max=%d expected=5\n", maximum);
                    status = -1;
                }
                for (unsigned int face = 0; face < 5; ++face) {
                    GLenum face_target =
                        GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
                    glCompressedTexImage2D(
                        face_target, 6,
                        GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                        1, 1, 0, 8, blocks);
                    repair_bound_texture_mipmap_range(face_target);
                }
                glGetTexParameteriv(GL_TEXTURE_CUBE_MAP,
                                    GL_TEXTURE_MAX_LEVEL, &maximum);
                if (maximum != 5) {
                    fprintf(stderr,
                            "gl-texture-selftest: five-face cube level "
                            "escaped clamp: max=%d expected=5\n", maximum);
                    status = -1;
                }
                glCompressedTexImage2D(
                    GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 6,
                    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                    1, 1, 0, 8, blocks);
                repair_bound_texture_mipmap_range(
                    GL_TEXTURE_CUBE_MAP_NEGATIVE_Z);
                glGetTexParameteriv(GL_TEXTURE_CUBE_MAP,
                                    GL_TEXTURE_MAX_LEVEL, &maximum);
                if (maximum != 6) {
                    fprintf(stderr,
                            "gl-texture-selftest: completed cube chain did "
                            "not restore requested max: max=%d expected=6\n",
                            maximum);
                    status = -1;
                }
                forget_mipmap_repairs(1, &texture);
                glDeleteTextures(1, &texture);
            }
        }
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(upload_context);
    CGLDestroyContext(render_context);
#pragma clang diagnostic pop
    if (status == 0) {
        puts("gl-texture-selftest: PASS (DXT1/DXT5 direct and sliced "
             "uploads survive shared contexts and texture-name reuse; "
             "3D image/subimage dispatch works; incomplete 2D/cubemap "
             "ranges are clamped and restored; mip table deletion keeps "
             "colliding entries)");
    }
    return status;
}

/* Exercise the guest ABI while changing Cocoa focus independently of Quartz.
   Only this self-test replaces mainScreen; no game launch uses that hook. */
static NSScreen *display_test_focus;
static NSScreen *display_test_main_screen(id self, SEL selector)
{
    (void)self; (void)selector;
    return display_test_focus;
}

int objc_bridge32_run_display_self_test(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        unsetenv("LP32_TEST_DISPLAY"); unsetenv("LP32_DISPLAY_INDEX");
        unsetenv("LP32_USE_SECONDARY_DISPLAY");
        NSArray *screens = NSScreen.screens;
        if (!screens.count) return -1;
        Method main = class_getClassMethod([NSScreen class], @selector(mainScreen));
        IMP original = method_setImplementation(main, (IMP)display_test_main_screen);
        bool valid = true;
#define DISPLAY_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Display bridge FAIL at line %d: %s\n", __LINE__, #condition); \
    valid = false; } } while (0)
        CGDirectDisplayID primary = CGMainDisplayID();
        uint32_t main_args[] = {proxy_for_object([NSScreen class]), objc_bridge32_guest_selector("mainScreen")};
        uint64_t result = 0;
        for (unsigned i = 0; i < screens.count * 2; ++i) {
            display_test_focus = screens[i % screens.count];
            DISPLAY_CHECK(objc_bridge32_dispatch("_objc_msgSend", main_args, &result) &&
                object_for_argument((uint32_t)result) == display_test_focus);
            DISPLAY_CHECK(objc_bridge32_dispatch("_CGMainDisplayID", NULL, &result) && result == primary);
            for (NSScreen *screen in screens) {
                uint32_t a[] = {display_id_for_screen(screen)};
                DISPLAY_CHECK(objc_bridge32_dispatch("_CGDisplayIsMain", a, &result) && result == (a[0] == primary));
            }
        }
        for (NSUInteger i = 0; i < screens.count; ++i) {
            char index[32]; snprintf(index, sizeof index, "%lu", (unsigned long)i);
            setenv("LP32_DISPLAY_INDEX", index, 1);
            DISPLAY_CHECK(preferred_game_screen() == screens[i]);
            DISPLAY_CHECK(preferred_game_display_id() == display_id_for_screen(screens[i]));
        }
        setenv("LP32_DISPLAY_INDEX", "9999", 1);
        DISPLAY_CHECK(preferred_game_screen() == display_test_focus && preferred_game_display_id() == primary);
        unsetenv("LP32_DISPLAY_INDEX");
        uint32_t output = compat_runtime32_allocate(16, 1);
        uint32_t frame_selector = objc_bridge32_guest_selector("frame");
        for (NSScreen *target in screens) {
            CGDirectDisplayID target_id = display_id_for_screen(target);
            CFDictionaryRef bounds = CGRectCreateDictionaryRepresentation(CGDisplayBounds(target_id));
            NSDictionary *window = @{(id)kCGWindowOwnerPID:@4242, (id)kCGWindowLayer:@0,
                (id)kCGWindowAlpha:@1, (id)kCGWindowBounds:(id)bounds};
            DISPLAY_CHECK(screen_for_foreground_window(@[window], 4242) == target);
            DISPLAY_CHECK(!screen_for_foreground_window(@[window], 4243));
            CFRelease(bounds);
            NSDictionary *mode = nil;
            NSSize native = display_native_size(target_id);
            for (NSDictionary *candidate in legacy_modes_for_display(target_id)) {
                if ([candidate[@"Width"] doubleValue] != native.width ||
                    [candidate[@"Height"] doubleValue] != native.height) { mode = candidate; break; }
            }
            DISPLAY_CHECK(mode != nil);
            NSSize selected = NSMakeSize([mode[@"Width"] doubleValue], [mode[@"Height"] doubleValue]);
            uint32_t switch_args[] = {target_id, proxy_for_object(mode), 0};
            DISPLAY_CHECK(objc_bridge32_dispatch("_CGDisplaySwitchToMode", switch_args, &result));
            for (NSScreen *screen in screens) {
                uint32_t args[] = {output, proxy_for_object(screen), frame_selector};
                DISPLAY_CHECK(objc_bridge32_dispatch("_objc_msgSend_stret", args, &result));
                const float *r = (void *)(uintptr_t)output;
                NSRect expected = screen.frame;
                if (screen == target) expected.size = selected;
                DISPLAY_CHECK(r[0] == expected.origin.x && r[1] == expected.origin.y &&
                    r[2] == expected.size.width && r[3] == expected.size.height);
                uint32_t display_args[] = {display_id_for_screen(screen)};
                DISPLAY_CHECK(objc_bridge32_dispatch("_CGDisplayCurrentMode", display_args, &result));
                NSDictionary *current = object_for_argument((uint32_t)result);
                NSSize size = screen == target ? selected : display_native_size(display_args[0]);
                DISPLAY_CHECK([current[@"Width"] doubleValue] == size.width &&
                    [current[@"Height"] doubleValue] == size.height);
                if (screen != target) {
                    DISPLAY_CHECK(objc_bridge32_dispatch("_CGDisplayRelease", display_args, &result));
                    DISPLAY_CHECK(guest_display_mode_display == target_id && guest_display_mode_active());
                }
            }
            DISPLAY_CHECK(objc_bridge32_dispatch("_CGReleaseAllDisplays", NULL, &result));
            DISPLAY_CHECK(!guest_display_mode_active());

            /* Creation uses launch placement even while AppKit focus is on
               another monitor. Subsequent frame changes follow the window. */
            display_test_focus = screens[0];
            launch_display = target_id;
            NSWindow *fullscreen = [NSFullScreenWindow alloc];
            uint32_t init_args[9] = {proxy_for_object(fullscreen), objc_bridge32_guest_selector(
                "initWithContentRect:styleMask:backing:defer:")};
            float tiny[] = {0,0,64,48}; memcpy(init_args + 2, tiny, sizeof tiny);
            init_args[7] = NSBackingStoreBuffered; init_args[8] = 1;
            DISPLAY_CHECK(objc_bridge32_dispatch("_objc_msgSend", init_args, &result));
            fullscreen = object_for_argument((uint32_t)result);
            fullscreen.releasedWhenClosed = NO;
            DISPLAY_CHECK(NSEqualRects(fullscreen.frame, target.frame));
            for (NSScreen *destination in screens) {
                [fullscreen setFrame:destination.frame display:NO];
                uint32_t frame_args[7] = {proxy_for_object(fullscreen), objc_bridge32_guest_selector("setFrame:display:")};
                memcpy(frame_args + 2, tiny, sizeof tiny);
                DISPLAY_CHECK(objc_bridge32_dispatch("_objc_msgSend", frame_args, &result));
                DISPLAY_CHECK(NSEqualRects(fullscreen.frame, destination.frame));
                DISPLAY_CHECK(objc_bridge32_dispatch("_CGMainDisplayID", NULL, &result) && result == primary);
            }
            [fullscreen close]; [fullscreen release];
        }
        compat_runtime32_deallocate(output);
        launch_display = 0;
        method_setImplementation(main, original);
        if (valid) printf("Display bridge PASS (%lu screens; dynamic Cocoa focus; independent Quartz primary; overrides; foreground placement; display modes and origins; LEGO fullscreen creation and moves)\n", (unsigned long)screens.count);
#undef DISPLAY_CHECK
        return valid ? 0 : -1;
    }
}
