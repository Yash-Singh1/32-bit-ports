#include "cfnetwork_bridge.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include <CFNetwork/CFNetwork.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Contexts retain guest tokens, never host-sized copies of guest callbacks. */
struct callback32 {
  uint32_t info, retain, release, describe, callback;
};
static const void *retain_context(const void *raw) {
  struct callback32 *copy = malloc(sizeof(*copy));
  if (!copy)
    return NULL;
  *copy = *(const struct callback32 *)raw;
  if (copy->retain)
    copy->info = compat_runtime32_call(copy->retain, &copy->info, 1);
  return copy;
}
static void release_context(const void *raw) {
  const struct callback32 *context = raw;
  if (context->release)
    compat_runtime32_call(context->release, &context->info, 1);
  free((void *)context);
}
static CFStringRef describe_context(const void *raw) {
  const struct callback32 *context = raw;
  if (!context->describe)
    return NULL;
  CFStringRef description = objc_bridge32_host_object(
      compat_runtime32_call(context->describe, &context->info, 1));
  return description ? CFRetain(description) : NULL;
}
static void *stream_retain(void *raw) { return (void *)retain_context(raw); }
static void stream_release(void *raw) { release_context(raw); }
static CFStringRef stream_describe(void *raw) { return describe_context(raw); }
static void stream_callback(CFReadStreamRef stream, CFStreamEventType event,
                            void *raw) {
  if (getenv("LP32_TRACE_NETWORK")) {
    CFStreamError error = CFReadStreamGetError(stream);
    fprintf(stderr,
            "compat32: HTTP stream event=%lu error-domain=%ld error=%d\n",
            (unsigned long)event, (long)error.domain, (int)error.error);
  }
  const struct callback32 *context = raw;
  uint32_t args[] = {objc_bridge32_guest_object((void *)stream),
                     (uint32_t)event, context->info};
  compat_runtime32_call(context->callback, args, 3);
}
static void timer_callback(CFRunLoopTimerRef timer, void *raw) {
  const struct callback32 *context = raw;
  uint32_t args[] = {objc_bridge32_guest_object((void *)timer), context->info};
  compat_runtime32_call(context->callback, args, 2);
}
static uint32_t copied(CFTypeRef value) {
  uint32_t token = objc_bridge32_guest_object((void *)value);
  if (value)
    CFRelease(value);
  return token;
}
/* CFIndex is 32 bits in the guest and 64 bits in the host CFStreamError. */
static void stream_error32(uint32_t address, CFStreamError error) {
  if (address) {
    int32_t fields[] = {(int32_t)error.domain, error.error};
    memcpy((void *)(uintptr_t)address, fields, sizeof(fields));
  }
}
struct source_context32 {
  struct callback32 common;
  uint32_t equal, hash, schedule, cancel;
};
static const void *source_retain(const void *raw) {
  struct source_context32 *copy = malloc(sizeof(*copy));
  if (!copy) return NULL;
  *copy = *(const struct source_context32 *)raw;
  if (copy->common.retain)
    copy->common.info = compat_runtime32_call(copy->common.retain, &copy->common.info, 1);
  return copy;
}
static Boolean source_equal(const void *left, const void *right) {
  const struct source_context32 *a = left, *b = right;
  if (a->equal != b->equal || a->hash != b->hash ||
      a->common.callback != b->common.callback) return false;
  if (!a->equal) return a->common.info == b->common.info;
  uint32_t args[] = {a->common.info, b->common.info};
  return compat_runtime32_call(a->equal, args, 2) != 0;
}
static CFHashCode source_hash(const void *raw) {
  const struct source_context32 *c = raw;
  return c->hash ? compat_runtime32_call(c->hash, &c->common.info, 1) : c->common.info;
}
static void source_schedule_callback(void *raw, CFRunLoopRef loop,
                                     CFStringRef mode, bool cancel) {
  const struct source_context32 *c = raw;
  uint32_t args[] = {c->common.info, objc_bridge32_guest_object(loop),
                     objc_bridge32_guest_object((void *)mode)};
  compat_runtime32_call(cancel ? c->cancel : c->schedule, args, 3);
}
static void source_schedule(void *raw, CFRunLoopRef loop, CFStringRef mode) {
  source_schedule_callback(raw, loop, mode, false);
}
static void source_cancel(void *raw, CFRunLoopRef loop, CFStringRef mode) {
  source_schedule_callback(raw, loop, mode, true);
}
static void source_perform(void *raw) {
  const struct source_context32 *c = raw;
  compat_runtime32_call(c->common.callback, &c->common.info, 1);
}
int cfnetwork_bridge32_dispatch(const char *name, const uint32_t *a,
                                uint64_t *out) {
#define IS(s) (!strcmp(name, s))
#define O(i) objc_bridge32_host_object(a[i])
#define P(i) ((void *)(uintptr_t)a[i])
  if (IS("_CFMakeCollectable")) {
    /* The compatibility runtime uses retain/release, never ObjC GC. */
    *out = a[0];
  } else if (IS("_CFHTTPMessageCreateRequest")) {
    if (getenv("LP32_TRACE_NETWORK")) {
      char host[256] = {0};
      CFStringRef name = CFURLCopyHostName(O(2));
      if (name) {
        CFStringGetCString(name, host, sizeof(host), kCFStringEncodingUTF8);
        CFRelease(name);
      }
      /* Never log URL paths/queries, headers, or bodies: they may contain keys.
       */
      fprintf(stderr, "compat32: HTTP request host=%s\n", host);
    }
    *out = copied(CFHTTPMessageCreateRequest(NULL, O(1), O(2), O(3)));
  } else if (IS("_CFHTTPMessageCreateEmpty"))
    *out = copied(CFHTTPMessageCreateEmpty(NULL, a[1] != 0));
  else if (IS("_CFHTTPMessageSetBody")) {
    CFHTTPMessageSetBody(O(0), O(1));
    *out = 0;
  } else if (IS("_CFHTTPMessageSetHeaderFieldValue")) {
    CFHTTPMessageSetHeaderFieldValue(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFHTTPMessageAppendBytes"))
    *out = CFHTTPMessageAppendBytes(O(0), P(1), a[2]);
  else if (IS("_CFHTTPMessageCopyAllHeaderFields"))
    *out = copied(CFHTTPMessageCopyAllHeaderFields(O(0)));
  else if (IS("_CFHTTPMessageCopyBody"))
    *out = copied(CFHTTPMessageCopyBody(O(0)));
  else if (IS("_CFHTTPMessageCopyHeaderFieldValue"))
    *out = copied(CFHTTPMessageCopyHeaderFieldValue(O(0), O(1)));
  else if (IS("_CFHTTPMessageCopyResponseStatusLine"))
    *out = copied(CFHTTPMessageCopyResponseStatusLine(O(0)));
  else if (IS("_CFHTTPMessageCopyVersion"))
    *out = copied(CFHTTPMessageCopyVersion(O(0)));
  else if (IS("_CFHTTPMessageIsHeaderComplete"))
    *out = CFHTTPMessageIsHeaderComplete(O(0));
  else if (IS("_CFHTTPAuthenticationCreateFromResponse"))
    *out = copied(CFHTTPAuthenticationCreateFromResponse(NULL, O(1)));
  else if (IS("_CFHTTPAuthenticationCopyMethod"))
    *out = copied(CFHTTPAuthenticationCopyMethod(O(0)));
  else if (IS("_CFHTTPAuthenticationCopyRealm"))
    *out = copied(CFHTTPAuthenticationCopyRealm(O(0)));
  else if (IS("_CFHTTPAuthenticationRequiresAccountDomain"))
    *out = CFHTTPAuthenticationRequiresAccountDomain(O(0));
  else if (IS("_CFHTTPAuthenticationRequiresUserNameAndPassword"))
    *out = CFHTTPAuthenticationRequiresUserNameAndPassword(O(0));
  else if (IS("_CFHTTPAuthenticationIsValid")) {
    CFStreamError error = {0};
    *out = CFHTTPAuthenticationIsValid(O(0), &error);
    stream_error32(a[1], error);
  } else if (IS("_CFHTTPMessageApplyCredentialDictionary")) {
    CFStreamError error = {0};
    *out = CFHTTPMessageApplyCredentialDictionary(O(0), O(1), O(2), &error);
    stream_error32(a[3], error);
  }
  else if (IS("_CFHTTPMessageGetResponseStatusCode")) {
    *out = (uint32_t)CFHTTPMessageGetResponseStatusCode(O(0));
    if (getenv("LP32_TRACE_NETWORK"))
      fprintf(stderr, "compat32: HTTP response status=%u\n", (uint32_t)*out);
  } else if (IS("_CFReadStreamCreateWithFile"))
    *out = copied(CFReadStreamCreateWithFile(NULL, O(1)));
  else if (IS("_CFWriteStreamCreateWithFile"))
    *out = copied(CFWriteStreamCreateWithFile(NULL, O(1)));
  else if (IS("_CFWriteStreamOpen"))
    *out = CFWriteStreamOpen(O(0));
  else if (IS("_CFWriteStreamClose")) {
    CFWriteStreamClose(O(0));
    *out = 0;
  } else if (IS("_CFPropertyListCreateWithStream")) {
    CFPropertyListFormat format = 0;
    CFErrorRef error = NULL;
    *out = copied(CFPropertyListCreateWithStream(NULL, O(1), (int32_t)a[2], a[3], &format, &error));
    if (a[4]) *(uint32_t *)P(4) = (uint32_t)format;
    uint32_t token = copied(error);
    if (a[5]) *(uint32_t *)P(5) = token;
  } else if (IS("_CFPropertyListWrite")) {
    CFErrorRef error = NULL;
    *out = (uint32_t)CFPropertyListWrite(O(0), O(1), a[2], a[3], &error);
    uint32_t token = copied(error);
    if (a[4]) *(uint32_t *)P(4) = token;
  } else if (IS("_CFReadStreamCreateForHTTPRequest"))
    *out = copied(CFReadStreamCreateForHTTPRequest(NULL, O(1)));
  else if (IS("_CFReadStreamCreateForStreamedHTTPRequest"))
    *out = copied(CFReadStreamCreateForStreamedHTTPRequest(NULL, O(1), O(2)));
  else if (IS("_CFReadStreamHasBytesAvailable"))
    *out = CFReadStreamHasBytesAvailable(O(0));
  else if (IS("_CFReadStreamCopyError"))
    *out = copied(CFReadStreamCopyError(O(0)));
  else if (IS("_CFReadStreamOpen"))
    *out = CFReadStreamOpen(O(0));
  else if (IS("_CFReadStreamClose")) {
    CFReadStreamClose(O(0));
    *out = 0;
  } else if (IS("_CFReadStreamRead"))
    *out = (uint32_t)CFReadStreamRead(O(0), P(1), (int32_t)a[2]);
  else if (IS("_CFReadStreamCopyProperty"))
    *out = copied(CFReadStreamCopyProperty(O(0), O(1)));
  else if (IS("_CFReadStreamSetProperty"))
    *out = CFReadStreamSetProperty(O(0), O(1), O(2));
  else if (IS("_CFReadStreamScheduleWithRunLoop")) {
    CFReadStreamScheduleWithRunLoop(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFReadStreamUnscheduleFromRunLoop")) {
    CFReadStreamUnscheduleFromRunLoop(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFReadStreamSetClient")) {
    const uint32_t *guest = P(3);
    if (!a[2] || !a[1]) {
      *out = CFReadStreamSetClient(O(0), 0, NULL, NULL);
      return 1;
    }
    if (!guest || guest[0]) {
      *out = 0;
      return 1;
    }
    struct callback32 callback = {guest[1], guest[2], guest[3], guest[4], a[2]};
    CFStreamClientContext context = {0, &callback, stream_retain,
                                     stream_release, stream_describe};
    *out = CFReadStreamSetClient(O(0), a[1], stream_callback, &context);
  } else if (IS("_SCDynamicStoreCopyProxies"))
    *out = copied(SCDynamicStoreCopyProxies(O(0)));
  else if (IS("_CFNetworkCopySystemProxySettings"))
    *out = copied(CFNetworkCopySystemProxySettings());
  else if (IS("_CFNetworkCopyProxiesForURL"))
    *out = copied(CFNetworkCopyProxiesForURL(O(0), O(1)));
  else if (IS("_CFNetworkCopyProxiesForAutoConfigurationScript")) {
    CFErrorRef error = NULL;
    *out = copied(CFNetworkCopyProxiesForAutoConfigurationScript(O(0), O(1), &error));
    if (a[2]) *(uint32_t *)P(2) = copied(error);
    else if (error) CFRelease(error);
  }
  else if (IS("_CFRunLoopGetMain"))
    *out = objc_bridge32_guest_object(CFRunLoopGetMain());
  else if (IS("_CFRunLoopGetCurrent"))
    *out = objc_bridge32_guest_object(CFRunLoopGetCurrent());
  else if (IS("_CFRunLoopRun")) {
    CFRunLoopRun();
    *out = 0;
  } else if (IS("_CFRunLoopStop")) {
    CFRunLoopStop(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopWakeUp")) {
    CFRunLoopWakeUp(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopAddSource")) {
    if (O(0) && O(1))
      CFRunLoopAddSource(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFRunLoopRemoveSource")) {
    if (O(0) && O(1))
      CFRunLoopRemoveSource(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFRunLoopContainsSource"))
    *out = O(0) && O(1) && CFRunLoopContainsSource(O(0), O(1), O(2));
  else if (IS("_CFRunLoopSourceCreate")) {
    const uint32_t *g = P(2);
    /* Version one contains a Mach port callback and has a different ABI. */
    if (!g || g[0]) { *out = 0; return 1; }
    struct source_context32 c = {{g[1], g[2], g[3], g[4], g[9]},
                                 g[5], g[6], g[7], g[8]};
    CFRunLoopSourceContext context = {0, &c, source_retain, release_context,
        g[4] ? describe_context : NULL, source_equal,
        source_hash, g[7] ? source_schedule : NULL,
        g[8] ? source_cancel : NULL, g[9] ? source_perform : NULL};
    *out = copied(CFRunLoopSourceCreate(NULL, (int32_t)a[1], &context));
  } else if (IS("_CFRunLoopSourceInvalidate")) {
    CFRunLoopSourceInvalidate(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopSourceSignal")) {
    CFRunLoopSourceSignal(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopAddTimer")) {
    CFRunLoopAddTimer(O(0), O(1), O(2));
    *out = 0;
  } else if (IS("_CFRunLoopTimerInvalidate")) {
    CFRunLoopTimerInvalidate(O(0));
    *out = 0;
  } else if (IS("_CFRunLoopTimerSetNextFireDate")) {
    double date;
    memcpy(&date, a + 1, 8);
    CFRunLoopTimerSetNextFireDate(O(0), date);
    *out = 0;
  } else if (IS("_CFRunLoopGetNextTimerFireDate")) {
    *out=compat_runtime32_return_double(CFRunLoopGetNextTimerFireDate(O(0),O(1)));
  } else if (IS("_CFRunLoopTimerGetContext")) {
    CFRunLoopTimerContext context; CFRunLoopTimerGetContext(O(0), &context);
    const struct callback32 *c = context.info;
    uint32_t guest[] = {0, c->info, c->retain, c->release, c->describe};
    memcpy(P(1), guest, sizeof(guest)); *out = 0;
  } else if (IS("_CFRunLoopRemoveTimer")) {
    CFRunLoopRemoveTimer(O(0), O(1), O(2)); *out = 0;
  } else if (IS("_CFRunLoopRunInMode")) {
    double seconds; memcpy(&seconds, a+1, 8);
    *out = CFRunLoopRunInMode(O(0), seconds, a[3] != 0);
  } else if (IS("_CFRunLoopTimerCreate")) {
    double date, interval;
    memcpy(&date, a + 1, 8);
    memcpy(&interval, a + 3, 8);
    const uint32_t *guest = P(8);
    if (guest && guest[0]) {
      *out = 0;
      return 1;
    }
    struct callback32 callback = {0, 0, 0, 0, a[7]};
    if (guest) {
      callback.info = guest[1];
      callback.retain = guest[2];
      callback.release = guest[3];
      callback.describe = guest[4];
    }
    CFRunLoopTimerContext context = {0, &callback, retain_context,
                                     release_context, describe_context};
    *out = copied(CFRunLoopTimerCreate(
        NULL, date, interval, a[5], (int32_t)a[6], timer_callback, &context));
  } else
    return 0;
  return 1;
}
uint32_t cfnetwork_bridge32_pointer_import(const char *name) {
  if (strcmp(name, "_kCFRunLoopCommonModes") == 0)
    return objc_bridge32_guest_object((void *)kCFRunLoopCommonModes);
  if (strncmp(name, "_kCFHTTP", 8) &&
      strncmp(name, "_kCFStreamPropertyHTTP", 22) &&
      strncmp(name, "_kCFProxy", 9))
    return 0;
  /* Restrict dynamic imports to CFNetwork's CFString constant families. */
  void **value = dlsym(RTLD_DEFAULT, name + 1);
  return value ? objc_bridge32_guest_object(*value) : 0;
}
