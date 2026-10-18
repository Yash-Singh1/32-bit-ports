#include "carbon_bridge.h"
#include "gl_backend.h"
#include "agl_pixel_format.h"
#include "focus_policy.h"
#include "carbon_text.h"
#include "carbon_ui.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include "resource_bridge.h"
#include "hitch_recorder.h"
#include "game_profile.h"
#include <Carbon/Carbon.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <math.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

/* Carbon still supplies many 64-bit services, but its opaque references and
 * callbacks must not be truncated to guest pointers. Keep those in a table. */
struct carbon_ref {
  void *host;
  void *callback;
  uint32_t token;
  int32_t guest_refcon;
  uint16_t background_color[3];
  uint16_t foreground_color[3];
  EventHandlerRef background_handler;
  CGImageRef background_image;
  uint32_t agl_drawable;
  uint32_t port_pixmap;
  bool display_port;
  bool legacy_window_coordinates;
  struct carbon_ref *next;
};
static struct carbon_ref *references;
/* Classic file APIs expose signed 16-bit IDs; modern FSIORefNum is 32-bit. */
static FSIORefNum classic_files[1024];
static uint32_t next_token = 0x74000000;
#pragma pack(push, 2)
struct alert_params32 {
  uint32_t version;
  uint8_t movable, help;
  uint32_t default_text, cancel_text, other_text;
  int16_t default_button, cancel_button;
  uint16_t position;
  uint32_t flags, icon;
};
struct fork_io32 {
  uint32_t link; int16_t type,trap; uint32_t command,completion;
  int16_t result; uint32_t reserved1; int16_t reserved2,fork;
  uint8_t reserved3; int8_t permissions; uint32_t ref,buffer,requested,actual;
  uint16_t mode; int64_t offset;
};
_Static_assert(offsetof(struct fork_io32,result)==16 &&
               offsetof(struct fork_io32,fork)==24 &&
               offsetof(struct fork_io32,offset)==46,"i386 FSForkIOParam layout");
struct pixmap32 {
  uint32_t pixels;
  int16_t row_bytes;
  Rect bounds;
  int16_t version, pack_type;
  uint32_t pack_size, hres, vres;
  int16_t pixel_type, pixel_size, components, component_size;
  uint32_t format, table, extension;
};
struct device32 {
  int16_t refnum, id, type;
  uint32_t inverse_table;
  int16_t resolution;
  uint32_t search, complement;
  int16_t flags;
  uint32_t pixmap, refcon, next;
  Rect bounds;
  uint32_t mode;
  int16_t cursor_bytes, cursor_depth;
  uint32_t cursor_data, cursor_mask, extension;
};
#pragma pack(pop)
static struct {
  CGDirectDisplayID id;
  uint32_t handle;
} displays[32];
static uint32_t display_count;
static uint32_t current_port;
static void initialize_displays(void) {
  if (display_count)
    return;
  CGDirectDisplayID ids[32];
  uint32_t count = 0;
  if (CGGetActiveDisplayList(32, ids, &count))
    return;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t handle = compat_runtime32_allocate(4, 1);
    uint32_t data = compat_runtime32_allocate(sizeof(struct device32), 1);
    uint32_t pixmap = compat_runtime32_allocate(4, 1);
    uint32_t pixels = compat_runtime32_allocate(sizeof(struct pixmap32), 1);
    if (!handle || !data || !pixmap || !pixels)
      return;
    *(uint32_t *)(uintptr_t)handle = data;
    *(uint32_t *)(uintptr_t)pixmap = pixels;
    CGRect bounds = CGDisplayBounds(ids[i]);
    struct device32 *device = (void *)(uintptr_t)data;
    device->bounds = (Rect){(int16_t)bounds.origin.y, (int16_t)bounds.origin.x,
                            (int16_t)(bounds.origin.y + bounds.size.height),
                            (int16_t)(bounds.origin.x + bounds.size.width)};
    device->type = 2;
    device->pixmap = pixmap;
    device->flags = (int16_t)((1u << 15) | (1u << 13) | 1u |
                              (ids[i] == CGMainDisplayID() ? (1u << 11) : 0));
    struct pixmap32 *map = (void *)(uintptr_t)pixels;
    map->bounds = device->bounds;
    map->row_bytes =
        (int16_t)(0x8000 | ((uint32_t)bounds.size.width * 4 & 0x3fff));
    map->hres = map->vres = 72 << 16;
    map->pixel_type = 16;
    map->pixel_size = 32;
    map->components = 3;
    map->component_size = 8;
    if (i)
      ((struct device32 *)(uintptr_t)*(uint32_t *)(uintptr_t)displays[i - 1]
           .handle)
          ->next = handle;
    displays[i].id = ids[i];
    displays[i].handle = handle;
    ++display_count;
  }
}
static void *library;
static pthread_once_t library_once = PTHREAD_ONCE_INIT;
static _Thread_local unsigned symbol_lookups;
static void open_carbon_library(void) {
  library = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon",
                   RTLD_LOCAL | RTLD_NOW);
}
static void *symbol(const char *name) {
  /* The system framework stays loaded. Cache successful and absent exports,
     including aliases, without a shared lock in event/input polling. */
  static _Thread_local struct {
    char name[128];
    void *function;
  } cache[128];
  uint32_t hash = 2166136261u;
  size_t length = 0;
  for (; name[length]; ++length) hash = (hash ^ (unsigned char)name[length]) * 16777619u;
  unsigned slot = hash % 128;
  if (length < sizeof(cache[slot].name) && !strcmp(cache[slot].name, name))
    return cache[slot].function;
  pthread_once(&library_once, open_carbon_library);
  ++symbol_lookups;
  void *function = library ? dlsym(library, name + 1) : NULL;
  if (length < sizeof(cache[slot].name)) {
    memcpy(cache[slot].name, name, length + 1);
    cache[slot].function = function;
  }
  return function;
}
static unsigned pending_fork_reads;
static void read_fork_async(void *opaque) {
  struct fork_io32 *guest=opaque;
  uint32_t completion=guest->completion,argument=(uint32_t)(uintptr_t)guest;
  ByteCount actual=0;
  OSErr status=((OSErr (*)(FSIORefNum,UInt16,SInt64,ByteCount,void *,ByteCount *))symbol("_FSReadFork"))(
      guest->fork,guest->mode,guest->offset,guest->requested,(void *)(uintptr_t)guest->buffer,&actual);
  guest->actual=(uint32_t)actual;
  __atomic_store_n(&guest->result,status,__ATOMIC_RELEASE);
  if(completion)compat_runtime32_call(completion,&argument,1);
  __atomic_sub_fetch(&pending_fork_reads,1,__ATOMIC_RELEASE);
}
/* FSSpec disappeared from the 64-bit entry points. Preserve its packed
 * 70-byte ABI and resolve parent IDs via FSRefs obtained from File Manager. */
struct __attribute__((packed)) spec32 {
  int16_t volume;
  uint32_t parent;
  unsigned char name[64];
};
struct directory_ref {
  int16_t volume;
  uint32_t id;
  FSRef ref;
  struct directory_ref *next;
};
static struct directory_ref *directories;
static void remember_directory(int16_t volume, uint32_t id, const FSRef *ref) {
  for (struct directory_ref *p = directories; p; p = p->next)
    if (p->volume == volume && p->id == id) {
      p->ref = *ref;
      return;
    }
  struct directory_ref *p = malloc(sizeof(*p));
  if (!p)
    return;
  *p = (struct directory_ref){volume, id, *ref, directories};
  directories = p;
}
/* i386 FSPermissionInfo ends in a 4-byte reserved field; x86_64 replaces it
   with an 8-byte FSFileSecurityRef, so later FSCatalogInfo fields move by
   four bytes (144 bytes on i386, 148 natively). The ACL reference is never
   exposed to the guest. */
enum { kCatalogInfo32Size = 144, kCatalogInfoCommonPrefix = 68 };
static void catalog_info_to_guest(const FSCatalogInfo *host, void *guest) {
  memcpy(guest, host, kCatalogInfoCommonPrefix);
  memset((char *)guest + kCatalogInfoCommonPrefix, 0, 4);
  memcpy((char *)guest + kCatalogInfoCommonPrefix + 4,
         (const char *)host + kCatalogInfoCommonPrefix + 8,
         kCatalogInfo32Size - kCatalogInfoCommonPrefix - 4);
}
static const FSCatalogInfo *catalog_info_from_guest(const void *guest, FSCatalogInfo *host) {
  if (!guest)
    return NULL;
  memset(host, 0, sizeof(*host));
  memcpy(host, guest, kCatalogInfoCommonPrefix);
  memcpy((char *)host + kCatalogInfoCommonPrefix + 8,
         (const char *)guest + kCatalogInfoCommonPrefix + 4,
         kCatalogInfo32Size - kCatalogInfoCommonPrefix - 4);
  return host;
}
_Static_assert(sizeof(FSCatalogInfo) == kCatalogInfo32Size + 4, "FSCatalogInfo layout");

static int16_t spec_from_ref(const FSRef *ref, struct spec32 *spec) {
  FSCatalogInfo info;
  HFSUniStr255 name;
  FSRef parent;
  int16_t (*get)(const FSRef *, uint32_t, void *, void *, void *, void *) =
      symbol("_FSGetCatalogInfo");
  int16_t status = get(ref,
                       kFSCatInfoVolume | kFSCatInfoParentDirID |
                           kFSCatInfoNodeID | kFSCatInfoNodeFlags,
                       &info, &name, NULL, &parent);
  if (status)
    return status;
  CFStringRef string =
      CFStringCreateWithCharacters(NULL, name.unicode, name.length);
  bool fits =
      string && CFStringGetPascalString(string, spec->name, sizeof(spec->name),
                                        kCFStringEncodingMacRoman);
  if (string)
    CFRelease(string);
  if (!fits)
    return -37;
  spec->volume = info.volume;
  spec->parent = info.parentDirID;
  remember_directory(info.volume, info.parentDirID, &parent);
  if (info.nodeFlags & kFSNodeIsDirectoryMask)
    remember_directory(info.volume, info.nodeID, ref);
  return 0;
}
static int16_t ref_from_spec(const struct spec32 *spec, FSRef *ref) {
  if (!spec || !ref || spec->name[0] > 63)
    return -50;
  for (struct directory_ref *p = directories; p; p = p->next)
    if (p->volume == spec->volume && p->id == spec->parent) {
      if (!spec->name[0]) {
        *ref = p->ref;
        return 0;
      }
      CFStringRef string = CFStringCreateWithPascalString(
          NULL, spec->name, kCFStringEncodingMacRoman);
      if (!string)
        return -37;
      UniChar name[64];
      CFIndex length = CFStringGetLength(string);
      CFStringGetCharacters(string, CFRangeMake(0, length), name);
      CFRelease(string);
      int16_t (*make)(const FSRef *, uint32_t, const UniChar *, uint32_t,
                      FSRef *) = symbol("_FSMakeFSRefUnicode");
      return make(&p->ref, (uint32_t)length, name, kTextEncodingUnknown, ref);
    }
  return -120;
}
static int16_t find_folder32(int16_t volume, uint32_t type, bool create,
                             FSRef *ref) {
  const char *home = getenv("LP32_TEST_HOME_DIR");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const char *suffix = type == kPreferencesFolderType ? "/Library/Preferences"
                       : type == kApplicationSupportFolderType
                           ? "/Library/Application Support"
                       : type == kCachedDataFolderType ? "/Library/Caches"
                       : type == kDocumentsFolderType  ? "/Documents"
                                                       : NULL;
#pragma clang diagnostic pop
  if (home && home[0] && suffix) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s%s", home, suffix);
    if (n < 0 || (size_t)n >= sizeof(path))
      return -50;
    if (create)
      for (char *p = path + 1;; ++p) {
        if (*p != '/' && *p)
          continue;
        char saved = *p;
        *p = 0;
        int r = mkdir(path, 0700);
        int error = errno;
        *p = saved;
        if (r && error != EEXIST)
          return -54;
        if (!saved)
          break;
      }
    return ((int16_t (*)(const UInt8 *, FSRef *, Boolean *))symbol(
        "_FSPathMakeRef"))((const UInt8 *)path, ref, NULL);
  }
  return ((int16_t (*)(int16_t, uint32_t, bool, FSRef *))symbol(
      "_FSFindFolder"))(volume, type, create, ref);
}
int carbon_bridge32_path_from_spec(uint32_t spec, char *path,
                                   uint32_t capacity) {
  FSRef ref;
  int16_t status = ref_from_spec((const void *)(uintptr_t)spec, &ref);
  if (!status)
    status = ((int16_t (*)(const FSRef *, void *, uint32_t))symbol(
        "_FSRefMakePath"))(&ref, path, capacity);
  return status;
}
static uint32_t wrap(void *host) {
  if (!host)
    return 0;
  for (struct carbon_ref *r = references; r; r = r->next)
    if (r->host == host)
      return r->token;
  struct carbon_ref *r = calloc(1, sizeof(*r));
  if (!r)
    return 0;
  if (next_token >= 0x75000000) {
    free(r);
    return 0;
  }
  r->host = host;
  r->token = next_token;
  next_token += 16;
  r->next = references;
  references = r;
  return r->token;
}
static struct carbon_ref *reference(uint32_t token) {
  for (struct carbon_ref *r = references; r; r = r->next)
    if (r->token == token)
      return r;
  return NULL;
}
static void *unwrap(uint32_t token) {
  struct carbon_ref *r = reference(token);
  return r ? r->host : objc_bridge32_host_object(token);
}
static OSStatus draw_window_background(EventHandlerCallRef call, EventRef event, void *data) {
  (void)call;
  struct carbon_ref *window = data;
  CGContextRef context = NULL;
  if (GetEventParameter(event, kEventParamCGContextRef, typeCGContextRef, NULL,
                        sizeof(context), NULL, &context) || !context) return eventNotHandledErr;
  Rect bounds;
  int32_t (*get_bounds)(void *, uint16_t, Rect *) = symbol("_GetWindowBounds");
  if (get_bounds(window->host, kWindowContentRgn, &bounds)) return eventNotHandledErr;
  CGContextSaveGState(context);
  CGContextSetRGBFillColor(context, window->background_color[0] / 65535.0,
      window->background_color[1] / 65535.0, window->background_color[2] / 65535.0, 1);
  CGContextFillRect(context, CGRectMake(0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top));
  if (window->background_image) {
    CGContextTranslateCTM(context, 0, bounds.bottom - bounds.top);
    CGContextScaleCTM(context, 1, -1);
    CGContextDrawImage(context, CGRectMake(0, 0, bounds.right - bounds.left,
                                         bounds.bottom - bounds.top), window->background_image);
  }
  CGContextRestoreGState(context);
  return noErr;
}
int carbon_bridge32_region_bounds(uint32_t region, void *rect) {
  void *host = unwrap(region);
  if (!host)
    return 0;
  if (!((uint8_t (*)(void *))symbol("_IsRegionRectangular"))(host))
    return 0;
  ((void *(*)(void *, void *))symbol("_GetRegionBounds"))(host, rect);
  return 1;
}
static uint32_t resource_handle(uint32_t type, int16_t id, uint32_t *size) {
  uint32_t args[] = {type, (uint32_t)(int32_t)id};
  uint64_t result = 0;
  resource_bridge32_dispatch("_GetResource", args, &result);
  uint32_t handle = (uint32_t)result;
  if (size) {
    resource_bridge32_dispatch("_GetHandleSize", &handle, &result);
    *size = (uint32_t)result;
  }
  return handle;
}
static uint16_t read_be16(const unsigned char *p) {
  return (uint16_t)p[0] << 8 | p[1];
}
static void *menu_from_resource(int16_t id) {
  uint32_t size, handle = resource_handle(0x4d454e55, id, &size);
  if (!handle || size < 15)
    return NULL;
  unsigned char *bytes = (void *)(uintptr_t)*(uint32_t *)(uintptr_t)handle;
  size_t pos = 14;
  if (pos + 1 + bytes[pos] > size)
    return NULL;
  void *menu = NULL;
  int32_t (*create)(int16_t, uint32_t, void **) = symbol("_CreateNewMenu");
  if (!create || create(id, 0, &menu))
    return NULL;
  CFStringRef title = CFStringCreateWithPascalString(NULL, bytes + pos,
                                                     kCFStringEncodingMacRoman);
  ((void (*)(void *, CFStringRef))symbol("_SetMenuTitleWithCFString"))(menu,
                                                                       title);
  CFRelease(title);
  pos += bytes[pos] + 1;
  uint32_t enabled =
      (uint32_t)read_be16(bytes + 10) << 16 | read_be16(bytes + 12);
  uint16_t item = 0;
  while (pos < size && bytes[pos]) {
    unsigned length = bytes[pos];
    if (pos + length + 5 > size)
      break;
    CFStringRef text = CFStringCreateWithPascalString(
        NULL, bytes + pos, kCFStringEncodingMacRoman);
    uint32_t attributes =
        (length == 1 && bytes[pos + 1] == '-') ? kMenuItemAttrSeparator : 0;
    ((int32_t (*)(void *, CFStringRef, uint32_t, uint32_t, uint16_t *))symbol(
        "_AppendMenuItemTextWithCFString"))(menu, text, attributes, 0, &item);
    CFRelease(text);
    pos += length + 1;
    ((int32_t (*)(void *, uint16_t, bool, uint16_t))symbol(
        "_SetMenuItemCommandKey"))(menu, item, false, bytes[pos + 1]);
    if (item < 32 && !(enabled & (1u << item)))
      ((void (*)(void *, uint16_t))symbol("_DisableMenuItem"))(menu, item);
    pos += 4;
  }
  return menu;
}
struct handler32 {
  uint32_t callback, data;
};
static void control_action(void *control, int16_t part) {
  uint32_t token = wrap(control);
  struct carbon_ref *ref = reference(token);
  struct handler32 *handler = ref ? ref->callback : NULL;
  if (handler) {
    uint32_t args[] = {token, (uint32_t)(int32_t)part};
    compat_runtime32_call(handler->callback, args, 2);
  }
}
struct descriptor32 {
  uint32_t type, data;
};
static AEDesc host_descriptor(const struct descriptor32 *d) {
  return (AEDesc){d->type, unwrap(d->data)};
}
static void guest_descriptor(struct descriptor32 *d, const AEDesc *host) {
  d->type = host->descriptorType;
  d->data = wrap(host->dataHandle);
}
static OSErr apple_event_handler(const AppleEvent *event, AppleEvent *reply,
                                 SRefCon raw) {
  struct handler32 *h = (void *)raw;
  uint32_t storage = compat_runtime32_allocate(16, 1);
  if (!storage)
    return memFullErr;
  struct descriptor32 *descs = (void *)(uintptr_t)storage;
  guest_descriptor(descs, event);
  guest_descriptor(descs + 1, reply);
  uint32_t args[] = {storage, storage + 8, h->data};
  OSErr status = (OSErr)compat_runtime32_call(h->callback, args, 3);
  *reply = host_descriptor(descs + 1);
  compat_runtime32_deallocate(storage);
  return status;
}
struct apple_handler_entry {
  uint32_t event_class, event_id, callback;
  bool system;
  struct apple_handler_entry *next;
};
static struct apple_handler_entry *apple_handlers;
static void record_mouse_event(EventRef event) {
  if (!event || GetEventClass(event) != kEventClassMouse) return;
  Point position;
  if (!GetEventParameter(event, kEventParamMouseLocation, typeQDPoint, NULL,
                         sizeof(position), NULL, &position))
    objc_bridge32_record_mouse_position(position.h, position.v);
}
static int32_t event_handler(void *call, void *event, void *raw) {
  struct handler32 *h = raw;
  carbon_ui_sync_focus();
  if (lp32_suppress_background_input() &&
      (GetEventClass(event) == kEventClassMouse || GetEventClass(event) == kEventClassKeyboard))
    return eventNotHandledErr;
  record_mouse_event(event);
  uint32_t a[] = {wrap(call), wrap(event), h->data};
  return (int32_t)compat_runtime32_call(h->callback, a, 3);
}
/* Noncomposited Carbon controls use window-port coordinates. We create a
 * composited native window, whose HIViews instead use their parent's space. */
static void control_port_rect(void *view, CGRect *rect, bool to_parent) {
  void *window = ((void *(*)(void *))symbol("_HIViewGetWindow"))(view);
  struct carbon_ref *ref = reference(wrap(window));
  if (!ref || !ref->legacy_window_coordinates)
    return;
  void *root = NULL;
  ((int32_t (*)(void *, void **))symbol("_GetRootControl"))(window, &root);
  void *parent = ((void *(*)(void *))symbol("_HIViewGetSuperview"))(view);
  if (root && parent && root != parent)
    ((int32_t (*)(CGRect *, void *, void *))symbol("_HIViewConvertRect"))(
        rect, to_parent ? root : parent, to_parent ? parent : root);
}
static void place_test_window(void *window) {
  const char *setting = getenv("LP32_TEST_DISPLAY");
  if (!setting || !setting[0])
    return;
  CGDirectDisplayID requested = (CGDirectDisplayID)strtoul(setting, NULL, 0),
                    ids[32];
  uint32_t count = 0;
  if (CGGetActiveDisplayList(32, ids, &count))
    return;
  bool found = false;
  for (uint32_t i = 0; i < count; ++i)
    if (ids[i] == requested)
      found = true;
  if (!found)
    return;
  CGRect display = CGDisplayBounds(requested);
  Rect bounds = {0};
  if (((int32_t (*)(void *, uint32_t, Rect *))symbol("_GetWindowBounds"))(
          window, 33, &bounds))
    return;
  int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
  int x = (int)(display.origin.x + fmax(0, (display.size.width - width) / 2));
  int y =
      (int)(display.origin.y + fmax(32, (display.size.height - height) / 3));
  ((void (*)(void *, int16_t, int16_t, bool))symbol("_MoveWindow"))(window, x,
                                                                    y, false);
}
/* Older control factories vanished from 64-bit Carbon. A container with
 * native buttons preserves the tab's one-based value and change events. */
struct tab_button {
  void *container;
  int32_t index;
};
static int32_t tab_hit(void *call, void *event, void *raw) {
  (void)call;
  (void)event;
  struct tab_button *button = raw;
  ((int32_t (*)(void *, int32_t))symbol("_HIViewSetValue"))(button->container,
                                                            button->index);
  return 0;
}
static int32_t create_view(CFStringRef class_id, void *window,
                           const Rect *bounds, void **view) {
  bool (*load)(void) = symbol("_NSApplicationLoad");
  if (load)
    load();
  int32_t status = ((int32_t (*)(CFStringRef, void *, void **))symbol(
      "_HIObjectCreate"))(class_id, NULL, view);
  if (status) {
    fprintf(stderr, "compat32: Carbon container creation failed: %d\n", status);
    return status;
  }
  CGRect frame =
      CGRectMake(bounds->left, bounds->top, bounds->right - bounds->left,
                 bounds->bottom - bounds->top);
  ((int32_t (*)(void *, const CGRect *))symbol("_HIViewSetFrame"))(*view,
                                                                   &frame);
  void *root = NULL;
  ((int32_t (*)(void *, void **))symbol("_GetRootControl"))(window, &root);
  if (!root)
    ((int32_t (*)(void *, void **))symbol("_CreateRootControl"))(window, &root);
  if (!root)
    root = ((void *(*)(void *))symbol("_HIViewGetRoot"))(window);
  if (root)
    status =
        ((int32_t (*)(void *, void *))symbol("_HIViewAddSubview"))(root, *view);
  if (getenv("LP32_TRACE_CARBON"))
    fprintf(stderr,
            "compat32: Carbon container attached=%d root=%d parent=%d\n",
            status, root != NULL,
            ((void *(*)(void *))symbol("_HIViewGetSuperview"))(*view) != NULL);
  ((int32_t (*)(void *, bool))symbol("_HIViewSetVisible"))(*view, true);
  return 0;
}
static int32_t create_container(void *window, const Rect *bounds, void **view) {
  int32_t status = create_view(CFSTR("com.apple.hiview"), window, bounds, view);
  if (!status)
    status = ((int32_t (*)(void *, uint64_t, uint64_t))symbol(
        "_HIViewChangeFeatures"))(*view, kControlSupportsEmbedding, 0);
  if (!status) {
    ((int32_t (*)(void *, bool))symbol("_HIObjectSetAccessibilityIgnored"))(
        *view, false);
    ((int32_t (*)(void *, uint64_t, CFStringRef, CFTypeRef))symbol(
        "_HIObjectSetAuxiliaryAccessibilityAttribute"))(
        *view, 0, CFSTR("AXRole"), CFSTR("AXGroup"));
  }
  return status;
}
static OSStatus draw_separator(EventHandlerCallRef call, EventRef event,
                               void *view) {
  (void)call;
  CGContextRef context = NULL;
  CGRect bounds;
  if (GetEventParameter(event, kEventParamCGContextRef, typeCGContextRef, NULL,
                        sizeof(context), NULL, &context) ||
      !context)
    return eventNotHandledErr;
  ((int32_t (*)(void *, CGRect *))symbol("_HIViewGetBounds"))(view, &bounds);
  CGContextSaveGState(context);
  CGContextSetGrayStrokeColor(context, 0.55, 1);
  CGContextSetLineWidth(context, 1);
  if (bounds.size.width >= bounds.size.height) {
    CGContextMoveToPoint(context, 0, bounds.size.height / 2);
    CGContextAddLineToPoint(context, bounds.size.width, bounds.size.height / 2);
  } else {
    CGContextMoveToPoint(context, bounds.size.width / 2, 0);
    CGContextAddLineToPoint(context, bounds.size.width / 2, bounds.size.height);
  }
  CGContextStrokePath(context);
  CGContextRestoreGState(context);
  return 0;
}
static void timer_callback(void *timer, void *raw) {
  struct handler32 *h = raw;
  uint32_t a[] = {wrap(timer), h->data};
  compat_runtime32_call(h->callback, a, 2);
}
static int agl_dispatch(const char *name, const uint32_t *a, uint64_t *out) {
  if (strncmp(name, "_agl", 4))
    return 0;
  /* Apple's AGL or LP32GL's, whichever implements OpenGL (gl_backend.h). */
  void *function = lp32_gl_backend_agl_symbol(name + 1);
#define IS(s) (!strcmp(name, s))
#define P(i) ((void *)(uintptr_t)a[i])
#define F(type, ...) ((type (*)(__VA_ARGS__))function)
  if (IS("_aglQueryRendererInfo")) {
    CGDirectDisplayID ids[32];
    unsigned count = 0;
    initialize_displays();
    if (a[0] && a[1] <= 32)
      for (uint32_t i = 0; i < a[1]; ++i)
        for (uint32_t j = 0; j < display_count; ++j)
          if (((uint32_t *)P(0))[i] == displays[j].handle)
            ids[count++] = displays[j].id;
    void *(*query)(const CGDirectDisplayID *, int) =
        lp32_gl_backend_agl_symbol("aglQueryRendererInfoForCGDirectDisplayIDs");
    *out = query ? wrap(query(count ? ids : NULL, (int)count)) : 0;
    return 1;
  }
  if (IS("_aglSetDrawable")) {
    if (lp32_gl_backend_is_replacement() || lp32_title_is_cod_carbon(lp32_profile()->title)) {
      void *cgl = NULL;
      uint8_t (*get)(void *, void **) = lp32_gl_backend_agl_symbol("aglGetCGLContext");
      *out = get && get(unwrap(a[0]), &cgl) && carbon_ui_bind_gl(unwrap(a[0]), cgl, unwrap(a[1]));
      struct carbon_ref *context = reference(a[0]);
      if (*out && context) context->agl_drawable = a[1];
      return 1;
    }
    uint8_t (*set)(void *, void *) = lp32_gl_backend_agl_symbol("aglSetWindowRef");
    *out = set ? set(unwrap(a[0]), unwrap(a[1])) : 0;
    if (*out && a[1]) {
      carbon_ui_use_native(unwrap(a[1]));
      place_test_window(unwrap(a[1]));
    }
    return 1;
  }
  if (IS("_aglGetDrawable")) {
    struct carbon_ref *context = reference(a[0]);
    if (context && context->agl_drawable) { *out = context->agl_drawable; return 1; }
    void *(*get)(void *) = lp32_gl_backend_agl_symbol("aglGetWindowRef");
    *out = get ? wrap(get(unwrap(a[0]))) : 0;
    return 1;
  }
  if (!function)
    return 0;
  if (IS("_aglUpdateContext") && carbon_ui_update_gl(unwrap(a[0]))) { *out = 1; return 1; }
  if (IS("_aglSwapBuffers") && carbon_ui_swap_gl(unwrap(a[0]))) { *out = 0; return 1; }
  if (IS("_aglDestroyContext")) carbon_ui_release_gl(unwrap(a[0]));
  if (IS("_aglGetCurrentContext")) {
    *out = wrap(F(void *, void)());
    return 1;
  }
  if (IS("_aglGetCGLContext") || IS("_aglGetCGLPixelFormat")) {
    void *pointer = NULL;
    *out = F(uint8_t, void *, void **)(unwrap(a[0]), &pointer);
    if (a[1])
      *(uint32_t *)P(1) = objc_bridge32_guest_pointer(pointer);
    return 1;
  }
  if (IS("_aglChoosePixelFormat")) {
    const int *attributes=P(2);int window_attributes[128];
    if (lp32_title_is_cod_carbon(lp32_profile()->title) &&
        lp32_agl_window_attributes(attributes,128,window_attributes,128))
      attributes=window_attributes;
    *out = wrap(F(void *, const void *, int, const int *)(NULL, 0, attributes));
    return 1;
  }
  if (IS("_aglCreateContext")) {
    *out = wrap(F(void *, void *, void *)(unwrap(a[0]), unwrap(a[1])));
    return 1;
  }
  if (IS("_aglNextRendererInfo") || IS("_aglNextPixelFormat")) {
    *out = wrap(F(void *, void *)(unwrap(a[0])));
    return 1;
  }
  if (IS("_aglDescribeRenderer") || IS("_aglDescribePixelFormat") ||
      IS("_aglGetInteger") || IS("_aglSetInteger")) {
    *out = F(uint8_t, void *, uint32_t, void *)(unwrap(a[0]), a[1], P(2));
    return 1;
  }
  if (IS("_aglDestroyPixelFormat") || IS("_aglDestroyRendererInfo") ||
      IS("_aglSwapBuffers")) {
    F(void, void *)(unwrap(a[0]));
    *out = 0;
    return 1;
  }
  if (IS("_aglSetCurrentContext") || IS("_aglDestroyContext") ||
      IS("_aglUpdateContext")) {
    *out = F(uint8_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_aglEnable") || IS("_aglDisable")) {
    *out = F(uint8_t, void *, uint32_t)(unwrap(a[0]), a[1]);
    return 1;
  }
  if (IS("_aglGetVersion")) {
    F(void, void *, void *)(P(0), P(1));
    *out = 0;
    return 1;
  }
  return 0;
#undef IS
#undef P
#undef F
}
int carbon_bridge32_dispatch(const char *name, const uint32_t *a,
                             uint64_t *out) {
#define IS(s) (!strcmp(name, s))
#define P(i) ((void *)(uintptr_t)a[i])
#define F(type, ...) ((type (*)(__VA_ARGS__))function)
  if (name[0] != '_') {
    char normalized[128];
    if (strlen(name) + 2 > sizeof(normalized))
      return 0;
    snprintf(normalized, sizeof(normalized), "_%s", name);
    return carbon_bridge32_dispatch(normalized, a, out);
  }
  if (agl_dispatch(name, a, out))
    return 1;
  if (IS("_FSpOpenDF")) {
    FSRef ref;
    int16_t status = ref_from_spec(P(0), &ref);
    unsigned slot;
    for (slot = 1; slot < 1024 && classic_files[slot]; ++slot) {}
    if (!a[2]) status = paramErr;
    if (!status && slot == 1024) status = tmfoErr;
    FSIORefNum file = 0;
    if (!status) status = ((OSErr (*)(const FSRef *, UniCharCount, const UniChar *, SInt8, FSIORefNum *))symbol("_FSOpenFork"))(&ref, 0, NULL, (int8_t)a[1], &file);
    if (!status) classic_files[slot] = file;
    if (a[2]) *(int16_t *)P(2) = status ? 0 : (int16_t)slot;
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_FSClose") || IS("_FSRead") || IS("_GetFPos") || IS("_SetFPos")) {
    unsigned slot = (uint16_t)a[0];
    if (!slot || slot >= 1024 || !classic_files[slot]) { *out = (uint32_t)rfNumErr; return 1; }
  }
  if (IS("_FSClose")) {
    *out = (uint32_t)(int32_t)((OSErr (*)(FSIORefNum))symbol("_FSCloseFork"))(classic_files[(uint16_t)a[0]]);
    if (!*out) classic_files[(uint16_t)a[0]] = 0;
    return 1;
  }
  if (IS("_FSRead")) {
    if (!a[1] || *(int32_t *)P(1) < 0) { *out = (uint32_t)paramErr; return 1; }
    ByteCount actual = 0;
    *out = (uint32_t)(int32_t)((OSErr (*)(FSIORefNum, UInt16, SInt64, ByteCount, void *, ByteCount *))symbol("_FSReadFork"))(classic_files[(uint16_t)a[0]], fsAtMark, 0,
        *(uint32_t *)P(1), P(2), &actual);
    *(uint32_t *)P(1) = (uint32_t)actual;
    return 1;
  }
  if (IS("_GetFPos")) {
    SInt64 position = 0;
    OSStatus status = ((OSErr (*)(FSIORefNum, SInt64 *))symbol("_FSGetForkPosition"))(classic_files[(uint16_t)a[0]], &position);
    if (!status && position > INT32_MAX) status = posErr;
    if (!status && a[1]) *(int32_t *)P(1) = (int32_t)position;
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_SetFPos")) {
    *out = (uint32_t)(int32_t)((OSErr (*)(FSIORefNum, UInt16, SInt64))symbol("_FSSetForkPosition"))(classic_files[(uint16_t)a[0]], (uint16_t)a[1], (int32_t)a[2]);
    return 1;
  }
  if (IS("_GetScriptVariable")) {
    *out = (uint32_t)((long (*)(int16_t,int16_t))symbol("_GetScriptVariable"))((int16_t)a[0], (int16_t)a[1]);
    return 1;
  }
  if (IS("_KBGetLayoutType")) {
    *out = ((uint32_t (*)(int16_t))symbol("_KBGetLayoutType"))((int16_t)a[0]);
    return 1;
  }
  if (IS("_AbsoluteToNanoseconds") || IS("_NanosecondsToAbsolute")) {
    mach_timebase_info_data_t scale;
    mach_timebase_info(&scale);
    uint64_t value = (uint64_t)a[0] | (uint64_t)a[1] << 32;
    *out = IS("_AbsoluteToNanoseconds") ?
        (uint64_t)((__uint128_t)value * scale.numer / scale.denom) :
        (uint64_t)((__uint128_t)value * scale.denom / scale.numer);
    return 1;
  }
  if (IS("_QDLocalToGlobalRect") || IS("_QDGlobalToLocalRect") ||
      IS("_QDLocalToGlobalPoint") || IS("_QDGlobalToLocalPoint")) {
    struct carbon_ref *port = reference(a[0]);
    if (!port || !a[1]) { *out = (uint32_t)paramErr; return 1; }
    unsigned points = strstr(name, "Rect") ? 2 : 1;
    if (carbon_ui_convert_game_point(port->host, P(1), strstr(name, "GlobalToLocal") != NULL)) {
      if (points == 2) carbon_ui_convert_game_point(port->host, (int16_t *)P(1) + 2, strstr(name, "GlobalToLocal") != NULL);
      *out = 0; return 1;
    }
    Rect bounds;
    *out = (uint32_t)((int32_t (*)(void *, uint32_t, Rect *))symbol("_GetWindowBounds"))(
        port->host, kWindowContentRgn, &bounds);
    if (!*out) {
      int direction = strstr(name, "GlobalToLocal") ? -1 : 1;
      int16_t *point = P(1);
      unsigned count = strstr(name, "Rect") ? 2 : 1;
      if (getenv("LP32_TRACE_INPUT")) {
        static unsigned samples;
        if (!(samples++ % 120)) fprintf(stderr, "compat32: QD convert %s point=%d,%d bounds=%d,%d,%d,%d port=%08x\n", name, point[1], point[0], bounds.left, bounds.top, bounds.right, bounds.bottom, a[0]);
      }
      for (unsigned i = 0; i < count; ++i) {
        point[i * 2] += direction * bounds.top;
        point[i * 2 + 1] += direction * bounds.left;
      }
    }
    return 1;
  }
  if (IS("_CreateNewPortForCGDisplayID")) {
    int32_t size[2];
    objc_bridge32_display_size(a[0], size);
    if (size[0] <= 0 || size[1] <= 0 || size[0] > 16384 || size[1] > 16384) { *out = 0; return 1; }
    Rect bounds = {0, 0, (int16_t)size[1], (int16_t)size[0]};
    void *window = NULL;
    int32_t status = ((int32_t (*)(uint32_t, uint32_t, const Rect *, void **))symbol("_CreateNewWindow"))(
        13, kWindowCompositingAttribute, &bounds, &window);
    *out = status ? 0 : wrap(window);
    if (!status) {
      reference((uint32_t)*out)->display_port = true;
      carbon_ui_fullscreen(window, a[0], size[0], size[1]);
    }
    return 1;
  }
  if (IS("_GetPortPixMap")) {
    struct carbon_ref *port = reference(a[0]);
    if (!port) return 0;
    if (!port->port_pixmap) {
      port->port_pixmap = compat_runtime32_allocate(4 + sizeof(struct pixmap32), 1);
      if (port->port_pixmap) {
        *(uint32_t *)(uintptr_t)port->port_pixmap = port->port_pixmap + 4;
        struct pixmap32 *map = (void *)(uintptr_t)(port->port_pixmap + 4);
        Rect bounds;
        ((int32_t (*)(void *, uint32_t, Rect *))symbol("_GetWindowBounds"))(port->host, kWindowContentRgn, &bounds);
        map->bounds = (Rect){0, 0, bounds.bottom - bounds.top, bounds.right - bounds.left};
        map->row_bytes = (int16_t)(0x8000 | (map->bounds.right * 4 & 0x3fff));
        map->pixel_type = 16; map->pixel_size = 32; map->components = 3; map->component_size = 8;
        map->hres = map->vres = 72 << 16;
      }
    }
    *out = port->port_pixmap;
    return 1;
  }
  if (IS("_DisposePort")) {
    struct carbon_ref *port = reference(a[0]);
    if (!port || !port->display_port) return 0;
    carbon_ui_dispose(port->host);
    ((void (*)(void *))symbol("_DisposeWindow"))(port->host);
    if (port->port_pixmap) compat_runtime32_deallocate(port->port_pixmap);
    port->host = NULL; port->port_pixmap = 0;
    if (current_port == a[0]) current_port = 0;
    *out = 0;
    return 1;
  }
  struct carbon_ref *port = reference(current_port);
  if (port && IS("_ForeColor")) {
    uint32_t rgb;
    switch (a[0]) {
      case 30: rgb = 0xffffff; break;
      case 33: rgb = 0; break;
      case 69: rgb = 0xffff00; break;
      case 137: rgb = 0xff00ff; break;
      case 205: rgb = 0xff0000; break;
      case 273: rgb = 0x00ffff; break;
      case 341: rgb = 0x00ff00; break;
      case 409: rgb = 0x0000ff; break;
      default: return 0;
    }
    for (unsigned i = 0; i < 3; ++i) port->foreground_color[i] = ((rgb >> (16 - i * 8)) & 255) * 257;
    *out = 0;
    return 1;
  }
  if (port && (IS("_PaintRect") || IS("_EraseRect")) && a[0]) {
    uint32_t scratch = compat_runtime32_allocate(4, 1);
    uint32_t context_args[] = {current_port, scratch};
    if (!scratch) { *out = (uint32_t)memFullErr; return 1; }
    carbon_bridge32_dispatch("_QDBeginCGContext", context_args, out);
    if (!*out) {
      CGContextRef context = objc_bridge32_host_object(*(uint32_t *)(uintptr_t)scratch);
      const Rect *rect = P(0);
      const uint16_t *color = IS("_EraseRect") ? port->background_color : port->foreground_color;
      CGContextSetRGBFillColor(context, color[0] / 65535.0, color[1] / 65535.0, color[2] / 65535.0, 1);
      CGContextFillRect(context, CGRectMake(rect->left, CGBitmapContextGetHeight(context) - rect->bottom,
          rect->right - rect->left, rect->bottom - rect->top));
      carbon_bridge32_dispatch("_QDEndCGContext", context_args, out);
    }
    compat_runtime32_deallocate(scratch);
    return 1;
  }
  if (IS("_QDBeginCGContext") || IS("_QDEndCGContext")) {
    /* QuickDraw ports no longer exist. Keep the window's Quartz contents in
       a bitmap and redraw them through its compositing HIView. */
    struct carbon_ref *window = reference(a[0]);
    if (!window || !a[1]) { *out = (uint32_t)paramErr; return 1; }
    uint32_t *guest_context = P(1);
    if (IS("_QDBeginCGContext")) {
      Rect bounds;
      *out = (uint32_t)((int32_t (*)(void *, uint32_t, Rect *))symbol("_GetWindowBounds"))(
          window->host, kWindowContentRgn, &bounds);
      if (*out) return 1;
      size_t width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
      if (!width || !height || width > 16384 || height > 16384) {
        *out = (uint32_t)paramErr; return 1;
      }
      CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
      CGContextRef context = CGBitmapContextCreate(NULL, width, height, 8, width * 4,
          colors, kCGImageAlphaPremultipliedLast);
      CGColorSpaceRelease(colors);
      if (context && window->background_image)
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), window->background_image);
      *guest_context = objc_bridge32_guest_object(context);
      if (context) CGContextRelease(context);
      *out = context ? 0 : (uint32_t)memFullErr;
    } else {
      CGContextRef context = objc_bridge32_host_object(*guest_context);
      if (!context) { *out = (uint32_t)paramErr; return 1; }
      if (window->background_image) CGImageRelease(window->background_image);
      window->background_image = CGBitmapContextCreateImage(context);
      uint64_t ignored;
      objc_bridge32_dispatch("_CGContextRelease", guest_context, &ignored);
      *guest_context = 0;
      void *root = ((void *(*)(void *))symbol("_HIViewGetRoot"))(window->host);
      if (!window->background_handler) {
        EventTypeSpec type = {kEventClassControl, kEventControlDraw};
        EventTargetRef target = ((EventTargetRef (*)(void *))symbol("_GetControlEventTarget"))(root);
        InstallEventHandler(target, draw_window_background, 1, &type, window,
                            &window->background_handler);
      }
      if (lp32_title_is_cod_carbon(lp32_profile()->title))
        carbon_ui_paint(window->host, window->background_image);
      else carbon_ui_use_native(window->host);
      ((int32_t (*)(void *, bool))symbol("_HIViewSetNeedsDisplay"))(root, true);
      *out = 0;
    }
    return 1;
  }
  if (IS("_SetWindowContentColor") && !symbol(name)) {
    struct carbon_ref *window = reference(a[0]);
    if (!window || !a[1]) { *out = (uint32_t)paramErr; return 1; }
    memcpy(window->background_color, P(1), 6);
    if (!window->background_handler) {
      EventTypeSpec type = {kEventClassControl, kEventControlDraw};
      void *root = ((void *(*)(void *))symbol("_HIViewGetRoot"))(window->host);
      EventTargetRef target = ((EventTargetRef (*)(void *))symbol("_GetControlEventTarget"))(root);
      *out = (uint32_t)InstallEventHandler(target, draw_window_background, 1, &type,
                                         window, &window->background_handler);
    } else *out = 0;
    return 1;
  }
  if (IS("_SetWRefCon") || IS("_GetWRefCon")) {
    struct carbon_ref *window = reference(a[0]);
    if (!window) { *out = 0; return 1; }
    if (IS("_SetWRefCon")) window->guest_refcon = (int32_t)a[1];
    *out = IS("_GetWRefCon") ? (uint32_t)window->guest_refcon : 0;
    return 1;
  }
  if (IS("_NewMenu")) {
    void *menu = NULL;
    int32_t (*create)(int16_t, uint32_t, void **) = symbol("_CreateNewMenu");
    int32_t status = create((int16_t)a[0], 0, &menu);
    if (!status && a[1]) {
      CFStringRef title = CFStringCreateWithPascalString(NULL, P(1), kCFStringEncodingMacRoman);
      ((int32_t (*)(void *, CFStringRef))symbol("_SetMenuTitleWithCFString"))(menu, title);
      if (title) CFRelease(title);
    }
    *out = status ? 0 : wrap(menu);
    return 1;
  }
  if (IS("_InvalMenuBar")) {
    ((void (*)(void))symbol("_DrawMenuBar"))();
    *out = 0;
    return 1;
  }
  if (IS("_GetCurrentEventKeyModifiers")) {
    *out = (getenv("LP32_BACKGROUND_TEST") || lp32_suppress_background_input()) ?
        0 : GetCurrentKeyModifiers();
    return 1;
  }
  if (IS("_EventAvail") && a[0] == 0) {
    /* A zero event mask only initializes/queries the legacy event system;
       it must not remove events from Carbon's application queue. */
    if (a[1]) {
      unsigned char record[16] = {0};
      uint32_t ticks = (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 16666667);
      uint16_t modifiers = (uint16_t)GetCurrentKeyModifiers();
      memcpy(record + 6, &ticks, 4);
      memcpy(record + 14, &modifiers, 2);
      memcpy(P(1), record, sizeof(record));
    }
    *out = 0;
    return 1;
  }
  if (IS("_ShowSheetWindow")) {
    *out = (uint32_t)carbon_ui_show_sheet(unwrap(a[0]), unwrap(a[1]));
    return 1;
  }
  if (IS("_HideSheetWindow")) {
    *out = (uint32_t)carbon_ui_hide_sheet(unwrap(a[0]));
    return 1;
  }
  if (IS("_ICStart")) {
    void *instance = NULL;
    *out = (uint32_t)((int32_t (*)(void **, uint32_t))symbol(name))(&instance,
                                                                    a[1]);
    if (a[0])
      *(uint32_t *)P(0) = wrap(instance);
    return 1;
  }
  if (IS("_ICGetPref")) {
    long size = a[4] ? *(int32_t *)P(4) : 0;
    *out = (uint32_t)((int32_t (*)(void *, const void *, uint32_t *, void *,
                                   long *))symbol(name))(unwrap(a[0]), P(1),
                                                         P(2), P(3), &size);
    if (a[4])
      *(int32_t *)P(4) = (int32_t)size;
    return 1;
  }
  if (IS("_ICStop")) {
    *out = (uint32_t)((int32_t (*)(void *))symbol(name))(unwrap(a[0]));
    return 1;
  }
  if (IS("_ICLaunchURL")) {
    long start = a[4] ? *(int32_t *)P(4) : 0;
    long end = a[5] ? *(int32_t *)P(5) : 0;
    *out = (uint32_t)((int32_t (*)(void *, const void *, const void *, long,
                                   long *, long *))symbol(name))(
        unwrap(a[0]), P(1), P(2), (int32_t)a[3], &start, &end);
    if (a[4])
      *(int32_t *)P(4) = (int32_t)start;
    if (a[5])
      *(int32_t *)P(5) = (int32_t)end;
    return 1;
  }
  if (name[0] != '_' || !strchr("FGIRQSCADTEMHPONUXY", name[1]))
    return 0;
  if (getenv("LP32_TRACE_CARBON"))
    fprintf(stderr, "compat32: Carbon %s %#x %#x %#x %#x\n", name, a[0], a[1],
            a[2], a[3]);
  if (IS("_OffsetRect") || IS("_InsetRect")) {
    Rect *rect = P(0);
    int16_t h = (int16_t)a[1], v = (int16_t)a[2];
    rect->left += h;
    rect->top += v;
    rect->right += IS("_OffsetRect") ? h : -h;
    rect->bottom += IS("_OffsetRect") ? v : -v;
    *out = 0;
    return 1;
  }
  if (IS("_SetRect")) {
    *(Rect *)P(0) =
        (Rect){(int16_t)a[2], (int16_t)a[1], (int16_t)a[4], (int16_t)a[3]};
    *out = 0;
    return 1;
  }
  if (IS("_EqualRect")) {
    *out = memcmp(P(0), P(1), sizeof(Rect)) == 0;
    return 1;
  }
  if (IS("_EmptyRect")) {
    const Rect *r = P(0);
    *out = r->right <= r->left || r->bottom <= r->top;
    return 1;
  }
  if (IS("_MapRect")) {
    Rect source = *(Rect *)P(1), dest = *(Rect *)P(2), rect = *(Rect *)P(0);
    int32_t sw = source.right - source.left, sh = source.bottom - source.top,
            dw = dest.right - dest.left, dh = dest.bottom - dest.top;
    if (sw) {
      rect.left =
          (int16_t)(dest.left + (int64_t)(rect.left - source.left) * dw / sw);
      rect.right =
          (int16_t)(dest.left + (int64_t)(rect.right - source.left) * dw / sw);
    }
    if (sh) {
      rect.top =
          (int16_t)(dest.top + (int64_t)(rect.top - source.top) * dh / sh);
      rect.bottom =
          (int16_t)(dest.top + (int64_t)(rect.bottom - source.top) * dh / sh);
    }
    *(Rect *)P(0) = rect;
    *out = 0;
    return 1;
  }
  if (IS("_PtInRect")) {
    Point point;
    memcpy(&point, a, 4);
    const Rect *r = P(1);
    *out = point.h >= r->left && point.h < r->right && point.v >= r->top &&
           point.v < r->bottom;
    return 1;
  }
  if (IS("_UnionRect")) {
    const Rect *x = P(0), *y = P(1);
    Rect *r = P(2);
    if (x->right <= x->left || x->bottom <= x->top)
      *r = *y;
    else if (y->right <= y->left || y->bottom <= y->top)
      *r = *x;
    else
      *r = (Rect){x->top < y->top ? x->top : y->top,
                  x->left < y->left ? x->left : y->left,
                  x->bottom > y->bottom ? x->bottom : y->bottom,
                  x->right > y->right ? x->right : y->right};
    *out = 0;
    return 1;
  }
  if (IS("_GetIconRefFromFile")) {
    FSRef ref;
    void *icon = NULL;
    int16_t label = 0;
    int32_t status = ref_from_spec(P(0), &ref);
    if (!status)
      status = ((int32_t (*)(const FSRef *, UniCharCount, const UniChar *,
                             uint32_t, const void *, uint32_t, void **,
                             int16_t *))symbol("_GetIconRefFromFileInfo"))(
          &ref, 0, NULL, 0, NULL, 0, &icon, &label);
    if (a[1])
      *(uint32_t *)P(1) = wrap(icon);
    if (a[2])
      *(int16_t *)P(2) = label;
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_GetThemeTextDimensions")) {
    HIThemeTextInfo info = {0};
    info.version = 1;
    info.fontID = a[1];
    info.state = a[2];
    Point *bounds = P(4);
    CGFloat width = 0, height = 0, baseline = 0;
    int32_t status = ((
        int32_t (*)(CFTypeRef, CGFloat, HIThemeTextInfo *, CGFloat *, CGFloat *,
                    CGFloat *))symbol("_HIThemeGetTextDimensions"))(
        objc_bridge32_host_object(a[0]), a[3] && bounds ? bounds->h : 0, &info,
        &width, &height, &baseline);
    if (!status) {
      if (bounds) {
        bounds->h = (int16_t)ceil(width);
        bounds->v = (int16_t)ceil(height);
      }
      if (a[5])
        *(int16_t *)P(5) = (int16_t)ceil(baseline);
    }
    if (getenv("LP32_TRACE_CARBON"))
      fprintf(stderr,
              "compat32: theme dimensions status=%d width=%g height=%g "
              "baseline=%g\n",
              status, width, height, baseline);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_InvalWindowRect")) {
    void *root = ((void *(*)(void *))symbol("_HIViewGetRoot"))(unwrap(a[0]));
    *out = (uint32_t)((int32_t (*)(void *, bool))symbol(
        "_HIViewSetNeedsDisplay"))(root, true);
    return 1;
  }
  if (IS("_UpdateControls")) {
    void *window = unwrap(a[0]);
    void *root = ((void *(*)(void *))symbol("_HIViewGetRoot"))(window);
    ((int32_t (*)(void *, bool))symbol("_HIViewSetNeedsDisplay"))(root, true);
    ((int32_t (*)(void *))symbol("_HIWindowFlush"))(window);
    *out = 0;
    return 1;
  }
  if (IS("_GetControlPropertySize") && !symbol(name)) {
    ByteCount size = 0;
    int32_t status =
        ((int32_t (*)(void *, uint32_t, uint32_t, ByteCount, ByteCount *,
                      void *))symbol("_GetControlProperty"))(
            unwrap(a[0]), a[1], a[2], 0, &size, NULL);
    if (a[3])
      *(uint32_t *)P(3) = (uint32_t)size;
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_StandardAlert")) {
    const unsigned char *title = P(1), *message = P(2);
    fprintf(stderr, "compat32: Carbon alert: %.*s | %.*s\n",
            title ? title[0] : 0, title ? (const char *)title + 1 : "",
            message ? message[0] : 0, message ? (const char *)message + 1 : "");
    return 0;
  }
  if (IS("_GetPort")) {
    if (a[0])
      *(uint32_t *)P(0) = current_port;
    *out = 0;
    return 1;
  }
  if (IS("_SetPort") || IS("_SetPortWindowPort")) {
    current_port = a[0];
    *out = 0;
    return 1;
  }
  if (IS("_GetWindowPort")) {
    *out = a[0];
    return 1;
  }
  if (IS("_IsValidPort")) {
    *out = a[0] && unwrap(a[0]);
    return 1;
  }
  if (IS("_GetMainDevice") || IS("_GetDeviceList") ||
      IS("_DMGetFirstScreenDevice")) {
    initialize_displays();
    *out = display_count ? displays[0].handle : 0;
    if (IS("_GetMainDevice"))
      for (uint32_t i = 0; i < display_count; ++i)
        if (displays[i].id == CGMainDisplayID())
          *out = displays[i].handle;
    return 1;
  }
  if (IS("_GetAvailableWindowPositioningBounds")) {
    /* The GDHandle API is absent in 64-bit Carbon. Translate our guest
       display handle and use its display-ID/CGRect replacement. */
    initialize_displays();
    CGDirectDisplayID display = CGMainDisplayID();
    bool found = a[0] == 0;
    for (uint32_t i = 0; i < display_count; ++i) {
      if (displays[i].handle == a[0]) { display = displays[i].id; found = true; break; }
    }
    *out = (uint32_t)paramErr;
    if (!found || !a[1]) return 1;
    CGRect bounds;
    int32_t (*get_bounds)(CGDirectDisplayID, uint32_t, CGRect *) =
        symbol("_HIWindowGetAvailablePositioningBounds");
    if (!get_bounds) return 1;
    int32_t status = get_bounds(display, kHICoordSpaceScreenPixel, &bounds);
    if (!status) *(Rect *)P(1) = (Rect){(int16_t)CGRectGetMinY(bounds),
        (int16_t)CGRectGetMinX(bounds), (int16_t)CGRectGetMaxY(bounds), (int16_t)CGRectGetMaxX(bounds)};
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_GetNextDevice") || IS("_DMGetNextScreenDevice")) {
    *out = 0;
    for (uint32_t i = 0; i + 1 < display_count; ++i)
      if (displays[i].handle == a[0])
        *out = displays[i + 1].handle;
    return 1;
  }
  if (IS("_DMGetDisplayIDByGDevice") || IS("_DMGetGDeviceByDisplayID")) {
    initialize_displays();
    *out = (uint32_t)-50;
    for (uint32_t i = 0; i < display_count; ++i) {
      if ((IS("_DMGetDisplayIDByGDevice") ? displays[i].handle
                                          : displays[i].id) != a[0])
        continue;
      if (a[1])
        *(uint32_t *)P(1) = IS("_DMGetDisplayIDByGDevice") ? displays[i].id
                                                           : displays[i].handle;
      *out = 0;
      break;
    }
    return 1;
  }
  if (IS("_TestDeviceAttribute")) {
    *out = 0;
    for (uint32_t i = 0; i < display_count; ++i)
      if (displays[i].handle == a[0] && a[1] < 16) {
        struct device32 *d = (void *)(uintptr_t)*(uint32_t *)P(0);
        *out = ((uint16_t)d->flags >> a[1]) & 1;
      }
    return 1;
  }
  if (IS("_FSpMakeFSRef")) {
    *out = (uint32_t)(int32_t)ref_from_spec(P(0), P(1));
    return 1;
  }
  if (IS("_FSpOpenResFile")) {
    FSRef ref;
    unsigned char path[4096];
    int16_t status = ref_from_spec(P(0), &ref);
    if (!status)
      status = ((int16_t (*)(const FSRef *, void *, uint32_t))symbol(
          "_FSRefMakePath"))(&ref, path, sizeof(path));
    *out =
        (uint32_t)(int32_t)(status
                                ? -1
                                : resource_bridge32_open((const char *)path));
    return 1;
  }
  if (IS("_PBHGetVolParmsSync")) {
    unsigned char *pb = P(0);
    int16_t volume;
    uint32_t destination, capacity;
    memcpy(&volume, pb + 22, 2);
    memcpy(&destination, pb + 32, 4);
    memcpy(&capacity, pb + 36, 4);
    GetVolParmsInfoBuffer info = {0};
    int32_t (*get)(int16_t, void *, ByteCount) = symbol("_FSGetVolumeParms");
    int16_t status = get ? (int16_t)get(volume, &info, sizeof(info)) : -4;
    if (!status && destination) {
      unsigned char converted[32] = {0};
      uint32_t local = wrap(info.vMLocalHand);
      uint32_t device =
          info.vMDeviceID ? compat_runtime32_copy_cstring(info.vMDeviceID) : 0;
      uint32_t max_name = (uint32_t)info.vMMaxNameLength;
      memcpy(converted, &info.vMVersion, 2);
      memcpy(converted + 2, &info.vMAttrib, 4);
      memcpy(converted + 6, &local, 4);
      memcpy(converted + 10, &info.vMServerAdr, 4);
      memcpy(converted + 14, &info.vMVolumeGrade, 4);
      memcpy(converted + 18, &info.vMForeignPrivID, 2);
      memcpy(converted + 20, &info.vMExtendedAttributes, 4);
      memcpy(converted + 24, &device, 4);
      memcpy(converted + 28, &max_name, 4);
      uint32_t count =
          capacity < sizeof(converted) ? capacity : sizeof(converted);
      memcpy((void *)(uintptr_t)destination, converted, count);
      memcpy(pb + 40, &count, 4);
    }
    memcpy(pb + 16, &status, 2);
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_PBGetCatInfoSync")) {
    unsigned char *pb = P(0);
    if (!pb) { *out = (uint32_t)paramErr; return 1; }
    struct spec32 spec = {0};
    uint32_t name;
    int16_t index;
    memcpy(&name, pb + 18, 4);
    memcpy(&spec.volume, pb + 22, 2);
    memcpy(&index, pb + 28, 2);
    memcpy(&spec.parent, pb + 48, 4);
    int16_t status = index > 0 ? unimpErr : noErr;
    if (!index && name) {
      const unsigned char *text = (void *)(uintptr_t)name;
      if (text[0] > 63) status = bdNamErr;
      else memcpy(spec.name, text, text[0] + 1);
    }
    FSRef ref, parent;
    FSCatalogInfo info = {0};
    HFSUniStr255 leaf;
    if (!status) status = ref_from_spec(&spec, &ref);
    if (!status) {
      int16_t (*get)(const FSRef *, uint32_t, void *, void *, void *, void *) = symbol("_FSGetCatalogInfo");
      status = get(&ref, kFSCatInfoGettableInfo, &info, &leaf, NULL, &parent);
    }
    if (!status) {
      bool directory = (info.nodeFlags & kFSNodeIsDirectoryMask) != 0;
      pb[30] = directory ? 0x10 : 0;
      memcpy(pb + 32, info.finderInfo, 16);
      memcpy(pb + 48, &info.nodeID, 4);
      memcpy(pb + 100, &info.parentDirID, 4);
      if (directory) {
        uint16_t count = info.valence > UINT16_MAX ? UINT16_MAX : (uint16_t)info.valence;
        memcpy(pb + 52, &count, 2);
        remember_directory(info.volume, info.nodeID, &ref);
      }
      remember_directory(info.volume, info.parentDirID, &parent);
      if (name && index < 0) {
        CFStringRef text = CFStringCreateWithCharacters(NULL, leaf.unicode, leaf.length);
        if (!CFStringGetPascalString(text, (void *)(uintptr_t)name, 256, kCFStringEncodingMacRoman)) status = bdNamErr;
        CFRelease(text);
      }
    }
    memcpy(pb + 16, &status, 2);
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_FSMakeFSSpec")) {
    const unsigned char *name = P(2);
    struct spec32 *spec = P(3);
    if (!name || !spec) {
      *out = (uint32_t)-50;
      return 1;
    }
    int16_t status = -43;
    FSRef ref;
    if (a[0] || a[1]) {
      if (name[0] > 63) {
        *out = (uint32_t)-37;
        return 1;
      }
      spec->volume = (int16_t)a[0];
      spec->parent = a[1];
      memcpy(spec->name, name, name[0] + 1);
      status = ref_from_spec(spec, &ref);
      if (!status)
        status = spec_from_ref(&ref, spec);
    } else {
      CFStringRef string =
          CFStringCreateWithPascalString(NULL, name, kCFStringEncodingMacRoman);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      CFURLRef url = CFURLCreateWithFileSystemPath(
          NULL, string,
          memchr(name + 1, ':', name[0]) ? kCFURLHFSPathStyle
                                         : kCFURLPOSIXPathStyle,
          false);
#pragma clang diagnostic pop
      unsigned char path[4096];
      if (url &&
          CFURLGetFileSystemRepresentation(url, true, path, sizeof(path))) {
        int16_t (*make)(const UInt8 *, FSRef *, Boolean *) =
            symbol("_FSPathMakeRef");
        status = make(path, &ref, NULL);
        if (!status)
          status = spec_from_ref(&ref, spec);
        else if (status == -43) {
          char *slash = strrchr((char *)path, '/');
          if (slash && slash[1]) {
            CFStringRef leaf = CFStringCreateWithCString(NULL, slash + 1,
                                                         kCFStringEncodingUTF8);
            bool fits = leaf && CFStringGetPascalString(
                                    leaf, spec->name, sizeof(spec->name),
                                    kCFStringEncodingMacRoman);
            if (leaf)
              CFRelease(leaf);
            *slash = 0;
            if (fits && !make(path, &ref, NULL)) {
              FSCatalogInfo info;
              int16_t (*get)(const FSRef *, uint32_t, void *, void *, void *,
                             void *) = symbol("_FSGetCatalogInfo");
              if (!get(&ref, kFSCatInfoVolume | kFSCatInfoNodeID, &info, NULL,
                       NULL, NULL)) {
                spec->volume = info.volume;
                spec->parent = info.nodeID;
                remember_directory(info.volume, info.nodeID, &ref);
              }
            }
          }
        }
      }
      if (url)
        CFRelease(url);
      if (string)
        CFRelease(string);
    }
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_FindFolder")) {
    FSRef ref;
    int16_t status = find_folder32((int16_t)a[0], a[1], a[2] != 0, &ref);
    if (!status) {
      FSCatalogInfo info;
      int16_t (*get)(const FSRef *, uint32_t, void *, void *, void *, void *) =
          symbol("_FSGetCatalogInfo");
      status = get(&ref, kFSCatInfoVolume | kFSCatInfoNodeID, &info, NULL, NULL,
                   NULL);
      if (!status) {
        if (a[3])
          *(int16_t *)P(3) = info.volume;
        if (a[4])
          *(uint32_t *)P(4) = info.nodeID;
        remember_directory(info.volume, info.nodeID, &ref);
      }
    }
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_InitCursor") || IS("_ShowCursor") || IS("_HideCursor")) {
    if (lp32_title_is_cod_carbon(lp32_profile()->title))
      return objc_bridge32_dispatch(name, a, out);
    if (!getenv("LP32_BACKGROUND_TEST")) {
      void (*cursor)(void) = symbol(name);
      if (cursor)
        cursor();
    }
    *out = 0;
    return 1;
  }
  if (IS("_StringToNum")) {
    /* Not in 64-bit Carbon. Like the Toolbox routine: optional sign, then the
       low four bits of every character as a decimal digit, unvalidated. */
    const uint8_t *text = P(0);
    uint32_t value = 0;
    unsigned length = text ? text[0] : 0, i = 1;
    bool negative = length && text[1] == '-';
    if (length && (text[1] == '-' || text[1] == '+')) ++i;
    for (; i <= length; ++i) value = value * 10 + (text[i] & 0x0f);
    if (a[1]) *(uint32_t *)P(1) = negative ? 0 - value : value;
    *out = 0;
    return 1;
  }
  if (IS("_SetItemCmd")) {
    *out = (uint32_t)((int32_t (*)(void *, uint16_t, bool, uint16_t))symbol(
        "_SetMenuItemCommandKey"))(unwrap(a[0]), (uint16_t)a[1], false,
                                   (uint16_t)a[2]);
    return 1;
  }
  if (IS("_GetNewMBar")) {
    *out = resource_handle(0x4d424152, (int16_t)a[0], NULL);
    return 1;
  }
  if (IS("_SetMenuBar")) {
    uint64_t size = 0;
    resource_bridge32_dispatch("_GetHandleSize", a, &size);
    if (a[0] && size >= 2) {
      const unsigned char *p = (void *)(uintptr_t)*(uint32_t *)P(0);
      unsigned count = read_be16(p);
      if (count <= (size - 2) / 2) {
        ((void (*)(void))symbol("_ClearMenuBar"))();
        for (unsigned i = 0; i < count; ++i) {
          void *menu = menu_from_resource((int16_t)read_be16(p + 2 + i * 2));
          if (menu)
            ((void (*)(void *, int16_t))symbol("_InsertMenu"))(menu, 0);
        }
      }
    }
    *out = 0;
    return 1;
  }
  if ((IS("_CreateEditTextControl") || IS("_CreateEditUnicodeTextControl")) &&
      !symbol(name)) {
    void *view = NULL;
    int32_t status = create_container(unwrap(a[0]), P(1), &view);
    uint32_t token = wrap(view);
    if (!status)
      status = carbon_text_attach(view, token, objc_bridge32_host_object(a[2]),
                                  a[3] != 0);
    unsigned output_index = IS("_CreateEditTextControl") ? 6 : 5;
    if (a[output_index])
      *(uint32_t *)P(output_index) = token;
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateSliderControl") && !symbol(name)) {
    void *view = NULL;
    int32_t status =
        create_view(CFSTR("com.apple.HISlider"), unwrap(a[0]), P(1), &view);
    if (!status) {
      ((int32_t (*)(void *, int32_t))symbol("_HIViewSetMinimum"))(
          view, (int32_t)a[3]);
      ((int32_t (*)(void *, int32_t))symbol("_HIViewSetMaximum"))(
          view, (int32_t)a[4]);
      ((int32_t (*)(void *, int32_t))symbol("_HIViewSetValue"))(view,
                                                                (int32_t)a[2]);
      if (a[8]) {
        struct handler32 *handler = malloc(sizeof(*handler));
        if (!handler) {
          *out = (uint32_t)-108;
          return 1;
        }
        *handler = (struct handler32){a[8], 0};
        reference(wrap(view))->callback = handler;
        ((void (*)(void *, void *))symbol("_SetControlAction"))(view,
                                                                control_action);
      }
    }
    if (a[9])
      *(uint32_t *)P(9) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateChasingArrowsControl") && !symbol(name)) {
    void *view = NULL;
    int32_t status = create_view(CFSTR("com.apple.HIChasingArrows"),
                                 unwrap(a[0]), P(1), &view);
    if (a[2])
      *(uint32_t *)P(2) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateSeparatorControl") && !symbol(name)) {
    void *view = NULL;
    int32_t status = create_container(unwrap(a[0]), P(1), &view);
    if (!status) {
      EventTypeSpec event = {kEventClassControl, kEventControlDraw};
      void *target =
          ((void *(*)(void *))symbol("_GetControlEventTarget"))(view);
      status =
          InstallEventHandler(target, draw_separator, 1, &event, view, NULL);
    }
    if (a[2])
      *(uint32_t *)P(2) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateCheckGroupBoxControl") && !symbol(name)) {
    void *view = NULL;
    int32_t status = create_view(CFSTR("com.apple.HICheckBoxGroup"),
                                 unwrap(a[0]), P(1), &view);
    if (!status) {
      ((int32_t (*)(void *, void *))symbol("_SetControlTitleWithCFString"))(
          view, objc_bridge32_host_object(a[2]));
      ((int32_t (*)(void *, int32_t))symbol("_HIViewSetValue"))(view,
                                                                (int32_t)a[3]);
      ((int32_t (*)(void *, uint64_t, uint64_t))symbol(
          "_HIViewChangeFeatures"))(view, a[5] ? kControlAutoToggles : 0,
                                    a[5] ? 0 : kControlAutoToggles);
    }
    if (a[6])
      *(uint32_t *)P(6) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreatePopupButtonControl") && !symbol(name)) {
    void *view = NULL;
    int32_t status = create_view(CFSTR("com.apple.HIPopupButton"), unwrap(a[0]),
                                 P(1), &view);
    if (!status) {
      ((int32_t (*)(void *, void *))symbol("_SetControlTitleWithCFString"))(
          view, objc_bridge32_host_object(a[2]));
      void *menu = NULL;
      if ((int16_t)a[3] != -12345)
        menu = menu_from_resource((int16_t)a[3]);
      if (menu)
        status = ((int32_t (*)(void *, int16_t, uint32_t, ByteCount,
                               const void *))symbol("_SetControlData"))(
            view, 0, 0x6d68616e, sizeof(menu), &menu);
    }
    if (a[8])
      *(uint32_t *)P(8) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateRoundButtonControl") && !symbol(name)) {
    void *view = NULL;
    int32_t status = create_view(CFSTR("com.apple.HIRoundButton"), unwrap(a[0]),
                                 P(1), &view);
    if (!status && a[3]) {
      uint32_t args[] = {wrap(view), 0, 0x636f6e74, 6, a[3]};
      uint64_t result;
      if (!carbon_bridge32_dispatch("_SetControlData", args, &result))
        return 0;
      status = (int32_t)result;
    }
    if (a[4])
      *(uint32_t *)P(4) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateBevelButtonControl") && !symbol(name)) {
    if (a[4] || a[6])
      return 0;
    void *view = NULL;
    int32_t status = create_view(CFSTR("com.apple.HIBevelButton"), unwrap(a[0]),
                                 P(1), &view);
    if (!status) {
      ((int32_t (*)(void *, void *))symbol("_SetControlTitleWithCFString"))(
          view, objc_bridge32_host_object(a[2]));
      if (a[5] && *(int16_t *)P(5)) {
        fprintf(stderr, "compat32: bevel content type=%d requires conversion\n",
                *(int16_t *)P(5));
        return 0;
      }
    }
    if (a[9])
      *(uint32_t *)P(9) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateUserPaneControl") && !symbol(name)) {
    void *view = NULL;
    *out = (uint32_t)create_container(unwrap(a[0]), P(1), &view);
    if (a[3])
      *(uint32_t *)P(3) = wrap(view);
    return 1;
  }
  if (IS("_CreateGroupBoxControl") && !symbol(name)) {
    void *view = NULL;
    int32_t status = create_container(unwrap(a[0]), P(1), &view);
    CFStringRef title = objc_bridge32_host_object(a[2]);
    if (!status && title && CFStringGetLength(title)) {
      const Rect *bounds = P(1);
      Rect label_bounds = {0, 8, 20,
                           (int16_t)(bounds->right - bounds->left - 8)};
      void *label = NULL;
      status = ((int32_t (*)(void *, const Rect *, CFStringRef, const void *,
                             void **))symbol("_CreateStaticTextControl"))(
          unwrap(a[0]), &label_bounds, title, NULL, &label);
      if (!status)
        ((int32_t (*)(void *, void *))symbol("_HIViewAddSubview"))(view, label);
    }
    if (a[4])
      *(uint32_t *)P(4) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateTabsControl") && !symbol(name)) {
    struct __attribute__((packed, aligned(2))) tab32 {
      uint32_t icon, title;
      uint8_t enabled, pad;
    };
    const struct tab32 *tabs = P(5);
    unsigned count = (uint16_t)a[4];
    fprintf(stderr, "compat32: Carbon tabs count=%u\n", count);
    if (count > 64) {
      *out = (uint32_t)-50;
      return 1;
    }
    void *view = NULL;
    int32_t status = create_container(unwrap(a[0]), P(1), &view);
    if (status) {
      *out = (uint32_t)status;
      return 1;
    }
    const Rect *bounds = P(1);
    int width = (bounds->right - bounds->left) / (int)(count ? count : 1);
    for (unsigned i = 0; i < count; ++i) {
      Rect rect = {0, (int16_t)(i * width), 26, (int16_t)((i + 1) * width)};
      void *button = NULL;
      status = ((int32_t (*)(void *, const Rect *, CFStringRef, void **))symbol(
          "_CreatePushButtonControl"))(unwrap(a[0]), &rect,
                                       objc_bridge32_host_object(tabs[i].title),
                                       &button);
      if (status)
        break;
      ((int32_t (*)(void *, void *))symbol("_HIViewAddSubview"))(view, button);
      if (!tabs[i].enabled)
        ((int32_t (*)(void *))symbol("_DisableControl"))(button);
      struct tab_button *context = malloc(sizeof(*context));
      if (!context) {
        status = -108;
        break;
      }
      *context = (struct tab_button){view, (int32_t)i + 1};
      EventTypeSpec event = {kEventClassControl, kEventControlHit};
      void *target =
          ((void *(*)(void *))symbol("_GetControlEventTarget"))(button);
      status = ((int32_t (*)(void *, void *, uint32_t, const void *, void *,
                             void **))symbol("_InstallEventHandler"))(
          target, tab_hit, 1, &event, context, NULL);
      if (status) {
        free(context);
        break;
      }
    }
    ((int32_t (*)(void *, int32_t))symbol("_HIViewSetMinimum"))(view,
                                                                count ? 1 : 0);
    ((int32_t (*)(void *, int32_t))symbol("_HIViewSetMaximum"))(view, count);
    ((int32_t (*)(void *, int32_t))symbol("_HIViewSetValue"))(view,
                                                              count ? 1 : 0);
    if (a[6])
      *(uint32_t *)P(6) = wrap(view);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_EmbedControl")) {
    *out = (uint32_t)((int32_t (*)(void *, void *))symbol("_HIViewAddSubview"))(
        unwrap(a[1]), unwrap(a[0]));
    return 1;
  }
  if (IS("_SetControlBounds")) {
    const Rect *bounds = P(1);
    CGRect frame =
        CGRectMake(bounds->left, bounds->top, bounds->right - bounds->left,
                   bounds->bottom - bounds->top);
    control_port_rect(unwrap(a[0]), &frame, true);
    ((int32_t (*)(void *, const CGRect *))symbol("_HIViewSetFrame"))(
        unwrap(a[0]), &frame);
    if (getenv("LP32_TRACE_CARBON"))
      fprintf(stderr, "compat32: bounds %#x x=%g y=%g w=%g h=%g\n", a[0],
              frame.origin.x, frame.origin.y, frame.size.width,
              frame.size.height);
    *out = 0;
    return 1;
  }
  const char *native_name = name;
  if (IS("_CountSubControls")) {
    uint32_t count = 0;
    void *view =
        ((void *(*)(void *))symbol("_HIViewGetFirstSubview"))(unwrap(a[0]));
    while (view && count < 65535) {
      ++count;
      view = ((void *(*)(void *))symbol("_HIViewGetNextView"))(view);
    }
    if (a[1])
      *(uint16_t *)P(1) = (uint16_t)count;
    *out = 0;
    return 1;
  }
  if (IS("_IsControlEnabled"))
    native_name = "_HIViewIsEnabled";
  if (IS("_GetControl32BitValue"))
    native_name = "_HIViewGetValue";
  if (IS("_GetControl32BitMinimum"))
    native_name = "_HIViewGetMinimum";
  if (IS("_GetControl32BitMaximum"))
    native_name = "_HIViewGetMaximum";
  if (IS("_SetControl32BitValue"))
    native_name = "_HIViewSetValue";
  if (IS("_SetControl32BitMinimum"))
    native_name = "_HIViewSetMinimum";
  if (IS("_SetControl32BitMaximum"))
    native_name = "_HIViewSetMaximum";
  if (IS("_SetControlCommandID"))
    native_name = "_HIViewSetCommandID";
  if (IS("_Enqueue") || IS("_Dequeue")) {
    /* Classic QHdr is packed to two-byte alignment, with 32-bit links. */
    static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
    unsigned char *header = P(1);
    uint32_t head, tail;
    if (!a[0] || !header) { *out = (uint32_t)-1; return 1; }
    pthread_mutex_lock(&queue_lock);
    memcpy(&head, header + 2, 4); memcpy(&tail, header + 6, 4);
    *out = 0;
    if (IS("_Enqueue")) {
      *(uint32_t *)P(0) = 0;
      if (tail) *(uint32_t *)(uintptr_t)tail = a[0]; else head = a[0];
      tail = a[0];
    } else {
      uint32_t previous = 0, cursor = head;
      while (cursor && cursor != a[0]) { previous = cursor; cursor = *(uint32_t *)(uintptr_t)cursor; }
      if (!cursor) *out = (uint32_t)-1;
      else {
        uint32_t next = *(uint32_t *)(uintptr_t)cursor;
        if (previous) *(uint32_t *)(uintptr_t)previous = next; else head = next;
        if (tail == cursor) tail = previous;
      }
    }
    memcpy(header + 2, &head, 4); memcpy(header + 6, &tail, 4);
    pthread_mutex_unlock(&queue_lock);
    return 1;
  }
  if (IS("_HIWindowCreate")) {
    /* HIWindowCreate was removed from LP64 Carbon. Translate its attribute
     * list and floating bounds to the surviving standard-window API. */
    const int32_t *bits = P(1);
    const float *bounds = P(4);
    uint32_t attributes = 0;
    if (!bounds || !a[5] || a[2] || (a[3] != 1 && a[3] != 2)) {
      *out = (uint32_t)paramErr;
      return 1;
    }
    for (unsigned i = 0; bits && i < 128 && bits[i]; ++i) {
      if (bits[i] < 1 || bits[i] > 32) {
        *out = (uint32_t)unimpErr;
        return 1;
      }
      attributes |= 1u << (bits[i] - 1);
    }
    Rect rect = {(int16_t)lroundf(bounds[1]), (int16_t)lroundf(bounds[0]),
                 (int16_t)lroundf(bounds[1] + bounds[3]),
                 (int16_t)lroundf(bounds[0] + bounds[2])};
    void *window = NULL;
    int32_t status = ((int32_t (*)(uint32_t, uint32_t, const Rect *, void **))
        symbol("_CreateNewWindow"))(a[0], attributes | kWindowCompositingAttribute,
                                   &rect, &window);
    *(uint32_t *)P(5) = wrap(window);
    *out = (uint32_t)status;
    return 1;
  }
  void *function = NULL;
  /* Resolve only after matching an implemented native operation, before its
     arguments/temporary allocations have side effects. Foreign imports must
     never search Carbon's dependency tree just to return unhandled. Keep the
     original missing-export fallback for every matched operation. */
#undef IS
#define IS(s) ({ \
    bool matches = !strcmp(name, s); \
    if (matches && !function) { \
      function = symbol(native_name); if (!function) return 0; \
    } \
    matches; \
  })
  if (IS("_HIGetMousePosition")) {
    HIPoint point;
    void *object = unwrap(a[1]);
    HIPoint *value = F(HIPoint *, uint32_t, void *, HIPoint *)(a[0], object, &point);
    if (value && a[2]) {
      float *guest = P(2); guest[0] = point.x; guest[1] = point.y;
      *out = a[2];
    } else *out = 0;
    return 1;
  }
  if (IS("_GetStandardAlertDefaultParams")) {
    AlertStdCFStringAlertParamRec params = {0};
    *out = (uint32_t)F(int32_t, void *, uint32_t)(&params, a[1]);
    if (!*out && a[0]) {
      struct alert_params32 guest = {.version = params.version,
                                     .movable = params.movable,
                                     .help = params.helpButton,
                                     .default_button = params.defaultButton,
                                     .cancel_button = params.cancelButton,
                                     .position = params.position,
                                     .flags = params.flags};
      CFStringRef texts[] = {params.defaultText, params.cancelText,
                             params.otherText};
      uint32_t tokens[3];
      for (unsigned i = 0; i < 3; ++i)
        tokens[i] = (intptr_t)texts[i] == -1
                        ? UINT32_MAX
                        : objc_bridge32_guest_object((void *)texts[i]);
      guest.default_text = tokens[0];
      guest.cancel_text = tokens[1];
      guest.other_text = tokens[2];
      guest.icon = wrap(params.icon);
      memcpy(P(0), &guest, a[1] == 1 ? 28 : sizeof(guest));
    }
    return 1;
  }
  if (IS("_CreateStandardAlert") || IS("_CreateStandardSheet")) {
    char title[1024] = {0}, message[2048] = {0};
    CFStringRef title_string = objc_bridge32_host_object(a[1]);
    CFStringRef message_string = objc_bridge32_host_object(a[2]);
    if (title_string) CFStringGetCString(title_string, title, sizeof(title), kCFStringEncodingUTF8);
    if (message_string) CFStringGetCString(message_string, message, sizeof(message), kCFStringEncodingUTF8);
    fprintf(stderr, "compat32: Carbon alert: %s | %s\n", title, message);
    AlertStdCFStringAlertParamRec params = {0}, *pointer = NULL;
    if (a[3]) {
      const struct alert_params32 *guest = P(3);
      if (guest->version != 1 && guest->version != 2) {
        *out = (uint32_t)-50;
        return 1;
      }
      params.version = guest->version;
      params.movable = guest->movable;
      params.helpButton = guest->help;
      uint32_t tokens[] = {guest->default_text, guest->cancel_text,
                           guest->other_text};
      CFStringRef texts[3];
      for (unsigned i = 0; i < 3; ++i)
        texts[i] = tokens[i] == UINT32_MAX
                       ? (CFStringRef)(intptr_t)-1
                       : objc_bridge32_host_object(tokens[i]);
      params.defaultText = texts[0];
      params.cancelText = texts[1];
      params.otherText = texts[2];
      params.defaultButton = guest->default_button;
      params.cancelButton = guest->cancel_button;
      params.position = guest->position;
      params.flags = guest->flags;
      if (guest->version == 2)
        params.icon = unwrap(guest->icon);
      pointer = &params;
    }
    void *dialog = NULL;
    if (IS("_CreateStandardSheet"))
      *out = (uint32_t)F(int32_t, uint16_t, void *, void *, void *, void *,
                         void **)(
          (uint16_t)a[0], objc_bridge32_host_object(a[1]),
          objc_bridge32_host_object(a[2]), pointer, unwrap(a[4]), &dialog);
    else
      *out = (uint32_t)F(int32_t, uint16_t, void *, void *, void *, void **)(
          (uint16_t)a[0], objc_bridge32_host_object(a[1]),
          objc_bridge32_host_object(a[2]), pointer, &dialog);
    unsigned index = IS("_CreateStandardSheet") ? 5 : 4;
    if (a[index])
      *(uint32_t *)P(index) = wrap(dialog);
    return 1;
  }
  if (IS("_GetDialogWindow")) {
    *out = wrap(F(void *, void *)(unwrap(a[0])));
    return 1;
  }
  if (IS("_YieldToAnyThread")) {
    *out = (uint32_t)(int32_t)F(int16_t, void)();
    return 1;
  }
  if (IS("_YieldToThread")) {
    *out = (uint32_t)(int32_t)F(int16_t, uint32_t)(a[0]);
    return 1;
  }
  if (IS("_GetIconRef")) {
    void *icon = NULL;
    *out = (uint32_t)(int32_t)F(int16_t, int16_t, uint32_t, uint32_t,
                                void **)((int16_t)a[0], a[1], a[2], &icon);
    if (a[3])
      *(uint32_t *)P(3) = wrap(icon);
    return 1;
  }
  if (IS("_ReleaseIconRef") || IS("_AcquireIconRef")) {
    *out = (uint32_t)(int32_t)F(int16_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_SetControlFontStyle")) {
    *out = (uint32_t)F(int32_t, void *, const void *)(unwrap(a[0]), P(1));
    return 1;
  }
  if (IS("_SetKeyboardFocus")) {
    *out = (uint32_t)F(int32_t, void *, void *,
                       int16_t)(unwrap(a[0]), unwrap(a[1]), (int16_t)a[2]);
    return 1;
  }
  if (IS("_GetKeyboardFocus")) {
    void *view = NULL;
    *out = (uint32_t)F(int32_t, void *, void **)(unwrap(a[0]), &view);
    if (a[1])
      *(uint32_t *)P(1) = wrap(view);
    return 1;
  }
  if (IS("_SetControlDragTrackingEnabled")) {
    *out = (uint32_t)F(int32_t, void *, bool)(unwrap(a[0]), a[1] != 0);
    return 1;
  }
  if (IS("_NewRgn")) {
    *out = wrap(F(void *, void)());
    return 1;
  }
  if (IS("_DisposeRgn") || IS("_SetEmptyRgn")) {
    F(void, void *)(unwrap(a[0]));
    *out = 0;
    return 1;
  }
  if (IS("_EmptyRgn") || IS("_IsRegionRectangular")) {
    *out = F(uint8_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_GetRegionBounds") || IS("_RectRgn")) {
    F(void, void *, void *)(unwrap(a[0]), P(1));
    *out = a[1];
    return 1;
  }
  if (IS("_SetRectRgn")) {
    F(void, void *, int16_t, int16_t, int16_t, int16_t)(unwrap(a[0]), a[1],
                                                        a[2], a[3], a[4]);
    *out = 0;
    return 1;
  }
  if (IS("_OffsetRgn")) {
    F(void, void *, int16_t, int16_t)(unwrap(a[0]), a[1], a[2]);
    *out = 0;
    return 1;
  }
  if (IS("_CopyRgn")) {
    F(void, void *, void *)(unwrap(a[0]), unwrap(a[1]));
    *out = 0;
    return 1;
  }
  if (IS("_SectRgn") || IS("_UnionRgn") || IS("_DiffRgn") || IS("_XorRgn")) {
    F(void, void *, void *, void *)(unwrap(a[0]), unwrap(a[1]), unwrap(a[2]));
    *out = 0;
    return 1;
  }
  if (IS("_SetWindowDefaultButton") || IS("_SetWindowCancelButton")) {
    *out = (uint32_t)F(int32_t, void *, void *)(unwrap(a[0]), unwrap(a[1]));
    return 1;
  }
  if (IS("_SetControlData")) {
    const void *data = P(4);
    ByteCount size = a[3];
    void *object = NULL;
    if ((a[2] == 0x6d68616e || a[2] == 0x6f6d7266) && data && size == 4) {
      uint32_t token;
      memcpy(&token, data, 4);
      object = unwrap(token);
      data = &object;
      size = sizeof(object);
    }
    struct __attribute__((packed, aligned(2))) content64 {
      int16_t type;
      uint64_t value;
    } content = {0};
    if (a[2] == 0x636f6e74 && data && size == 6) {
      uint32_t value;
      memcpy(&content.type, data, 2);
      memcpy(&value, (const char *)data + 2, 4);
      if (content.type >= 129 && content.type <= 133 && content.type != 132)
        return 0;
      content.value = content.type == 134
                          ? (uintptr_t)objc_bridge32_host_object(value)
                      : content.type == 132 ? (uintptr_t)unwrap(value)
                                            : value;
      data = &content;
      size = sizeof(content);
    }
    if (a[2] == 0x63667374 || a[2] == 0x70776366 || a[2] == 0x696e6366) {
      if (size != 4 || !data) {
        *out = (uint32_t)-50;
        return 1;
      }
      uint32_t token;
      memcpy(&token, data, 4);
      object = objc_bridge32_host_object(token);
      data = &object;
      size = sizeof(object);
    }
    *out = (uint32_t)F(int32_t, void *, int16_t, uint32_t, ByteCount,
                       const void *)(unwrap(a[0]), (int16_t)a[1], a[2], size,
                                     data);
    return 1;
  }
  if (IS("_GetControlData")) {
    ByteCount size = 0;
    if (a[2] == 0x6d68616e || a[2] == 0x6f6d7266) {
      void *menu = NULL;
      int32_t status =
          F(int32_t, void *, int16_t, uint32_t, ByteCount, void *, ByteCount *)(
              unwrap(a[0]), (int16_t)a[1], a[2], sizeof(menu), &menu, &size);
      if (!status && a[3] >= 4 && a[4])
        *(uint32_t *)P(4) = wrap(menu);
      if (a[5])
        *(uint32_t *)P(5) = 4;
      *out = (uint32_t)status;
      return 1;
    }
    if (a[2] == 0x63667374 || a[2] == 0x70776366 || a[2] == 0x696e6366) {
      CFStringRef string = NULL;
      int32_t status = F(int32_t, void *, int16_t, uint32_t, ByteCount, void *,
                         ByteCount *)(unwrap(a[0]), (int16_t)a[1], a[2],
                                      sizeof(string), &string, &size);
      if (!status && a[3] >= 4 && a[4])
        *(uint32_t *)P(4) = objc_bridge32_guest_object((void *)string);
      else if (!status)
        status = -50;
      if (string)
        CFRelease(string);
      if (a[5])
        *(uint32_t *)P(5) = 4;
      *out = (uint32_t)status;
      return 1;
    }
    *out = (uint32_t)F(int32_t, void *, int16_t, uint32_t, ByteCount, void *,
                       ByteCount *)(unwrap(a[0]), (int16_t)a[1], a[2], a[3],
                                    P(4), &size);
    if (a[5])
      *(uint32_t *)P(5) = (uint32_t)size;
    return 1;
  }
  if (IS("_SetControlProperty") || IS("_SetWindowProperty")) {
    *out = (uint32_t)F(int32_t, void *, uint32_t, uint32_t, ByteCount,
                       const void *)(unwrap(a[0]), a[1], a[2], a[3], P(4));
    return 1;
  }
  if (IS("_GetControlProperty") || IS("_GetWindowProperty")) {
    ByteCount size = 0;
    int32_t r = F(int32_t, void *, uint32_t, uint32_t, ByteCount, ByteCount *,
                  void *)(unwrap(a[0]), a[1], a[2], a[3], &size, P(5));
    if (a[4])
      *(uint32_t *)P(4) = (uint32_t)size;
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_GetControlPropertySize")) {
    ByteCount size = 0;
    int32_t r = F(int32_t, void *, uint32_t, uint32_t,
                  ByteCount *)(unwrap(a[0]), a[1], a[2], &size);
    if (a[3])
      *(uint32_t *)P(3) = (uint32_t)size;
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_RemoveControlProperty")) {
    *out = (uint32_t)F(int32_t, void *, uint32_t, uint32_t)(unwrap(a[0]), a[1],
                                                            a[2]);
    return 1;
  }
  if (IS("_GetBestControlRect")) {
    *out = (uint32_t)F(int32_t, void *, Rect *, int16_t *)(unwrap(a[0]), P(1),
                                                           P(2));
    if (!*out && a[1]) {
      Rect *r = P(1);
      CGRect rect =
          CGRectMake(r->left, r->top, r->right - r->left, r->bottom - r->top);
      control_port_rect(unwrap(a[0]), &rect, false);
      *r =
          (Rect){rect.origin.y, rect.origin.x, rect.origin.y + rect.size.height,
                 rect.origin.x + rect.size.width};
    }
    return 1;
  }
  if (IS("_GetRootControl") || IS("_CreateRootControl")) {
    void *control = NULL;
    int32_t r = F(int32_t, void *, void **)(unwrap(a[0]), &control);
    if (a[1])
      *(uint32_t *)P(1) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_GetControlEventTarget") || IS("_GetControlOwner") ||
      IS("_HIObjectGetEventTarget") || IS("_HIViewGetSuperview") ||
      IS("_HIViewGetWindow")) {
    *out = wrap(F(void *, void *)(unwrap(a[0])));
    if (getenv("LP32_TRACE_CARBON"))
      fprintf(stderr, "compat32: Carbon %s -> %#x\n", name, (uint32_t)*out);
    return 1;
  }
  if (IS("_GetControlBounds")) {
    CGRect rect = {0};
    ((int32_t (*)(void *, CGRect *))symbol("_HIViewGetFrame"))(unwrap(a[0]),
                                                               &rect);
    control_port_rect(unwrap(a[0]), &rect, false);
    if (a[1])
      *(Rect *)P(1) =
          (Rect){rect.origin.y, rect.origin.x, rect.origin.y + rect.size.height,
                 rect.origin.x + rect.size.width};
    *out = a[1];
    return 1;
  }
  if (IS("_SetControlBounds") || IS("_GetControlID") || IS("_GetControlKind") ||
      IS("_CountSubControls")) {
    *out = (uint32_t)F(int32_t, void *, void *)(unwrap(a[0]), P(1));
    return 1;
  }
  if (IS("_GetIndexedSubControl")) {
    void *control = NULL;
    int32_t r = F(int32_t, void *, uint16_t, void **)(unwrap(a[0]),
                                                      (uint16_t)a[1], &control);
    if (a[2])
      *(uint32_t *)P(2) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_GetControl32BitValue") || IS("_GetControl32BitMinimum") ||
      IS("_GetControl32BitMaximum")) {
    *out = (uint32_t)F(int32_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_SetControl32BitValue") || IS("_SetControl32BitMinimum") ||
      IS("_SetControl32BitMaximum") || IS("_SetControlCommandID")) {
    F(void, void *, int32_t)(unwrap(a[0]), (int32_t)a[1]);
    *out = 0;
    return 1;
  }
  if (IS("_EnableControl") || IS("_DisableControl")) {
    *out = (uint32_t)F(int32_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_IsControlEnabled")) {
    *out = F(uint8_t, void *, void *)(unwrap(a[0]), NULL);
    return 1;
  }
  if (IS("_IsControlVisible") || IS("_IsControlHilited")) {
    *out = F(uint8_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_SetControlVisibility")) {
    *out = (uint32_t)F(int32_t, void *, bool, bool)(unwrap(a[0]), a[1] != 0,
                                                    a[2] != 0);
    return 1;
  }
  if (IS("_CopyControlTitleAsCFString")) {
    CFStringRef text = NULL;
    int32_t r = F(int32_t, void *, CFStringRef *)(unwrap(a[0]), &text);
    if (a[1])
      *(uint32_t *)P(1) = objc_bridge32_guest_object((void *)text);
    if (text)
      CFRelease(text);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_SetControlTitleWithCFString")) {
    *out = (uint32_t)F(int32_t, void *,
                       void *)(unwrap(a[0]), objc_bridge32_host_object(a[1]));
    return 1;
  }
  if (IS("_CreateUserPaneControl")) {
    void *control = NULL;
    int32_t r = F(int32_t, void *, const Rect *, uint32_t,
                  void **)(unwrap(a[0]), P(1), a[2], &control);
    if (a[3])
      *(uint32_t *)P(3) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CreateChasingArrowsControl") || IS("_CreateSeparatorControl")) {
    void *control = NULL;
    int32_t r =
        F(int32_t, void *, const Rect *, void **)(unwrap(a[0]), P(1), &control);
    if (a[2])
      *(uint32_t *)P(2) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CreateTabsControl")) {
    struct __attribute__((packed, aligned(2))) tab32 {
      uint32_t icon, title;
      uint8_t enabled, pad;
    };
    struct __attribute__((packed, aligned(2))) native_tab {
      void *icon;
      CFStringRef title;
      uint8_t enabled, pad;
    };
    unsigned count = (uint16_t)a[4];
    struct native_tab *tabs = calloc(count ? count : 1, sizeof(*tabs));
    if (!tabs) {
      *out = (uint32_t)-108;
      return 1;
    }
    const struct tab32 *guest = P(5);
    for (unsigned i = 0; i < count; ++i) {
      if (guest[i].icon) {
        free(tabs);
        return 0;
      }
      tabs[i].title = objc_bridge32_host_object(guest[i].title);
      tabs[i].enabled = guest[i].enabled;
    }
    void *control = NULL;
    int32_t r = F(int32_t, void *, const Rect *, uint16_t, uint16_t, uint16_t,
                  const void *, void **)(unwrap(a[0]), P(1), (uint16_t)a[2],
                                         (uint16_t)a[3], count, tabs, &control);
    free(tabs);
    if (a[6])
      *(uint32_t *)P(6) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CreateStaticTextControl")) {
    void *control = NULL;
    int32_t r = F(int32_t, void *, const Rect *, void *, const void *, void **)(
        unwrap(a[0]), P(1), objc_bridge32_host_object(a[2]), P(3), &control);
    if (a[4])
      *(uint32_t *)P(4) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CreateCheckGroupBoxControl")) {
    void *control = NULL;
    int32_t r = F(int32_t, void *, const Rect *, void *, int32_t, bool, bool,
                  void **)(unwrap(a[0]), P(1), objc_bridge32_host_object(a[2]),
                           (int32_t)a[3], a[4] != 0, a[5] != 0, &control);
    if (a[6])
      *(uint32_t *)P(6) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CreateCheckBoxControl")) {
    void *control = NULL;
    int32_t r = F(int32_t, void *, const Rect *, void *, int32_t, bool,
                  void **)(unwrap(a[0]), P(1), objc_bridge32_host_object(a[2]),
                           (int32_t)a[3], a[4] != 0, &control);
    if (a[5])
      *(uint32_t *)P(5) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CreatePopupButtonControl")) {
    void *control = NULL;
    int32_t r = F(int32_t, void *, const Rect *, void *, int16_t, bool, int16_t,
                  int16_t, uint8_t, void **)(
        unwrap(a[0]), P(1), objc_bridge32_host_object(a[2]), (int16_t)a[3],
        a[4] != 0, (int16_t)a[5], (int16_t)a[6], (uint8_t)a[7], &control);
    if (a[8])
      *(uint32_t *)P(8) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CreatePushButtonControl")) {
    void *control = NULL;
    int32_t r = F(int32_t, void *, const Rect *, void *, void **)(
        unwrap(a[0]), P(1), objc_bridge32_host_object(a[2]), &control);
    if (a[3])
      *(uint32_t *)P(3) = wrap(control);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_HIViewAddSubview")) {
    *out = (uint32_t)F(int32_t, void *, void *)(unwrap(a[0]), unwrap(a[1]));
    if (getenv("LP32_TRACE_CARBON"))
      fprintf(stderr, "compat32: HIViewAddSubview status=%d\n", (int32_t)*out);
    return 1;
  }
  if (IS("_HIViewSetNeedsDisplay")) {
    *out = (uint32_t)F(int32_t, void *, bool)(unwrap(a[0]), a[1] != 0);
    return 1;
  }
  if (IS("_HIViewSetFrame") || IS("_HIViewGetFrame") ||
      IS("_HIViewGetBounds")) {
    float *p = P(1);
    CGRect rect;
    if (IS("_HIViewSetFrame"))
      rect = CGRectMake(p[0], p[1], p[2], p[3]);
    int32_t r = F(int32_t, void *, CGRect *)(unwrap(a[0]), &rect);
    if (!r && !IS("_HIViewSetFrame")) {
      p[0] = rect.origin.x;
      p[1] = rect.origin.y;
      p[2] = rect.size.width;
      p[3] = rect.size.height;
    }
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_SetWindowContentColor")) {
    *out = (uint32_t)F(int32_t, void *, const void *)(unwrap(a[0]), P(1));
    return 1;
  }
  if (IS("_CreateNibReferenceWithCFBundle")) {
    CFBundleRef bundle = carbon_ui_copy_bundle(objc_bridge32_host_object(a[0]));
    void *nib = NULL;
    int32_t status = bundle ? F(int32_t, CFBundleRef, CFStringRef, void **)(
        bundle, objc_bridge32_host_object(a[1]), &nib) : paramErr;
    if (bundle)
      CFRelease(bundle);
    if (a[2])
      *(uint32_t *)P(2) = wrap(nib);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateWindowFromNib")) {
    void *window = NULL;
    *out = (uint32_t)F(int32_t, void *, CFStringRef, void **)(
        unwrap(a[0]), objc_bridge32_host_object(a[1]), &window);
    if (a[2])
      *(uint32_t *)P(2) = wrap(window);
    return 1;
  }
  if (IS("_DisposeNibReference")) {
    F(void, void *)(unwrap(a[0]));
    *out = 0;
    return 1;
  }
  if (IS("_CreateNewWindow")) {
    void *window = NULL;
    /* Modern Carbon windows require compositing; the legacy QuickDraw
     * port is represented by the same guest token and attached via AGL. */
    int32_t r = F(int32_t, uint32_t, uint32_t, const Rect *,
                  void **)(a[0], a[1] | (1u << 19), P(2), &window);
    if (a[3])
      *(uint32_t *)P(3) = wrap(window);
    struct carbon_ref *ref = reference(wrap(window));
    if (ref)
      ref->legacy_window_coordinates = !(a[1] & kWindowCompositingAttribute);
    fprintf(stderr,
            "compat32: Carbon window class=%u attrs=%#x status=%d token=%#x\n",
            a[0], a[1], r, wrap(window));
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_ShowWindow") || IS("_HideWindow") || IS("_DisposeWindow") ||
      IS("_SelectWindow")) {
    if (IS("_SelectWindow") &&
        lp32_title_is_cod_carbon(lp32_profile()->title) &&
        carbon_ui_select(unwrap(a[0]))) {
      *out = 0;
      return 1;
    }
    if (IS("_ShowWindow")) {
      place_test_window(unwrap(a[0]));
      carbon_ui_show(unwrap(a[0]));
      *out = 0;
      return 1;
    }
    if (IS("_HideWindow"))
      carbon_ui_hide(unwrap(a[0]));
    if (IS("_DisposeWindow"))
      carbon_ui_dispose(unwrap(a[0]));
    if (IS("_DisposeWindow")) {
      struct carbon_ref *window = reference(a[0]);
      if (window && window->background_handler) {
        RemoveEventHandler(window->background_handler);
        window->background_handler = NULL;
      }
      if (window && window->background_image) {
        CGImageRelease(window->background_image);
        window->background_image = NULL;
      }
    }
    if (!IS("_SelectWindow") || !getenv("LP32_BACKGROUND_TEST"))
      F(void, void *)(unwrap(a[0]));
    *out = 0;
    return 1;
  }
  if (IS("_RetainWindow") || IS("_ReleaseWindow")) {
    *out = (uint32_t)F(int32_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_GetWindowEventTarget")) {
    *out = wrap(F(void *, void *)(unwrap(a[0])));
    return 1;
  }
  if (IS("_GetWindowBounds") || IS("_SetWindowBounds")) {
    if (getenv("LP32_TRACE_CARBON") && IS("_SetWindowBounds") && a[2]) {
      const Rect *r = P(2);
      fprintf(stderr, "compat32: SetWindowBounds %#x region=%u rect=%d,%d,%d,%d\n",
              a[0], a[1], r->left, r->top, r->right, r->bottom);
    }
    *out = (uint32_t)F(int32_t, void *, uint32_t, void *)(unwrap(a[0]), a[1],
                                                          P(2));
    if (!*out && IS("_SetWindowBounds")) carbon_ui_geometry_changed(unwrap(a[0]));
    return 1;
  }
  if (IS("_GetWindowPortBounds")) {
    *out = a[1];
    ((int32_t (*)(void *, uint32_t, Rect *))symbol("_GetWindowBounds"))(
        unwrap(a[0]), 33, P(1));
    Rect *bounds = P(1);
    bounds->right -= bounds->left;
    bounds->bottom -= bounds->top;
    bounds->left = bounds->top = 0;
    return 1;
  }
  if (IS("_GetWindowClass") || IS("_GetWindowAttributes")) {
    *out = (uint32_t)F(int32_t, void *, void *)(unwrap(a[0]), P(1));
    return 1;
  }
  if (IS("_IsWindowVisible") || IS("_IsWindowActive") || IS("_IsWindowHilited") ||
      IS("_IsWindowCollapsed") || IS("_IsWindowCollapsable")) {
    if (IS("_IsWindowActive") || IS("_IsWindowHilited")) {
      int active = carbon_ui_is_active(unwrap(a[0]));
      if (active >= 0) { *out = active; return 1; }
    }
    if (IS("_IsWindowVisible")) {
      int visible = carbon_ui_is_visible(unwrap(a[0]));
      if (visible >= 0) {
        *out = visible;
        return 1;
      }
    }
    *out = F(uint8_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_SetWindowTitleWithCFString")) {
    *out = (uint32_t)F(int32_t, void *,
                       void *)(unwrap(a[0]), objc_bridge32_host_object(a[1]));
    return 1;
  }
  if (IS("_SetWindowAlpha")) {
    float alpha;
    memcpy(&alpha, a + 1, 4);
    *out = (uint32_t)F(int32_t, void *, float)(unwrap(a[0]), alpha);
    return 1;
  }
  if (IS("_TransitionWindow")) {
    *out = (uint32_t)F(int32_t, void *, uint32_t, uint32_t,
                       const Rect *)(unwrap(a[0]), a[1], a[2], P(3));
    return 1;
  }
  if (IS("_MoveWindow") || IS("_SizeWindow")) {
    if (getenv("LP32_TRACE_CARBON"))
      fprintf(stderr, "compat32: %s %#x x=%d y=%d flag=%u\n", name, a[0], (int16_t)a[1], (int16_t)a[2], a[3]);
    F(void, void *, int16_t, int16_t,
      bool)(unwrap(a[0]), (int16_t)a[1], (int16_t)a[2],
            a[3] != 0 && !getenv("LP32_BACKGROUND_TEST"));
    carbon_ui_geometry_changed(unwrap(a[0]));
    *out = 0;
    return 1;
  }
  if (IS("_SetWindowResizeLimits") || IS("_GetWindowResizeLimits")) {
    CGSize limits[2] = {{0, 0}, {0, 0}};
    if (IS("_SetWindowResizeLimits"))
      for (unsigned i = 0; i < 2; ++i)
        if (a[i + 1]) {
          float *p = (void *)(uintptr_t)a[i + 1];
          limits[i] = CGSizeMake(p[0], p[1]);
        }
    *out = (uint32_t)F(int32_t, void *, CGSize *, CGSize *)(
        unwrap(a[0]), a[1] ? &limits[0] : NULL, a[2] ? &limits[1] : NULL);
    if (!*out && IS("_GetWindowResizeLimits"))
      for (unsigned i = 0; i < 2; ++i)
        if (a[i + 1]) {
          float *p = (void *)(uintptr_t)a[i + 1];
          p[0] = limits[i].width;
          p[1] = limits[i].height;
        }
    return 1;
  }
  if (IS("_RepositionWindow")) {
    *out = (uint32_t)F(int32_t, void *, void *, uint32_t)(unwrap(a[0]),
                                                          unwrap(a[1]), a[2]);
    return 1;
  }
  if (IS("_SetAutomaticControlDragTrackingEnabledForWindow")) {
    *out = (uint32_t)F(int32_t, void *, bool)(unwrap(a[0]), a[1] != 0);
    return 1;
  }
  if (IS("_SetThemeWindowBackground")) {
    *out = (uint32_t)F(int32_t, void *, uint32_t, bool)(unwrap(a[0]), a[1],
                                                        a[2] != 0);
    return 1;
  }
  if (IS("_HIObjectCreate")) {
    void *object = NULL;
    int32_t status = F(int32_t, void *, void *, void **)(
        objc_bridge32_host_object(a[0]), unwrap(a[1]), &object);
    if (a[2])
      *(uint32_t *)P(2) = objc_bridge32_guest_object(object);
    if (object)
      CFRelease(object);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_HIObjectRegisterSubclass")) {
    struct handler32 *handler = malloc(sizeof(*handler));
    if (!handler) {
      *out = (uint32_t)-108;
      return 1;
    }
    *handler = (struct handler32){a[3], a[6]};
    void *class_ref = NULL;
    int32_t status = F(int32_t, void *, void *, uint32_t, void *, uint32_t,
                       const void *, void *, void **)(
        objc_bridge32_host_object(a[0]), objc_bridge32_host_object(a[1]), a[2],
        event_handler, a[4], P(5), handler, &class_ref);
    if (status)
      free(handler);
    else {
      uint32_t token = wrap(class_ref);
      struct carbon_ref *ref = reference(token);
      if (ref)
        ref->callback = handler;
      if (a[7])
        *(uint32_t *)P(7) = token;
    }
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_CreateEvent")) {
    double time;
    memcpy(&time, a + 3, sizeof(time));
    void *event = NULL;
    int32_t status =
        F(int32_t, void *, uint32_t, uint32_t, double, uint32_t, void **)(
            objc_bridge32_host_object(a[0]), a[1], a[2], time, a[5], &event);
    if (a[6])
      *(uint32_t *)P(6) = wrap(event);
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_RetainEvent")) {
    *out = wrap(F(void *, void *)(unwrap(a[0])));
    return 1;
  }
  if (IS("_ReleaseEvent")) {
    F(void, void *)(unwrap(a[0]));
    *out = 0;
    return 1;
  }
  if (IS("_GetMainEventQueue") || IS("_GetEventDispatcherTarget")) {
    *out = wrap(F(void *, void)());
    return 1;
  }
  if (IS("_SendEventToEventTarget") || IS("_PostEventToQueue")) {
    *out = (uint32_t)F(int32_t, void *, void *,
                       int16_t)(unwrap(a[0]), unwrap(a[1]), (int16_t)a[2]);
    return 1;
  }
  if (IS("_ReceiveNextEvent")) {
    carbon_ui_sync_focus();
    double timeout;
    memcpy(&timeout, a + 2, 8);
    void *event = NULL;
    int32_t r = F(int32_t, uint32_t, const void *, double, bool,
                  void **)(a[0], P(1), timeout, a[4] != 0, &event);
    carbon_ui_sync_focus();
    if (!r) record_mouse_event(event);
    if (a[5])
      *(uint32_t *)P(5) = wrap(event);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_AddEventTypesToHandler") || IS("_RemoveEventTypesFromHandler")) {
    *out = (uint32_t)F(int32_t, void *, uint32_t, void *)(unwrap(a[0]), a[1],
                                                          P(2));
    return 1;
  }
  if (IS("_AEInstallEventHandler")) {
    struct handler32 *h = malloc(sizeof(*h));
    if (!h) {
      *out = (uint32_t)-108;
      return 1;
    }
    *h = (struct handler32){a[2], a[3]};
    OSErr r = F(OSErr, uint32_t, uint32_t, AEEventHandlerUPP, SRefCon,
                bool)(a[0], a[1], apple_event_handler, (SRefCon)h, a[4] != 0);
    if (r)
      free(h);
    else {
      struct apple_handler_entry *entry = malloc(sizeof(*entry));
      if (entry) {
        *entry = (struct apple_handler_entry){a[0], a[1], a[2], a[4] != 0, apple_handlers};
        apple_handlers = entry;
      }
    }
    *out = (uint32_t)(int32_t)r;
    return 1;
  }
  if (IS("_AERemoveEventHandler")) {
    struct apple_handler_entry **slot = &apple_handlers;
    while (*slot && ((*slot)->event_class != a[0] || (*slot)->event_id != a[1] ||
                    (*slot)->callback != a[2] || (*slot)->system != (a[3] != 0)))
      slot = &(*slot)->next;
    OSErr status = errAEHandlerNotFound;
    if (*slot) {
      status = F(OSErr, uint32_t, uint32_t, AEEventHandlerUPP, bool)(
          a[0], a[1], apple_event_handler, a[3] != 0);
      if (!status) {struct apple_handler_entry *entry = *slot; *slot = entry->next; free(entry);}
    }
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_AECountItems") || IS("_AEGetDescData") || IS("_AEDisposeDesc") ||
      IS("_AECoerceDesc") || IS("_AEGetParamDesc")) {
    AEDesc desc = host_descriptor(P(0));
    OSErr r;
    if (IS("_AECountItems")) {
      long count = 0;
      r = F(OSErr, const AEDesc *, long *)(&desc, &count);
      if (a[1])
        *(int32_t *)P(1) = (int32_t)count;
    } else if (IS("_AEGetDescData"))
      r = F(OSErr, const AEDesc *, void *, Size)(&desc, P(1), (int32_t)a[2]);
    else if (IS("_AEDisposeDesc")) {
      r = F(OSErr, AEDesc *)(&desc);
      guest_descriptor(P(0), &desc);
    } else {
      AEDesc result = {typeNull, NULL};
      if (IS("_AECoerceDesc"))
        r = F(OSErr, const AEDesc *, uint32_t, AEDesc *)(&desc, a[1], &result);
      else
        r = F(OSErr, const AEDesc *, uint32_t, uint32_t,
              AEDesc *)(&desc, a[1], a[2], &result);
      guest_descriptor(P(IS("_AECoerceDesc") ? 2 : 3), &result);
    }
    *out = (uint32_t)(int32_t)r;
    return 1;
  }
  if (IS("_DrawMenuBar") || IS("_HideMenuBar") || IS("_ShowMenuBar")) {
    if (lp32_title_is_cod_carbon(lp32_profile()->title) &&
        !IS("_DrawMenuBar")) {
      /* Persist the guest request across native AppKit activation changes. */
      carbon_ui_set_menu_bar_visible(IS("_ShowMenuBar"));
      *out = 0;
      return 1;
    }
    if (!getenv("LP32_BACKGROUND_TEST"))
      F(void, void)();
    *out = 0;
    return 1;
  }
  if (IS("_GetMBarHeight")) {
    *out = (uint32_t)(int32_t)F(int16_t, void)();
    return 1;
  }
  if (IS("_InsertMenu")) {
    F(void, void *, int16_t)(unwrap(a[0]), (int16_t)a[1]);
    *out = 0;
    return 1;
  }
  if (IS("_CreateNewMenu")) {
    void *menu = NULL;
    int32_t r =
        F(int32_t, int16_t, uint32_t, void **)((int16_t)a[0], a[1], &menu);
    if (a[2])
      *(uint32_t *)P(2) = wrap(menu);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_RetainMenu") || IS("_ReleaseMenu")) {
    *out = (uint32_t)F(int32_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_DeleteMenu")) {
    F(void, int16_t)((int16_t)a[0]);
    *out = 0;
    return 1;
  }
  if (IS("_DeleteMenuItems")) {
    *out = (uint32_t)F(int32_t, void *, uint16_t,
                       uint32_t)(unwrap(a[0]), (uint16_t)a[1], a[2]);
    return 1;
  }
  if (IS("_EnableMenuCommand") || IS("_DisableMenuCommand")) {
    F(void, void *, uint32_t)(unwrap(a[0]), a[1]);
    *out = 0;
    return 1;
  }
  if (IS("_InsertMenuItemTextWithCFString")) {
    *out = (uint32_t)F(int32_t, void *, void *, uint16_t, uint32_t,
                       uint32_t)(unwrap(a[0]), objc_bridge32_host_object(a[1]),
                                 (uint16_t)a[2], a[3], a[4]);
    return 1;
  }
  if (IS("_GetIndMenuItemWithCommandID")) {
    void *menu = NULL;
    int32_t r = F(int32_t, void *, uint32_t, uint32_t, void **,
                  void *)(unwrap(a[0]), a[1], a[2], &menu, P(4));
    if (a[3])
      *(uint32_t *)P(3) = wrap(menu);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CopyMenuItemTextAsCFString")) {
    CFStringRef text = NULL;
    int32_t r = F(int32_t, void *, uint16_t, void *)(unwrap(a[0]),
                                                     (uint16_t)a[1], &text);
    if (a[2])
      *(uint32_t *)P(2) = objc_bridge32_guest_object((void *)text);
    if (text)
      CFRelease(text);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_GetCurrentKeyModifiers") || IS("_GetDblTime") ||
      IS("_GetCaretTime")) {
    *out = (IS("_GetCurrentKeyModifiers") && (getenv("LP32_BACKGROUND_TEST") || lp32_suppress_background_input()))
               ? 0
               : F(uint32_t, void)();
    return 1;
  }
  if (IS("_GetKeys")) {
    if (getenv("LP32_BACKGROUND_TEST")) {
      memset(P(0), 0, 16);
      for(unsigned key=0;key<128;++key)
        if(objc_bridge32_test_key_down(key))((unsigned char *)P(0))[key/8]|=1u<<(key%8);
    } else if (lp32_suppress_background_input()) {
      memset(P(0), 0, 16);
    } else
      F(void, void *)(P(0));
    *out = 0;
    return 1;
  }
  if (IS("_UCConvertCFAbsoluteTimeToLongDateTime")) {
    double value;memcpy(&value,a,8);
    *out=(uint32_t)F(int32_t,double,int64_t *)(value,P(2));return 1;
  }
  if (IS("_UCConvertLongDateTimeToCFAbsoluteTime")) {
    int64_t value;memcpy(&value,a,8);
    *out=(uint32_t)F(int32_t,int64_t,double *)(value,P(2));return 1;
  }
  if (IS("_TransformProcessType")) {
    *out = (uint32_t)F(int32_t, const void *, uint32_t)(P(0), a[1]);
    return 1;
  }
  if (IS("_SetFrontProcess") || IS("_SetFrontProcessWithOptions")) {
    if (getenv("LP32_BACKGROUND_TEST")) *out = 0;
    else if (IS("_SetFrontProcess")) *out = (uint32_t)F(int32_t, const void *)(P(0));
    else *out = (uint32_t)F(int32_t, const void *, uint32_t)(P(0), a[1]);
    return 1;
  }
  if (IS("_SameProcess")) {
    *out = (uint32_t)F(int32_t, const void *, const void *, uint8_t *)(P(0), P(1), P(2));
    return 1;
  }
  if (IS("_GetCurrentProcess") || IS("_GetFrontProcess") || IS("_GetProcessBundleLocation")) {
    if (IS("_GetFrontProcess") && getenv("LP32_BACKGROUND_TEST") &&
        !getenv("LP32_TEST_FOCUS_LOSS")) {
      /* Match the simulated active state used by background rendering tests. */
      *out = (uint32_t)((int32_t (*)(void *))symbol("_GetCurrentProcess"))(P(0));
      return 1;
    }
    if (IS("_GetCurrentProcess") || IS("_GetFrontProcess"))
      *out = (uint32_t)F(int32_t, void *)(P(0));
    else
      *out = (uint32_t)F(int32_t, const void *, void *)(P(0), P(1));
    return 1;
  }
  if (IS("_UpTime")) { *out = F(uint64_t, void)(); return 1; }
  if (IS("_AddDurationToAbsolute")) {
    *out = F(uint64_t, int32_t, uint64_t)((int32_t)a[0], (uint64_t)a[1] | (uint64_t)a[2] << 32);
    return 1;
  }
  if (IS("_MPDelayUntil")) { *out = (uint32_t)F(int32_t, const void *)(P(0)); return 1; }
  if (IS("_Microseconds")) { F(void, void *)(P(0)); *out = 0; return 1; }
  if (IS("_FlushEventQueue")) { *out = (uint32_t)F(int32_t, void *)(unwrap(a[0])); return 1; }
  if (IS("_GetNextProcess")) {
    *out = (uint32_t)F(int32_t, void *)(P(0));
    return 1;
  }
  if (IS("_GetProcessInformation")) {
    uint32_t *guest = P(1);
    if (!guest || guest[0] < 60) {
      *out = (uint32_t)-50;
      return 1;
    }
    FSRef app;
    ProcessInfoRec info = {0};
    info.processInfoLength = sizeof(info);
    info.processName = (void *)(uintptr_t)guest[1];
    info.processAppRef = guest[14] ? &app : NULL;
    int32_t status = F(int32_t, const void *, void *)(P(0), &info);
    if (!status) {
      memcpy(guest + 2, &info.processNumber, 8);
      guest[4] = info.processType;
      guest[5] = info.processSignature;
      guest[6] = info.processMode;
      guest[7] = compat_runtime32_image()->min_address;
      guest[8] = info.processSize;
      guest[9] = info.processFreeMem;
      memcpy(guest + 10, &info.processLauncher, 8);
      guest[12] = info.processLaunchDate;
      guest[13] = info.processActiveTime;
      if (guest[14])
        status = spec_from_ref(&app, (void *)(uintptr_t)guest[14]);
    }
    *out = (uint32_t)status;
    return 1;
  }
  if (IS("_FSFindFolder")) {
    *out =
        (uint32_t)(int32_t)find_folder32((int16_t)a[0], a[1], a[2] != 0, P(3));
    return 1;
  }
  if (IS("_FSRefMakePath")) {
    *out =
        (uint32_t)F(int32_t, const void *, void *, uint32_t)(P(0), P(1), a[2]);
    return 1;
  }
  if (IS("_FSPathMakeRef")) {
    *out = (uint32_t)F(int32_t, const void *, void *, void *)(P(0), P(1), P(2));
    if (getenv("LP32_TRACE_FILES"))
      fprintf(stderr, "compat32: FSPathMakeRef %s -> %d\n", (const char *)P(0),
              (int32_t)*out);
    return 1;
  }
  if (IS("_FSGetCatalogInfo")) {
    FSCatalogInfo info;
    catalog_info_from_guest(P(2), &info);
    int16_t status = F(int16_t, const void *, uint32_t, void *, void *, void *,
                       void *)(P(0), a[1], a[2] ? &info : NULL, P(3), NULL, P(5));
    if (!status && a[2])
      catalog_info_to_guest(&info, P(2));
    if (!status) {
      /* Legacy callers can query only a parent ID, then use that ID in a
         later parameter-block call without ever requesting an FSSpec. */
      struct spec32 ignored;
      int16_t converted = spec_from_ref(P(0), a[4] ? P(4) : &ignored);
      if (a[4]) status = converted;
    }
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_FSGetDataForkName") || IS("_FSGetResourceForkName")) {
    *out = (uint32_t)F(int32_t, void *)(P(0));
    return 1;
  }
  if (IS("_FSMakeFSRefUnicode")) {
    *out = (uint32_t)(int32_t)F(int16_t, const void *, uint32_t, const void *,
                                uint32_t, void *)(P(0), a[1], P(2), a[3], P(4));
    if (getenv("LP32_TRACE_FILES")) {
      UInt8 parent[4096] = {0};
      char child[1024] = {0};
      ((int32_t (*)(const void *, void *, uint32_t))symbol("_FSRefMakePath"))(
          P(0), parent, sizeof(parent));
      CFStringRef string = CFStringCreateWithCharacters(NULL, P(2), a[1]);
      if (string) {
        CFStringGetCString(string, child, sizeof(child), kCFStringEncodingUTF8);
        CFRelease(string);
      }
      fprintf(stderr, "compat32: FSMakeFSRefUnicode %s/%s -> %d\n", parent,
              child, (int32_t)*out);
    }
    return 1;
  }
  if (IS("_FSGetVolumeInfo")) {
    *out = (uint32_t)(int32_t)F(int16_t, int16_t, uint32_t, void *, uint32_t,
                                void *, void *, void *)(
        (int16_t)a[0], a[1], P(2), a[3], P(4), P(5), P(6));
    return 1;
  }
  if (IS("_FSCreateDirectoryUnicode") || IS("_FSCreateFileUnicode")) {
    FSRef ref;
    int16_t status;
    FSCatalogInfo info;
    const FSCatalogInfo *initial = catalog_info_from_guest(P(4), &info);
    if (IS("_FSCreateDirectoryUnicode"))
      status = F(int16_t, const void *, uint32_t, const void *, uint32_t,
                 const void *, void *, void *,
                 void *)(P(0), a[1], P(2), a[3], initial, &ref, NULL, P(7));
    else
      status = F(int16_t, const void *, uint32_t, const void *, uint32_t,
                 const void *, void *,
                 void *)(P(0), a[1], P(2), a[3], initial, &ref, NULL);
    if (!status) {
      if (a[5])
        memcpy(P(5), &ref, sizeof(ref));
      if (a[6])
        status = spec_from_ref(&ref, P(6));
    }
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_FSOpenFork")) {
    FSIORefNum file=0;
    *out = (uint32_t)(int32_t)F(int16_t, const void *, uint16_t, const void *,
                                int8_t, void *)(P(0), (uint16_t)a[1], P(2),
                                                (int8_t)a[3], &file);
    if(!*out && (file<0 || file>INT16_MAX)) {
      ((OSErr (*)(FSIORefNum))symbol("_FSCloseFork"))(file);*out=(uint32_t)tmfoErr;
    }
    if(!*out && a[4])*(int16_t *)P(4)=(int16_t)file;
    return 1;
  }
  if (IS("_PBReadForkAsync")) {
    struct fork_io32 *guest=P(0);
    if(!guest)return 0;
    guest->actual=0;__atomic_store_n(&guest->result,1,__ATOMIC_RELEASE);
    __atomic_add_fetch(&pending_fork_reads,1,__ATOMIC_RELAXED);
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),guest,read_fork_async);
    *out=0;
    return 1;
  }
  if (IS("_FSCloseFork") || IS("_FSFlushFork")) {
    *out = (uint32_t)(int32_t)F(int16_t, int16_t)((int16_t)a[0]);
    return 1;
  }
  if (IS("_FSReadFork") || IS("_FSWriteFork")) {
    ByteCount count = 0;
    int64_t offset = (int64_t)((uint64_t)a[2] | ((uint64_t)a[3] << 32));
    int16_t r =
        F(int16_t, int16_t, uint16_t, int64_t, ByteCount, void *, ByteCount *)(
            (int16_t)a[0], (uint16_t)a[1], offset, a[4], P(5), &count);
    if (a[6])
      *(uint32_t *)P(6) = (uint32_t)count;
    *out = (uint32_t)(int32_t)r;
    return 1;
  }
  if (IS("_FSGetForkSize") || IS("_FSGetForkPosition")) {
    *out = (uint32_t)(int32_t)F(int16_t, int16_t, void *)((int16_t)a[0], P(1));
    return 1;
  }
  if (IS("_FSSetForkSize") || IS("_FSSetForkPosition")) {
    *out = (uint32_t)(int32_t)F(int16_t, int16_t, uint16_t, int64_t)(
        (int16_t)a[0], (uint16_t)a[1],
        (int64_t)((uint64_t)a[2] | ((uint64_t)a[3] << 32)));
    return 1;
  }
  if (IS("_FSDeleteObject")) {
    *out = (uint32_t)(int32_t)F(int16_t, const void *)(P(0));
    return 1;
  }
  if (IS("_FSMoveObject")) {
    *out = (uint32_t)(int32_t)F(int16_t, const void *, const void *,
                                void *)(P(0), P(1), P(2));
    return 1;
  }
  if (IS("_FSCompareFSRefs") || IS("_FSExchangeObjects") ||
      IS("_FSpMakeFSRef")) {
    *out = (uint32_t)(int32_t)F(int16_t, const void *, void *)(P(0), P(1));
    return 1;
  }
  if (IS("_FSIsAliasFile")) {
    *out = (uint32_t)(int32_t)F(int16_t, const void *, void *,
                                void *)(P(0), P(1), P(2));
    return 1;
  }
  if (IS("_FSResolveAliasFile")) {
    *out = (uint32_t)(int32_t)F(int16_t, void *, bool, void *,
                                void *)(P(0), a[1] != 0, P(2), P(3));
    return 1;
  }
  if (IS("_FSSetCatalogInfo")) {
    FSCatalogInfo info;
    *out = (uint32_t)(int32_t)F(int16_t, const void *, uint32_t,
                                const void *)(P(0), a[1], catalog_info_from_guest(P(2), &info));
    return 1;
  }
  if (IS("_FSOpenIterator")) {
    void *iterator = NULL;
    int16_t r =
        F(int16_t, const void *, uint32_t, void **)(P(0), a[1], &iterator);
    if (!r && a[2])
      *(uint32_t *)P(2) = wrap(iterator);
    *out = (uint32_t)(int32_t)r;
    return 1;
  }
  if (IS("_FSCloseIterator")) {
    *out = (uint32_t)(int32_t)F(int16_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_FSGetCatalogInfoBulk")) {
    if (a[1] > 65536) {
      *out = (uint32_t)-50;
      return 1;
    }
    ItemCount count = 0;
    FSRef *refs = P(6);
    bool allocated = a[7] && !refs;
    if (allocated) {
      refs = calloc(a[1] ? a[1] : 1, sizeof(*refs));
      if (!refs) {
        *out = (uint32_t)-108;
        return 1;
      }
    }
    FSCatalogInfo *infos = a[5] ? calloc(a[1] ? a[1] : 1, sizeof(*infos)) : NULL;
    if (a[5] && !infos) {
      if (allocated)
        free(refs);
      *out = (uint32_t)-108;
      return 1;
    }
    int16_t status = F(int16_t, void *, ItemCount, ItemCount *, void *,
                       uint32_t, void *, void *, void *, void *)(
        unwrap(a[0]), a[1], &count, P(3), a[4], infos, refs, NULL, P(8));
    for (ItemCount i = 0; infos && i < count && i < a[1]; ++i)
      catalog_info_to_guest(infos + i, (char *)P(5) + i * kCatalogInfo32Size);
    free(infos);
    if (a[2])
      *(uint32_t *)P(2) = (uint32_t)count;
    if (a[7])
      for (ItemCount i = 0; i < count && i < a[1]; ++i) {
        int16_t s = spec_from_ref(refs + i, (struct spec32 *)P(7) + i);
        if (s && !status)
          status = s;
      }
    if (allocated)
      free(refs);
    *out = (uint32_t)(int32_t)status;
    return 1;
  }
  if (IS("_GetCurrentEventTime")) {
    *out = compat_runtime32_return_double(F(double, void)());
    return 1;
  }
  if (IS("_GetEventTime")) {
    *out = compat_runtime32_return_double(F(double, void *)(unwrap(a[0])));
    return 1;
  }
  if (IS("_RunCurrentEventLoop")) {
    double timeout;
    memcpy(&timeout, a, 8);
    *out = (uint32_t)F(int32_t, double)(timeout);
    return 1;
  }
  if (IS("_InstallEventLoopTimer")) {
    double delay, interval;
    memcpy(&delay, a + 1, 8);
    memcpy(&interval, a + 3, 8);
    struct handler32 *h = malloc(sizeof(*h));
    if (!h) {
      *out = (uint32_t)-108;
      return 1;
    }
    *h = (struct handler32){a[5], a[6]};
    void *timer = NULL;
    int32_t r = F(int32_t, void *, double, double, void *, void *, void **)(
        unwrap(a[0]), delay, interval, timer_callback, h, &timer);
    if (r)
      free(h);
    else {
      uint32_t token = wrap(timer);
      struct carbon_ref *ref = reference(token);
      if (ref)
        ref->callback = h;
      if (a[7])
        *(uint32_t *)P(7) = token;
    }
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_RemoveEventLoopTimer")) {
    struct carbon_ref *ref = reference(a[0]);
    int32_t r = F(int32_t, void *)(unwrap(a[0]));
    if (!r && ref) {
      free(ref->callback);
      ref->callback = NULL;
      ref->host = NULL;
    }
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_SetEventLoopTimerNextFireTime")) {
    double delay;
    memcpy(&delay, a + 1, 8);
    *out = (uint32_t)F(int32_t, void *, double)(unwrap(a[0]), delay);
    return 1;
  }
  if (IS("_GetApplicationEventTarget") || IS("_GetCurrentEventLoop") ||
      IS("_GetMainEventLoop")) {
    *out = wrap(F(void *, void)());
    return 1;
  }
  if (IS("_InstallEventHandler")) {
    struct handler32 *h = malloc(sizeof(*h));
    if (!h) {
      *out = (uint32_t)-108;
      return 1;
    }
    *h = (struct handler32){a[1], a[4]};
    void *handler = NULL;
    int32_t r =
        F(int32_t, void *, void *, uint32_t, const void *, void *,
          void **)(unwrap(a[0]), event_handler, a[2], P(3), h, &handler);
    if (r) {
      free(h);
    } else {
      uint32_t token = wrap(handler);
      struct carbon_ref *ref = reference(token);
      if (ref)
        ref->callback = h;
      if (a[5])
        *(uint32_t *)P(5) = token;
    }
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_RemoveEventHandler")) {
    struct carbon_ref *ref = reference(a[0]);
    int32_t r = F(int32_t, void *)(unwrap(a[0]));
    if (!r && ref) {
      free(ref->callback);
      ref->callback = NULL;
      ref->host = NULL;
    }
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_CallNextEventHandler")) {
    *out = (uint32_t)F(int32_t, void *, void *)(unwrap(a[0]), unwrap(a[1]));
    return 1;
  }
  if (IS("_GetEventClass") || IS("_GetEventKind")) {
    *out = F(uint32_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_SetEventParameter")) {
    uint32_t type = a[2];
    ByteCount size = a[3];
    const void *data = P(4);
    void *pointer = NULL;
    double numbers[4];
    if (data && size == 4 &&
        (type == typeWindowRef || type == typeControlRef ||
         type == typeEventRef || type == typeEventTargetRef ||
         type == typeHIObjectRef || type == typeCGContextRef ||
         type == typeCFStringRef || type == typeVoidPtr)) {
      uint32_t token;
      memcpy(&token, data, 4);
      pointer = type == typeVoidPtr ? (void *)(uintptr_t)token : unwrap(token);
      data = &pointer;
      size = sizeof(pointer);
    } else if (data && (type == typeHIPoint || type == typeHISize ||
                        type == typeHIRect)) {
      unsigned count = type == typeHIRect ? 4 : 2;
      if (size != count * sizeof(float)) {
        *out = (uint32_t)-50;
        return 1;
      }
      float values[4];
      memcpy(values, data, size);
      for (unsigned i = 0; i < count; ++i)
        numbers[i] = values[i];
      data = numbers;
      size = count * sizeof(double);
    }
    *out = (uint32_t)F(int32_t, void *, uint32_t, uint32_t, ByteCount,
                       const void *)(unwrap(a[0]), a[1], type, size, data);
    return 1;
  }
  if (IS("_GetEventParameter")) {
    uint32_t type = 0;
    ByteCount size = 0;
    unsigned char small[256];
    void *buffer = small;
    int32_t r = F(int32_t, void *, uint32_t, uint32_t, uint32_t *, ByteCount,
                  ByteCount *, void *)(unwrap(a[0]), a[1], a[2], &type,
                                       sizeof(small), &size, buffer);
    if (r == eventParameterNotFoundErr) {
      *out = (uint32_t)r;
      return 1;
    }
    if (size > sizeof(small) && size <= 1024 * 1024) {
      buffer = malloc(size);
      if (!buffer) {
        *out = (uint32_t)-108;
        return 1;
      }
      r = F(int32_t, void *, uint32_t, uint32_t, uint32_t *, ByteCount,
            ByteCount *,
            void *)(unwrap(a[0]), a[1], a[2], &type, size, &size, buffer);
    }
    uint32_t converted[8];
    if (!r && size == 8 &&
        (type == typeWindowRef || type == typeControlRef ||
         type == typeEventRef || type == typeEventTargetRef ||
         type == typeHIObjectRef || type == typeCGContextRef ||
         type == typeCFStringRef || type == typeVoidPtr)) {
      void *host;
      memcpy(&host, buffer, sizeof(host));
      converted[0] = type == typeCFStringRef || type == typeCGContextRef
                         ? objc_bridge32_guest_object(host)
                     : type == typeVoidPtr && (uintptr_t)host <= UINT32_MAX
                         ? (uint32_t)(uintptr_t)host
                         : wrap(host);
      if (buffer != small)
        free(buffer);
      buffer = small;
      memcpy(buffer, converted, 4);
      size = 4;
    } else if (!r && type == typeHICommand &&
               size == sizeof(HICommandExtended)) {
      HICommandExtended command;
      memcpy(&command, buffer, sizeof(command));
      /* Carbon records have two-byte packing on i386: the pointer and
         following MenuItemIndex make the guest record 14 bytes. */
      uint32_t words[] = {command.attributes, command.commandID,
                          wrap(command.source.control)};
      uint16_t item = command.source.menu.menuItemIndex;
      memcpy(buffer, words, sizeof(words));
      memcpy((uint8_t *)buffer + 12, &item, sizeof(item));
      size = 14;
    } else if (!r && (type == typeHIPoint || type == typeHISize ||
                      type == typeHIRect)) {
      unsigned count = type == typeHIRect ? 4 : 2;
      if (size == count * sizeof(double)) {
        double numbers[4];
        memcpy(numbers, buffer, size);
        float floats[4];
        for (unsigned i = 0; i < count; ++i)
          floats[i] = (float)numbers[i];
        memcpy(buffer, floats, count * 4);
        size = count * 4;
      }
    }
    if (a[3])
      *(uint32_t *)P(3) = type;
    if (a[5])
      *(uint32_t *)P(5) = (uint32_t)size;
    if (!r && a[6]) {
      if (a[4] < size)
        r = -9870;
      else
        memcpy(P(6), buffer, size);
    }
    if (buffer != small)
      free(buffer);
    *out = (uint32_t)r;
    return 1;
  }
  if (IS("_InstallStandardEventHandler")) {
    *out = (uint32_t)F(int32_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_RunAppModalLoopForWindow") || IS("_QuitAppModalLoopForWindow")) {
    if (IS("_RunAppModalLoopForWindow")) {
      *out = (uint32_t)carbon_ui_run_modal(unwrap(a[0]));
      return 1;
    }
    if (carbon_ui_stop_modal(unwrap(a[0]))) {
      *out = 0;
      return 1;
    }
    if (IS("_RunAppModalLoopForWindow") && getenv("LP32_TRACE_CARBON")) {
      Rect bounds = {0};
      ((int32_t (*)(void *, uint32_t, Rect *))symbol("_GetWindowBounds"))(
          unwrap(a[0]), 33, &bounds);
      fprintf(stderr, "compat32: modal bounds=%d,%d,%d,%d visible=%d\n",
              bounds.left, bounds.top, bounds.right, bounds.bottom,
              ((bool (*)(void *))symbol("_IsWindowVisible"))(unwrap(a[0])));
    }
    *out = (uint32_t)F(int32_t, void *)(unwrap(a[0]));
    return 1;
  }
  if (IS("_RunApplicationEventLoop") || IS("_QuitApplicationEventLoop")) {
    F(void, void)();
    *out = 0;
    return 1;
  }
  return 0;
#undef IS
#undef P
#undef F
}

int carbon_bridge32_has_symbol(const char *name) {
  if (name[0] == '_')
    ++name;
  return !strcmp(name, "CreateRoundButtonControl") ||
         !strcmp(name, "CreateEditUnicodeTextControl");
}

int carbon_bridge32_run_dispatch_self_test(void) {
  uint32_t args[4] = {0};
  uint64_t result = 0x1234;
  const char *foreign[] = {"_OSAtomicAdd32Barrier", "_CFAbsoluteTimeGetCurrent",
                           "_CGEventSourceKeyState", "_glBindBuffer"};
  unsigned before = symbol_lookups;
  uint64_t start = hitch_now();
  for (unsigned i = 0; i < 100000; ++i)
    if (carbon_bridge32_dispatch(foreign[i % 4], args, &result) || result != 0x1234)
      return -1;
  fprintf(stderr, "carbon-dispatch-selftest: foreign calls=100000 ns/call=%.0f lookups=%u\n",
          (hitch_now() - start) / 100000.0, symbol_lookups - before);
  if (symbol_lookups != before) return -1;
  /* Real native export, dynamic spelling, and repeated calls remain valid. */
  if (!carbon_bridge32_dispatch("GetCurrentEventTime", args, &result)) return -1;
  before = symbol_lookups;
  for (unsigned i = 0; i < 10000; ++i)
    if (!carbon_bridge32_dispatch("_GetCurrentEventTime", args, &result)) return -1;
  if (symbol_lookups != before) return -1;
  /* The hot atomic import used by Marvel must keep its 32-bit ABI and memory
     ordering when resolved through the runtime's cached callback route. */
  uint32_t cell = compat_runtime32_allocate(4, 1);
  uint32_t thunk = compat_runtime32_guest_callback("_OSAtomicAdd32Barrier");
  if (!cell || !thunk) return -1;
  args[0] = 1; args[1] = cell;
  start = hitch_now();
  for (unsigned i = 0; i < 100000; ++i)
    if (compat_runtime32_call(thunk, args, 2) != i + 1) return -1;
  fprintf(stderr, "carbon-dispatch-selftest: guest atomic ns/call=%.0f\n",
          (hitch_now() - start) / 100000.0);
  compat_runtime32_deallocate(cell);
  puts("carbon-dispatch-selftest: PASS (foreign imports, native cache, dynamic spelling, guest atomic callback)");
  return 0;
}

/* Exercise the packed classic API against a real temporary file. */
int carbon_bridge32_run_file_self_test(void) {
  char path[] = "/tmp/lp32-carbon-file-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) return -1;
  int passed = -1;
  uint32_t memory = compat_runtime32_allocate(256, 1);
  uint32_t a[8] = {0}; uint64_t out = 0;
  int16_t handle = 0;
  int16_t fork=0;
  void *completion=MAP_FAILED;
  if (!memory || write(fd, "abcdef", 6) != 6) goto done;
  close(fd); fd = -1;
  FSRef ref;
  if (((OSErr (*)(const UInt8 *, FSRef *, Boolean *))symbol("_FSPathMakeRef"))((UInt8 *)path, &ref, NULL) ||
      spec_from_ref(&ref, (void *)(uintptr_t)memory)) goto done;
  unsigned char *bytes = (void *)(uintptr_t)memory;
  memset(bytes+80, 0xcc, 8);
  a[0]=memory;a[1]=fsRdPerm;a[2]=memory+80;
  if (!carbon_bridge32_dispatch("_FSpOpenDF",a,&out) || out || bytes[82]!=0xcc) goto done;
  memcpy(&handle,bytes+80,2);
  a[0]=(uint16_t)handle;a[1]=memory+88;a[2]=memory+96;
  *(uint32_t *)(bytes+88)=4;*(uint32_t *)(bytes+92)=0x12345678;
  if (!carbon_bridge32_dispatch("_FSRead",a,&out) || out || memcmp(bytes+96,"abcd",4) ||
      *(uint32_t *)(bytes+88)!=4 || *(uint32_t *)(bytes+92)!=0x12345678) goto done;
  a[1]=memory+88;
  if (!carbon_bridge32_dispatch("_GetFPos",a,&out) || out || *(uint32_t *)(bytes+88)!=4) goto done;
  a[1]=fsFromStart;a[2]=2;
  if (!carbon_bridge32_dispatch("_SetFPos",a,&out) || out) goto done;
  a[1]=memory+88;a[2]=memory+96;*(uint32_t *)(bytes+88)=8;
  if (!carbon_bridge32_dispatch("_FSRead",a,&out) || (int32_t)out!=eofErr ||
      *(uint32_t *)(bytes+88)!=4 || memcmp(bytes+96,"cdef",4)) goto done;
  if (!carbon_bridge32_dispatch("_FSClose",a,&out) || out) goto done;
  handle=0;
  if (!carbon_bridge32_dispatch("_GetFPos",a,&out) || (int32_t)out!=rfNumErr) goto done;
  memcpy(bytes,&ref,sizeof(ref));memset(bytes+80,0xcc,8);
  a[0]=memory;a[1]=0;a[2]=0;a[3]=fsRdPerm;a[4]=memory+80;
  if(!carbon_bridge32_dispatch("_FSOpenFork",a,&out) || out || bytes[82]!=0xcc || bytes[83]!=0xcc)goto done;
  memcpy(&fork,bytes+80,2);
  /* A real i386 completion writes qType through its parameter-block argument. */
  const unsigned char code[]={0x8b,0x44,0x24,0x04,0x66,0xc7,0x40,0x04,0x34,0x12,0xc3};
  completion=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_32BIT,-1,0);
  if(completion==MAP_FAILED)goto done;
  memcpy(completion,code,sizeof(code));
  struct fork_io32 *io=(void *)(bytes+128);
  for(unsigned pass=0;pass<2;++pass) {
    memset(io,0,sizeof(*io));io->completion=(uint32_t)(uintptr_t)completion;
    io->fork=fork;io->buffer=memory+96;io->mode=fsFromStart;io->offset=pass?4:1;
    io->requested=pass?8:3;bytes[128+sizeof(*io)]=0xa5;a[0]=memory+128;
    if(!carbon_bridge32_dispatch("_PBReadForkAsync",a,&out) || out)goto done;
    for(unsigned tries=0;__atomic_load_n(&pending_fork_reads,__ATOMIC_ACQUIRE) && tries<5000;++tries)usleep(1000);
    if(__atomic_load_n(&pending_fork_reads,__ATOMIC_ACQUIRE))return -1;
    if(io->result!=(pass?eofErr:0) || io->actual!=(pass?2u:3u) || io->type!=0x1234 ||
       memcmp(bytes+96,pass?"ef":"bcd",io->actual) || bytes[128+sizeof(*io)]!=0xa5)goto done;
  }
  passed=0;
done:
  if(completion!=MAP_FAILED)munmap(completion,4096);
  if(fork){a[0]=(uint16_t)fork;carbon_bridge32_dispatch("_FSCloseFork",a,&out);}
  if(handle){a[0]=(uint16_t)handle;carbon_bridge32_dispatch("_FSClose",a,&out);}
  if(memory)compat_runtime32_deallocate(memory);
  if(fd>=0)close(fd);
  unlink(path);
  fprintf(stderr,"carbon-file-selftest: %s (open, read, seek, async completion, EOF, close, packed outputs)\n",passed?"FAIL":"PASS");
  return passed;
}

/* Uses hidden native windows to verify the guest coordinate contract. */
int carbon_bridge32_run_geometry_self_test(void) {
  uint32_t memory = compat_runtime32_allocate(128, 1);
  if (!memory)
    return -1;
  Rect *rect = (void *)(uintptr_t)memory;
  uint32_t *handles = (void *)(uintptr_t)(memory + 64);
  int result = -1;
  {
    uint64_t out;
    uint32_t args[] = {0, memory};
    uint32_t *guard = (void *)(uintptr_t)(memory + sizeof(Rect));
    *guard = 0x1234abcd;
    if (!carbon_bridge32_dispatch("_GetAvailableWindowPositioningBounds", args, &out) ||
        out || rect->right <= rect->left || rect->bottom <= rect->top || *guard != 0x1234abcd) goto done;
    Rect main_bounds = *rect;
    if (!carbon_bridge32_dispatch("_GetMainDevice", args, &out) || !out) goto done;
    args[0] = (uint32_t)out;
    if (!carbon_bridge32_dispatch("_GetAvailableWindowPositioningBounds", args, &out) ||
        out || memcmp(rect, &main_bounds, sizeof(Rect)) || *guard != 0x1234abcd) goto done;
    args[0] = UINT32_MAX;
    if (!carbon_bridge32_dispatch("_GetAvailableWindowPositioningBounds", args, &out) ||
        (int32_t)out != paramErr || memcmp(rect, &main_bounds, sizeof(Rect))) goto done;
  }
  for (unsigned composited = 0; composited < 2; ++composited) {
    uint64_t out = 0;
    uint32_t args[8] = {0};
    *rect = (Rect){200, 100, 680, 740};
    args[0] = 5;
    args[1] = composited ? kWindowCompositingAttribute : 0;
    args[2] = memory;
    args[3] = memory + 64;
    if (!carbon_bridge32_dispatch("_CreateNewWindow", args, &out) || out)
      goto done;
    *rect = (Rect){40, 50, 140, 250};
    args[0] = handles[0];
    args[1] = memory;
    args[2] = 0;
    args[3] = memory + 68;
    if (!carbon_bridge32_dispatch("_CreateUserPaneControl", args, &out) || out)
      goto dispose;
    *rect = (Rect){0, 0, 20, 40};
    args[3] = memory + 72;
    if (!carbon_bridge32_dispatch("_CreateUserPaneControl", args, &out) || out)
      goto dispose;
    args[0] = handles[1];
    args[1] = handles[2];
    if (!carbon_bridge32_dispatch("_HIViewAddSubview", args, &out) || out)
      goto dispose;
    *rect = (Rect){70, 80, 90, 120};
    args[0] = handles[2];
    args[1] = memory;
    if (!carbon_bridge32_dispatch("_SetControlBounds", args, &out))
      goto dispose;
    CGRect frame = {0};
    ((int32_t (*)(void *, CGRect *))symbol("_HIViewGetFrame"))(
        unwrap(handles[2]), &frame);
    if (frame.origin.x != (composited ? 80 : 30) ||
        frame.origin.y != (composited ? 70 : 30) || frame.size.width != 40 ||
        frame.size.height != 20)
      goto dispose;
    memset(rect, 0, sizeof(*rect));
    if (!carbon_bridge32_dispatch("_GetControlBounds", args, &out) ||
        rect->left != 80 || rect->top != 70 || rect->right != 120 ||
        rect->bottom != 90)
      goto dispose;
    args[0] = handles[0];
    if (!carbon_bridge32_dispatch("_GetWindowPortBounds", args, &out) ||
        rect->left || rect->top || rect->right != 640 || rect->bottom != 480)
      goto dispose;
    /* Dynamic lookups omit the Mach-O underscore. Version-one alert
       parameters occupy 28 bytes; the following word must remain untouched. */
    uint32_t *guard = (void *)(uintptr_t)(memory + 28);
    *guard = 0x1234abcd;
    args[0] = memory;
    args[1] = 1;
    if (!carbon_bridge32_dispatch("GetStandardAlertDefaultParams", args,
                                  &out) ||
        out || ((struct alert_params32 *)(uintptr_t)memory)->version != 1 ||
        *guard != 0x1234abcd)
      goto dispose;
    EventRef event = NULL;
    CreateEvent(NULL, kEventClassCommand, kEventCommandProcess,
                GetCurrentEventTime(), 0, &event);
    HICommandExtended command = {.attributes = kHICommandFromControl,
                                 .commandID = 'test'};
    command.source.control = unwrap(handles[2]);
    SetEventParameter(event, kEventParamDirectObject, typeHICommand,
                      sizeof(command), &command);
    uint32_t event_token = wrap(event);
    *guard = 0x1234abcd;
    uint32_t event_args[] = {
        event_token, kEventParamDirectObject, typeHICommand, 0, 14, memory + 32,
        memory};
    bool event_ok =
        carbon_bridge32_dispatch("_GetEventParameter", event_args, &out) &&
        !out && *(uint32_t *)(uintptr_t)(memory + 32) == 14 &&
        *(uint32_t *)(uintptr_t)(memory + 4) == 'test' &&
        *(uint32_t *)(uintptr_t)(memory + 8) == handles[2] &&
        *guard == 0x1234abcd;
    uint32_t retain_args[] = {event_token};
    bool retained =
        carbon_bridge32_dispatch("_RetainEvent", retain_args, &out) &&
        out == event_token;
    event_ok = event_ok && retained;
    ReleaseEvent(event);
    if (retained)
      ReleaseEvent(event);
    if (!event_ok)
      goto dispose;
    result = 0;
  dispose:
    args[0] = handles[0];
    carbon_bridge32_dispatch("_DisposeWindow", args, &out);
    if (result)
      goto done;
    if (!composited)
      result = -1;
  }
done:
  compat_runtime32_deallocate(memory);
  fprintf(stderr,
          "Carbon geometry %s (legacy port coordinates, composited parent "
          "coordinates, nested controls, local window bounds, alert ABI, "
          "command events)\n",
          result ? "FAIL" : "PASS");
  return result;
}
